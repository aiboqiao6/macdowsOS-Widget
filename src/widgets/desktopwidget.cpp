#include "widgets/desktopwidget.h"
#include "app/appsettings.h"
#include "ui/widgetlibrarydialog.h"
#include "ui/liquidglassmenu.h"
#include "rendering/renderdiagnostics.h"
#include "widgets/musiccard.h"
#include "widgets/cardmetrics.h"
#include "rendering/cleartypetext.h"

#include <QApplication>
#include <QtConcurrent/QtConcurrentRun>
#include <QElapsedTimer>
#include <QAction>
#include <QDir>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QHash>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QScreen>
#include <QSet>
#include <QSysInfo>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QToolTip>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QUrl>
#include <QtMath>
#include <QSaveFile>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <utility>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <bluetoothapis.h>
#  include <bthdef.h>
#  include <functiondiscovery.h>
#  include <functiondiscoverycategories.h>
#  include <propkey.h>
#  include <propsys.h>
#endif

using namespace CardMetrics;

namespace {
int s_fontSmoothingLevel = 4;

void configureFontSmoothing(QFont& font, int level)
{
    switch (qBound(0, level, 4)) {
    case 0:
        font.setStyleStrategy(QFont::NoAntialias);
        font.setHintingPreference(QFont::PreferFullHinting);
        break;
    case 1:
        font.setStyleStrategy(static_cast<QFont::StyleStrategy>(
            QFont::PreferAntialias | QFont::NoSubpixelAntialias));
        font.setHintingPreference(QFont::PreferVerticalHinting);
        break;
    case 3:
        font.setStyleStrategy(static_cast<QFont::StyleStrategy>(
            QFont::PreferAntialias | QFont::PreferQuality | QFont::NoSubpixelAntialias));
        font.setHintingPreference(QFont::PreferFullHinting);
        break;
    case 2:
    default:
        font.setStyleStrategy(static_cast<QFont::StyleStrategy>(
            QFont::PreferAntialias | QFont::PreferQuality));
        font.setHintingPreference(QFont::PreferFullHinting);
        break;
    }
}

QString pingFangFamily()
{
    static const QString family = [] {
        const QStringList candidates = {
            QStringLiteral(":/fonts/pingfang0.ttf"),
            QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("pingfang0.ttf")),
            QDir::current().filePath(QStringLiteral("pingfang0.ttf"))
        };
        for (const QString& path : candidates) {
            if (!QFile::exists(path))
                continue;
            const int id = QFontDatabase::addApplicationFont(path);
            if (id < 0)
                continue;
            const QStringList families = QFontDatabase::applicationFontFamilies(id);
            if (!families.isEmpty())
                return families.first();
        }
        // 资源不可用时仍使用字体内嵌族名，由 Qt 查找已安装的同名字体。
        return QStringLiteral("PingFang SC");
    }();
    return family;
}

int defaultVariant(DesktopWidget::CardKind kind)
{
    switch (kind) {
    case DesktopWidget::CardKind::Battery: return 1;    // 横向状态。
    case DesktopWidget::CardKind::Weather: return 0;    // 紧凑预报。
    case DesktopWidget::CardKind::Clock: return 0;      // 浅色指针表盘。
    case DesktopWidget::CardKind::Dictionary: return 1; // 横向释义。
    case DesktopWidget::CardKind::Music: return 1;
    }
    return 0;
}

int variantCount(DesktopWidget::CardKind kind)
{
    return kind == DesktopWidget::CardKind::Clock ? 9
        : kind == DesktopWidget::CardKind::Music ? 2 : 3;
}

int normalizedVariant(DesktopWidget::CardKind kind, int variant)
{
    if (variant < 0)
        return defaultVariant(kind);
    return qBound(0, variant, variantCount(kind) - 1);
}

int gridColumnSpan(DesktopWidget::CardKind kind, int variant)
{
    variant = normalizedVariant(kind, variant);
    if (kind == DesktopWidget::CardKind::Clock)
        return variant == 7 || variant == 8 ? 2 : 1;
    if (kind == DesktopWidget::CardKind::Battery)
        return variant == 1 || variant == 2 ? 2 : 1;
    if (kind == DesktopWidget::CardKind::Weather)
        return variant == 1 || variant == 2 ? 2 : 1;
    return variant == 1 ? 2 : 1;
}

int gridRowSpan(DesktopWidget::CardKind kind, int variant)
{
    const int normalized = normalizedVariant(kind, variant);
    if (kind == DesktopWidget::CardKind::Clock)
        return 1;
    return ((kind == DesktopWidget::CardKind::Battery
             || kind == DesktopWidget::CardKind::Weather)
            && normalized == 2) ? 2 : 1;
}

int gridGapForArea(const QRect& area, qreal scale)
{
    if (!area.isValid())
        return qMax(4, qRound(kGridBaseGap * scale));

    const qreal shortestEdge = qreal(qMin(area.width(), area.height()));
    const qreal screenFactor = qBound<qreal>(0.72, shortestEdge / 1080.0, 1.65);
    return qBound(4, qRound(kGridBaseGap * scale * screenFactor), 28);
}

QSize gridWidgetSize(DesktopWidget::CardKind kind, int variant, int cell, int gap)
{
    const int span = gridColumnSpan(kind, variant);
    const int rows = gridRowSpan(kind, variant);
    return QSize(cell * span + gap * (span - 1),
                 cell * rows + gap * (rows - 1));
}

QPoint gridOrigin(const QRect& area, int gap)
{
    // 外侧留白与相邻网格间隙保持一致。
    return area.topLeft() + QPoint(gap, gap);
}

class GridSnapOverlay;
GridSnapOverlay* gridFadeOverlay();

class GridSnapOverlay final : public QWidget
{
public:
    explicit GridSnapOverlay(bool fadeLayer = false)
        : m_fadeLayer(fadeLayer)
    {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                       | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        m_opacityAnimation = new QPropertyAnimation(this, "windowOpacity", this);
        m_opacityAnimation->setEasingCurve(QEasingCurve::InOutCubic);
        connect(m_opacityAnimation, &QPropertyAnimation::finished, this, [this]() {
            if (m_hideAfterFade) {
                m_hideAfterFade = false;
                hide();
            }
        });
    }

    // 放置提示通过原生窗口透明度弱化填充，保持白色轮廓清晰。
    static constexpr qreal kOverlayOpacity = 0.68;

    void display(const QRect& target)
    {
        if (!target.isValid())
            return;

        m_hideAfterFade = false;
        m_opacityAnimation->stop();
        if (isVisible() && geometry() != target) {
            // 旧轮廓使用独立透明窗口淡出，新轮廓立即定位到目标位置。
            gridFadeOverlay()->fadeFrom(geometry(), windowOpacity());
        }

        setGeometry(target);
        setWindowOpacity(kOverlayOpacity);
        if (!isVisible())
            show();

#ifdef Q_OS_WIN
        // 放置提示不抢夺焦点，并紧贴拖动卡片下方。
        const HWND overlay = reinterpret_cast<HWND>(winId());
        if (QWidget* grabber = QWidget::mouseGrabber()) {
            const HWND dragged = reinterpret_cast<HWND>(grabber->window()->winId());
            SetWindowPos(overlay, dragged, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
#endif
        update();
    }

    void fadeFrom(const QRect& source, qreal sourceOpacity)
    {
        if (!m_fadeLayer || !source.isValid())
            return;
        m_hideAfterFade = false;
        m_opacityAnimation->stop();
        setGeometry(source);
        setWindowOpacity(qBound<qreal>(0.0, sourceOpacity, kOverlayOpacity));
        show();
#ifdef Q_OS_WIN
        const HWND overlay = reinterpret_cast<HWND>(winId());
        if (QWidget* grabber = QWidget::mouseGrabber()) {
            const HWND dragged = reinterpret_cast<HWND>(grabber->window()->winId());
            SetWindowPos(overlay, dragged, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
#endif
        animateOpacity(windowOpacity(), 0.0, 95);
    }

    void dismiss()
    {
        if (!isVisible() || m_hideAfterFade)
            return;
        m_hideAfterFade = true;
        animateOpacity(windowOpacity(), 0.0, 85);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal radius = qMin<qreal>(28.0, qMin(width(), height()) * .12);
        const QRectF bounds = QRectF(rect()).adjusted(3.5, 3.5, -3.5, -3.5);

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(255, 255, 255, 245), 4.0));
        painter.drawRoundedRect(bounds, radius, radius);
    }

private:
    void animateOpacity(qreal start, qreal end, int duration)
    {
        if (!m_opacityAnimation)
            return;
        m_opacityAnimation->stop();
        m_opacityAnimation->setDuration(qMax(1, duration));
        m_opacityAnimation->setStartValue(start);
        m_opacityAnimation->setEndValue(end);
        m_opacityAnimation->start();
    }

    QPropertyAnimation* m_opacityAnimation = nullptr;
    bool m_fadeLayer = false;
    bool m_hideAfterFade = false;
};

GridSnapOverlay* gridOverlay()
{
    static GridSnapOverlay* overlay = new GridSnapOverlay;
    return overlay;
}

GridSnapOverlay* gridFadeOverlay()
{
    static GridSnapOverlay* overlay = new GridSnapOverlay(true);
    return overlay;
}

#ifdef Q_OS_WIN
QString normalizedDeviceName(const QString& value)
{
    QString result;
    result.reserve(value.size());
    for (const QChar ch : value.toLower()) {
        if (ch.isLetterOrNumber())
            result.append(ch);
    }
    return result;
}

int batteryValue(const PROPVARIANT& value)
{
    switch (value.vt) {
    case VT_UI1: return value.bVal;
    case VT_I1:  return value.cVal;
    case VT_UI2: return value.uiVal;
    case VT_I2:  return value.iVal;
    case VT_UI4: return static_cast<int>(value.ulVal);
    case VT_I4:  return value.lVal;
    default:     return -1;
    }
}

QString stringValue(const PROPVARIANT& value)
{
    if (value.vt == VT_LPWSTR && value.pwszVal)
        return QString::fromWCharArray(value.pwszVal);
    if (value.vt == VT_BSTR && value.bstrVal)
        return QString::fromWCharArray(value.bstrVal);
    return {};
}

struct WindowsPeripheralProperty {
    QString name;
    QString category;
    int level = -1;
    bool connected = false;
};

QString stringListValue(const PROPVARIANT& value)
{
    if (value.vt == (VT_VECTOR | VT_LPWSTR)) {
        QStringList parts;
        for (ULONG i = 0; i < value.calpwstr.cElems; ++i) {
            if (value.calpwstr.pElems[i])
                parts.append(QString::fromWCharArray(value.calpwstr.pElems[i]));
        }
        return parts.join(QChar(' '));
    }
    return stringValue(value);
}

bool boolValue(const PROPVARIANT& value)
{
    return (value.vt == VT_BOOL && value.boolVal == VARIANT_TRUE)
           || (value.vt == VT_UI1 && value.bVal != 0)
           || (value.vt == VT_UI4 && value.ulVal != 0);
}

QVector<WindowsPeripheralProperty> windowsPeripheralProperties()
{
    QVector<WindowsPeripheralProperty> result;
    const HRESULT apartmentResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = apartmentResult == S_OK || apartmentResult == S_FALSE;

    IFunctionDiscovery* discovery = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(FunctionDiscovery), nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   __uuidof(IFunctionDiscovery),
                                   reinterpret_cast<void**>(&discovery)))) {
        IFunctionInstanceCollection* collection = nullptr;
        // 通过 Windows 设备发现读取驱动提供的 System.Devices.BatteryLife。
        if (SUCCEEDED(discovery->GetInstanceCollection(FCTN_CATEGORY_DEVICES,
                                                       nullptr, TRUE,
                                                       &collection))) {
            DWORD count = 0;
            if (SUCCEEDED(collection->GetCount(&count))) {
                count = qMin<DWORD>(count, 512);
                for (DWORD i = 0; i < count; ++i) {
                    IFunctionInstance* instance = nullptr;
                    if (FAILED(collection->Item(i, &instance)) || !instance)
                        continue;

                    IPropertyStore* properties = nullptr;
                    if (SUCCEEDED(instance->OpenPropertyStore(STGM_READ, &properties))) {
                        PROPVARIANT name{}, category{}, level{}, connected{};
                        PropVariantInit(&name);
                        PropVariantInit(&category);
                        PropVariantInit(&level);
                        PropVariantInit(&connected);
                        if (SUCCEEDED(properties->GetValue(PKEY_Devices_FriendlyName, &name))) {
                            WindowsPeripheralProperty device;
                            device.name = stringValue(name).trimmed();
                            if (SUCCEEDED(properties->GetValue(PKEY_Devices_Category, &category)))
                                device.category = stringListValue(category);
                            if (SUCCEEDED(properties->GetValue(PKEY_Devices_BatteryLife, &level))) {
                                const int percentage = batteryValue(level);
                                if (percentage >= 0 && percentage <= 100)
                                    device.level = percentage;
                            }
                            if (SUCCEEDED(properties->GetValue(PKEY_Devices_Connected, &connected)))
                                device.connected = boolValue(connected);
                            // 旧驱动未提供连接标志时仍保留电量记录，后续可与蓝牙枚举结果关联。
                            if (!device.name.isEmpty() && (device.connected || device.level >= 0))
                                result.append(device);
                        }
                        PropVariantClear(&connected);
                        PropVariantClear(&level);
                        PropVariantClear(&category);
                        PropVariantClear(&name);
                        properties->Release();
                    }
                    instance->Release();
                }
            }
            collection->Release();
        }
        discovery->Release();
    }

    if (uninitialize)
        CoUninitialize();
    return result;
}

int iconKindForBluetoothDevice(const QString& name, ULONG classOfDevice)
{
    const QString lowered = name.toLower();
    if (lowered.contains(QStringLiteral("mouse")) || lowered.contains(QStringLiteral("鼠标")))
        return 1;
    if (lowered.contains(QStringLiteral("keyboard")) || lowered.contains(QStringLiteral("键盘")))
        return 2;
    if (lowered.contains(QStringLiteral("head")) || lowered.contains(QStringLiteral("airpod"))
        || lowered.contains(QStringLiteral("buds")) || lowered.contains(QStringLiteral("耳机")))
        return 3;

    const ULONG major = GET_COD_MAJOR(classOfDevice);
    const ULONG minor = GET_COD_MINOR(classOfDevice);
    if (major == COD_MAJOR_PERIPHERAL) {
        if ((minor & COD_PERIPHERAL_MINOR_POINTER_MASK) != 0)
            return 1;
        if ((minor & COD_PERIPHERAL_MINOR_KEYBOARD_MASK) != 0)
            return 2;
    }
    if (major == COD_MAJOR_AUDIO)
        return 3;
    return 4;
}
#endif
}

QString DesktopWidget::pingFangFontFamily()
{
    const QString family = pingFangFamily();
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        QFont applicationFont = app->font();
        if (applicationFont.family() != family) {
            applicationFont.setFamily(family);
            configureFontSmoothing(applicationFont, s_fontSmoothingLevel);
            app->setFont(applicationFont);
        }
    }
    return family;
}

void DesktopWidget::setGlobalFontSmoothing(int level)
{
    const int next = qBound(0, level, 4);
    const bool changed = s_fontSmoothingLevel != next;
    s_fontSmoothingLevel = next;
    ClearTypeText::setEnabled(next == 4);
    AppSettings().setValue(QStringLiteral("appearance/fontSmoothing"), next);
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        QFont font = app->font();
        font.setFamily(pingFangFamily());
        configureFontSmoothing(font, next);
        app->setFont(font);
        if (changed)
            WidgetLibraryDialog::refreshFontRendering();
        for (QWidget* widget : app->allWidgets()) {
            // 应用字体更新后，显式刷新现有控件解析后的策略，避免 QSS 保留旧字体配置。
            QFont widgetFont = widget->font();
            configureFontSmoothing(widgetFont, next);
            widget->setFont(widgetFont);
            widget->update();
        }
    }
}

int DesktopWidget::globalFontSmoothing()
{
    return s_fontSmoothingLevel;
}

void DesktopWidget::applyFontSmoothing(QFont& font)
{
    configureFontSmoothing(font, s_fontSmoothingLevel);
}

QList<DesktopWidget*> DesktopWidget::s_instances;

DesktopWidget::DesktopWidget(QWidget* parent, CardKind kind, bool ownsTray,
                             const QString& instanceId, int variant, bool previewOnly)
    : LiquidGlassWidget(parent), m_cardKind(kind), m_ownsTray(ownsTray),
      m_cardVariant(normalizedVariant(kind, variant)),
      m_instanceId(instanceId.isEmpty()
                        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                        : instanceId)
{
    m_hostDeviceName = QSysInfo::machineHostName().trimmed();
    if (m_hostDeviceName.isEmpty())
        m_hostDeviceName = QStringLiteral("本机");
    s_instances.append(this);
    AppSettings settings;
    m_widgetScale = qBound<qreal>(0.40, settings.value(QStringLiteral("appearance/scale"), 1.0).toDouble(), 2.0);
    setGlassMargins(0);
    const int initialCell = qMax(1, qRound(kGridBaseCell * m_widgetScale));
    QScreen* initialScreen = QGuiApplication::primaryScreen();
    const int initialGap = initialScreen
        ? gridGapForArea(initialScreen->availableGeometry(), m_widgetScale)
        : qMax(4, qRound(kGridBaseGap * m_widgetScale));
    setFixedSize(gridWidgetSize(m_cardKind, m_cardVariant, initialCell, initialGap));
    setGlassRadius(34.0 * m_widgetScale);
    configureGlass(3.8f, 1.32f, 4.5f, 3, 0.008f);
    // 预览构造跳过网络、外设轮询和桌面捕获；音乐绘制复用共享会话。
    if (previewOnly) {
        setDesktopCaptureEnabled(false);
        setDesktopLayerEnabled(false);
        return;
    }
    // 时钟保持低频连续动画以插值秒针；静态卡片由数据变化触发绘制。
    setAnimationEnabled(m_cardKind == CardKind::Clock);
    const bool savedMouseThrough = settings.value(QStringLiteral("interaction/mouseThrough"), false).toBool();
    const bool savedLowPowerRefresh = settings.value(QStringLiteral("interaction/lowPowerRefresh"), false).toBool();
    const bool savedLiveBackdrop = settings.value(QStringLiteral("appearance/liveBackdrop"), true).toBool();
    const int savedFontSmoothing = qBound(0,
        settings.value(QStringLiteral("appearance/fontSmoothing"), 4).toInt(), 4);
    const int savedBlurStrength = settings.value(QStringLiteral("appearance/blurStrength"), 50).toInt();
    const qreal savedOpacity = settings.value(QStringLiteral("appearance/opacity"), 1.0).toDouble();
    const qreal savedRenderQuality = qBound<qreal>(0.5,
        settings.value(QStringLiteral("performance/renderScale"), 0.60).toDouble(), 1.5);
    setRenderScale(float(savedRenderQuality));
    setMaterialBlurStrength(savedBlurStrength);
    setMouseThroughEnabled(savedMouseThrough);
    setLowPowerRefreshEnabled(savedLowPowerRefresh);
    setLiveBackdropEnabled(savedLiveBackdrop);
    if (savedFontSmoothing != globalFontSmoothing())
        setGlobalFontSmoothing(savedFontSmoothing);
    setGlassOpacity(savedOpacity);
    setToolTip(m_cardKind == CardKind::Battery ? QStringLiteral("电量 · 拖动移动组件")
               : (m_cardKind == CardKind::Weather ? QStringLiteral("天气 · 拖动移动组件")
                  : (m_cardKind == CardKind::Clock ? QStringLiteral("时钟 · 拖动移动组件")
                     : (m_cardKind == CardKind::Music ? QStringLiteral("音乐 · 拖动移动组件")
                        : QStringLiteral("词典 · 拖动移动组件")))));

    if (m_cardKind == CardKind::Music) {
        connect(SystemMediaSession::instance(), &SystemMediaSession::changed,
                this, qOverload<>(&DesktopWidget::update));
    }

    if (m_cardKind == CardKind::Battery) {
        connect(&m_batteryTimer, &QTimer::timeout, this, &DesktopWidget::refreshBattery);
        m_batteryTimer.start(2000);
        connect(&m_deviceWatcher, &QFutureWatcher<QVector<ConnectedDevice>>::finished, this, [this]() {
            m_connectedDevices = m_deviceWatcher.result();
            update();
        });
    }

    m_weatherLocation = settings.value(QStringLiteral("weather/location"),
                                       QStringLiteral("北京")).toString().trimmed();
    m_weatherCityId = settings.value(QStringLiteral("weather/cityId"),
                                     QStringLiteral("101010100")).toString().trimmed();
    if (m_weatherLocation.isEmpty() || m_weatherLocation.compare(
            QStringLiteral("Beijing"), Qt::CaseInsensitive) == 0)
        m_weatherLocation = QStringLiteral("北京");
    if (m_weatherCityId.isEmpty())
        m_weatherCityId = QStringLiteral("101010100");
    if (m_cardKind == CardKind::Weather) {
        m_weatherNetwork = new QNetworkAccessManager(this);
        connect(m_weatherNetwork, &QNetworkAccessManager::finished,
                this, &DesktopWidget::handleWeatherReply);
        m_weatherTimer.setTimerType(Qt::VeryCoarseTimer);
        m_weatherTimer.setInterval(10 * 60 * 1000);
        connect(&m_weatherTimer, &QTimer::timeout, this, &DesktopWidget::refreshWeather);
        m_weatherTimer.start();
        refreshWeather();
    }
    if (m_cardKind == CardKind::Dictionary) {
        m_dictionaryNetwork = new QNetworkAccessManager(this);
        connect(m_dictionaryNetwork, &QNetworkAccessManager::finished,
                this, &DesktopWidget::handleDictionaryReply);
        m_dictionaryTimer.setTimerType(Qt::VeryCoarseTimer);
        m_dictionaryTimer.setInterval(30 * 60 * 1000);
        connect(&m_dictionaryTimer, &QTimer::timeout,
                this, &DesktopWidget::refreshDictionary);
        m_dictionaryTimer.start();
        refreshDictionary();
    }

    // 设置窗口立即向所有组件推送变更，定时读取负责同步外部修改。
    m_settingsTimer.setTimerType(Qt::VeryCoarseTimer);
    // 外部配置五秒同步一次，避免重复读取 JSON；正常设置变更即时推送。
    m_settingsTimer.setInterval(5000);
    connect(&m_settingsTimer, &QTimer::timeout, this, [this]() {
        AppSettings current;
        const QString savedLocation = current.value(QStringLiteral("weather/location"),
                                                    QStringLiteral("北京")).toString().trimmed();
        const QString savedCityId = current.value(QStringLiteral("weather/cityId"),
                                                   QStringLiteral("101010100")).toString().trimmed();
        if (m_cardKind == CardKind::Weather && !savedLocation.isEmpty()
            && (!savedCityId.isEmpty() && savedCityId != m_weatherCityId
                || savedLocation != m_weatherLocation)) {
            m_weatherLocation = savedLocation;
            m_weatherCityId = savedCityId;
            refreshWeather();
        }
        const qreal scale = qBound<qreal>(0.40, current.value(QStringLiteral("appearance/scale"), 1.0).toDouble(), 2.0);
        if (!qFuzzyCompare(scale, m_widgetScale))
            applyScale(scale);
        const int blurStrength = qBound(0, current.value(QStringLiteral("appearance/blurStrength"), 50).toInt(), 100);
        if (blurStrength != materialBlurStrength())
            setMaterialBlurStrength(blurStrength);
        const qreal opacity = qBound<qreal>(0.05, current.value(QStringLiteral("appearance/opacity"), 1.0).toDouble(), 1.0);
        if (!qFuzzyCompare(opacity, glassOpacity()))
            setGlassOpacity(opacity);
        const bool through = current.value(QStringLiteral("interaction/mouseThrough"), false).toBool();
        if (through != mouseThroughEnabled())
            setMouseThroughEnabled(through);
        const bool lowPower = current.value(QStringLiteral("interaction/lowPowerRefresh"), false).toBool();
        if (lowPower != lowPowerRefreshEnabled())
            setLowPowerRefreshEnabled(lowPower);
        const bool liveBackdrop = current.value(QStringLiteral("appearance/liveBackdrop"), true).toBool();
        if (liveBackdrop != liveBackdropEnabled())
            setLiveBackdropEnabled(liveBackdrop);
        const int fontSmoothing = qBound(0,
            current.value(QStringLiteral("appearance/fontSmoothing"), 4).toInt(), 4);
        if (fontSmoothing != globalFontSmoothing())
            setGlobalFontSmoothing(fontSmoothing);
        const qreal renderQuality = qBound<qreal>(0.5,
            current.value(QStringLiteral("performance/renderScale"), 0.60).toDouble(), 1.5);
        if (qAbs(renderQuality - qreal(renderScale())) > 0.001)
            setRenderScale(float(renderQuality));
    });
    m_settingsTimer.start();

    if (!m_hasSavedPosition)
        moveToGroupPosition();
    captureDesktopBackdrop();
    if (m_cardKind == CardKind::Battery)
        refreshBattery();
    if (m_ownsTray)
        setupTrayIcon();
}

DesktopWidget::~DesktopWidget()
{
    NativeWindows::restoreApplicationWindows(m_dragHiddenWindows);
    m_dragHiddenWindows.clear();
    NativeWindows::restoreApplicationWindows(m_libraryDragHiddenWindows);
    m_libraryDragHiddenWindows.clear();
    delete m_library;
    s_instances.removeAll(this);
}

void DesktopWidget::shutdown()
{
    const auto widgets = s_instances;
    qDeleteAll(widgets);
}

void DesktopWidget::showEvent(QShowEvent* event)
{
    LiquidGlassWidget::showEvent(event);
    // 恢复时其他组件可能仍隐藏，逐个显示时重新处理重叠。
    if (overlapsVisibleWidget(geometry(), this)) {
        const QRect target = nearbyWidgetRect(m_cardKind, m_cardVariant, pos(), m_widgetScale, this);
        if (target.isValid()) {
            m_restoringPosition = true;
            setFixedSize(target.size());
            move(target.topLeft());
            m_restoringPosition = false;
            captureDesktopBackdrop();
        }
    }
    for (DesktopWidget* widget : s_instances)
        widget->updateTrayVisibilityLabel();
}

void DesktopWidget::hideEvent(QHideEvent* event)
{
    LiquidGlassWidget::hideEvent(event);
    for (DesktopWidget* widget : s_instances)
        widget->updateTrayVisibilityLabel();
}

QIcon DesktopWidget::createTrayIcon() const
{
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawBatteryIcon(painter, QRectF(3, 7, 26, 18),
                    m_hasBattery ? m_batteryLevel : -1, m_charging);
    return QIcon(pixmap);
}

void DesktopWidget::setupTrayIcon()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    auto* menu = new QMenu(this);
    menu->setStyleSheet(QStringLiteral(
        "QMenu { background:#1d2333; color:#f5f7ff; border:1px solid #4b5878; "
        "border-radius:8px; padding:5px; }"
        "QMenu::item { padding:6px 28px 6px 12px; border-radius:5px; }"
        "QMenu::item:selected { background:#36528a; }"
        "QMenu::separator { height:1px; background:#3a4560; margin:4px 8px; }"));

    m_visibilityAction = menu->addAction(QString());
    connect(m_visibilityAction, &QAction::triggered, this, &DesktopWidget::toggleWidgetVisibility);

    auto* libraryAction = menu->addAction(QStringLiteral("添加小组件…"));
    connect(libraryAction, &QAction::triggered, this, &DesktopWidget::showWidgetLibrary);

    m_rearrangeAction = menu->addAction(QString());
    connect(m_rearrangeAction, &QAction::triggered, this, &DesktopWidget::rearrangeWidgets);

    auto* settingsAction = menu->addAction(QStringLiteral("设置…"));
    connect(settingsAction, &QAction::triggered, this, &DesktopWidget::showSettingsDialog);

    m_startupAction = menu->addAction(QStringLiteral("开机自启动"));
    m_startupAction->setCheckable(true);
    m_startupAction->setChecked(isStartupEnabled());
    connect(m_startupAction, &QAction::toggled, this, &DesktopWidget::setStartupEnabled);

    menu->addSeparator();
    auto* logsAction = menu->addAction(QStringLiteral("打开渲染日志文件夹"));
    logsAction->setEnabled(!RenderDiagnostics::logPath().isEmpty());
    connect(logsAction, &QAction::triggered, this, []() {
        const QString path = RenderDiagnostics::logPath();
        if (!path.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    });
    auto* aboutAction = menu->addAction(QStringLiteral("关于 macdowsOS Widget"));
    connect(aboutAction, &QAction::triggered, this, &DesktopWidget::showAboutDialog);
    menu->addSeparator();
    auto* quitAction = menu->addAction(QStringLiteral("退出"));
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setIcon(createTrayIcon());
    m_trayIcon->setToolTip(m_hasBattery
                               ? QStringLiteral("macdowsOS Widget  ·  %1%").arg(m_batteryLevel)
                               : QStringLiteral("macdowsOS Widget  ·  电源适配器"));
    m_trayIcon->setContextMenu(menu);
    m_trayClickTimer.setSingleShot(true);
    m_trayClickTimer.setInterval(QApplication::doubleClickInterval());
    connect(&m_trayClickTimer, &QTimer::timeout, this, &DesktopWidget::toggleWidgetVisibility);
    connect(m_trayIcon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger)
                    m_trayClickTimer.start();
                else if (reason == QSystemTrayIcon::DoubleClick) {
                    m_trayClickTimer.stop();
                    toggleWidgetVisibility();
                }
            });
    m_trayIcon->show();
    updateTrayVisibilityLabel();
    updateRearrangeLabel();
}

void DesktopWidget::updateTrayVisibilityLabel()
{
    if (!m_visibilityAction)
        return;
    bool anyVisible = false;
    for (DesktopWidget* widget : s_instances)
        anyVisible = anyVisible || widget->isVisible();
    m_visibilityAction->setText(anyVisible ? QStringLiteral("隐藏所有组件")
                                           : QStringLiteral("显示所有组件"));
}

void DesktopWidget::toggleWidgetVisibility()
{
    bool anyVisible = false;
    for (DesktopWidget* widget : s_instances)
        anyVisible = anyVisible || widget->isVisible();
    for (DesktopWidget* widget : s_instances) {
        if (anyVisible) {
            widget->hide();
        } else {
            widget->show();
            widget->captureDesktopBackdrop();
        }
    }
    updateTrayVisibilityLabel();
    saveAllConfigurations();
}

void DesktopWidget::updateRearrangeLabel()
{
    if (m_rearrangeAction)
        m_rearrangeAction->setText(QStringLiteral("重新排列组件"));
}

void DesktopWidget::rearrangeWidgets()
{
    arrangeGroup();
    saveAllConfigurations();
    updateRearrangeLabel();
}

void DesktopWidget::applyScale(qreal scale)
{
    m_widgetScale = qBound<qreal>(0.40, scale, 2.0);
    AppSettings settings;
    settings.setValue(QStringLiteral("appearance/scale"), m_widgetScale);
    setGlassRadius(34.0 * m_widgetScale);
    const int cell = qMax(1, qRound(kGridBaseCell * m_widgetScale));
    QScreen* currentScreen = QGuiApplication::screenAt(geometry().center());
    if (!currentScreen)
        currentScreen = QGuiApplication::primaryScreen();
    const int gap = currentScreen
        ? gridGapForArea(currentScreen->availableGeometry(), m_widgetScale)
        : qMax(4, qRound(kGridBaseGap * m_widgetScale));
    setFixedSize(gridWidgetSize(m_cardKind, m_cardVariant, cell, gap));
    if (isVisible() && overlapsVisibleWidget(geometry(), this)) {
        const QRect target = nearbyWidgetRect(m_cardKind, m_cardVariant, pos(), m_widgetScale, this);
        if (target.isValid()) {
            m_restoringPosition = true;
            setFixedSize(target.size());
            move(target.topLeft());
            m_restoringPosition = false;
        }
    }
    updateCardGeometry();
    if (m_library)
        m_library->setInterfaceScale(m_widgetScale);
    captureDesktopBackdrop();
    update();
}

void DesktopWidget::updateCardGeometry()
{
    // 每个窗口仅包含一个完整玻璃对象；基类管理圆角与背景，本类负责选定卡片内容。
    setGlassObjectVisible(0, true);
    setGlassObjectGeometry(0, QPointF(0, 0), QSizeF(width(), height()));
    setGlassObjectCornerRadius(0, float(34.0 * m_widgetScale));
}

void DesktopWidget::moveToGroupPosition()
{
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;
    const QRect area = screen->availableGeometry();
    const int cell = qMax(1, qRound(kGridBaseCell * m_widgetScale));
    const int gap = gridGapForArea(area, m_widgetScale);
    const int pitch = cell + gap;
    // 扣除两侧外边距后计算网格容量，同时保留末端一份间隙。
    const int columns = qMax(1, (area.width() - gap) / pitch);
    const int rows = qMax(1, (area.height() - gap) / pitch);
    const int span = gridColumnSpan(m_cardKind, m_cardVariant);
    int column = qMax(0, columns - span);
    const int rowSpan = gridRowSpan(m_cardKind, m_cardVariant);
    int row = qMax(0, rows - rowSpan);
    switch (m_cardKind) {
    case CardKind::Battery:
        row = m_cardVariant == 1 ? qMax(0, rows - 1) : qMax(0, rows - rowSpan); break;
    case CardKind::Weather:
        row = qMax(0, rows - rowSpan); break;
    case CardKind::Clock:
        row = qMax(0, rows - rowSpan); break;
    case CardKind::Dictionary:
    case CardKind::Music:
        row = qMax(0, rows - 1); break;
    }
    const QRect target = nearestGridRect(m_cardKind, m_cardVariant,
        gridOrigin(area, gap) + QPoint(column * pitch, row * pitch), m_widgetScale, this);
    if (target.isValid()) {
        m_restoringPosition = true;
        setFixedSize(target.size());
        move(target.topLeft());
        m_restoringPosition = false;
    }
}

void DesktopWidget::arrangeGroup()
{
    // 先腾空旧位置，避免组件自身的旧布局阻挡重新排列。
    int offset = 0;
    QHash<DesktopWidget*, QPoint> previous;
    for (DesktopWidget* widget : s_instances) {
        previous.insert(widget, widget->pos());
        widget->m_restoringPosition = true;
        widget->move(-100000 - offset, -100000 - offset);
        widget->m_restoringPosition = false;
        offset += 8;
    }
    for (DesktopWidget* widget : s_instances) {
        widget->moveToGroupPosition();
        if (!QGuiApplication::screenAt(widget->geometry().center())) {
            // 网格已满时恢复原位置，不能将卡片留在屏幕外的暂存坐标。
            for (DesktopWidget* restore : s_instances)
                restore->move(previous.value(restore));
            QToolTip::showText(QCursor::pos(), QStringLiteral("当前屏幕空间不足，请缩小组件后重新排列"));
            break;
        }
        widget->captureDesktopBackdrop();
    }
}

void DesktopWidget::syncGroupSettings(qreal scale, int blurStrength,
                                      qreal opacity, bool mouseThrough, bool lowPower,
                                      bool liveBackdrop, qreal renderQuality, int fontSmoothing)
{
    AppSettings settings;
    settings.setValue(QStringLiteral("performance/renderScale"), renderQuality);
    settings.setValue(QStringLiteral("appearance/liveBackdrop"), liveBackdrop);
    settings.setValue(QStringLiteral("appearance/fontSmoothing"), fontSmoothing);
    settings.sync();
    setGlobalFontSmoothing(fontSmoothing);
    // 尺寸变化尽量保留自由放置位置，仅显式重新排列时布局整个网格。
    bool scaleChanged = false;
    for (DesktopWidget* widget : s_instances) {
        if (widget && !qFuzzyCompare(widget->m_widgetScale, scale)) {
            scaleChanged = true;
            break;
        }
    }
    for (DesktopWidget* widget : s_instances) {
        if (scaleChanged)
            widget->applyScale(scale);
        widget->setMaterialBlurStrength(blurStrength);
        widget->setGlassOpacity(opacity);
        widget->setMouseThroughEnabled(mouseThrough);
        widget->setLowPowerRefreshEnabled(lowPower);
        widget->setLiveBackdropEnabled(liveBackdrop);
        widget->setRenderScale(float(renderQuality));
        if (widget->m_library) {
            widget->m_library->setInterfaceScale(scale);
            widget->m_library->setLiveBackdropEnabled(liveBackdrop);
            widget->m_library->setRenderScale(float(renderQuality));
            widget->m_library->setMaterialBlurStrength(blurStrength);
            widget->m_library->setGlassOpacity(opacity);
        }
    }
    saveAllConfigurations();
}

QString DesktopWidget::kindName(CardKind kind)
{
    switch (kind) {
    case CardKind::Battery: return QStringLiteral("battery");
    case CardKind::Weather: return QStringLiteral("weather");
    case CardKind::Clock: return QStringLiteral("clock");
    case CardKind::Dictionary: return QStringLiteral("dictionary");
    case CardKind::Music: return QStringLiteral("music");
    }
    return QStringLiteral("battery");
}

DesktopWidget::CardKind DesktopWidget::kindFromName(const QString& value)
{
    if (value == QStringLiteral("weather")) return CardKind::Weather;
    if (value == QStringLiteral("clock")) return CardKind::Clock;
    if (value == QStringLiteral("dictionary")) return CardKind::Dictionary;
    if (value == QStringLiteral("music")) return CardKind::Music;
    return CardKind::Battery;
}

QString DesktopWidget::configurationPath()
{
    const QString path = AppSettings::filePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    return path;
}

void DesktopWidget::saveConfiguration() const
{
    saveAllConfigurations();
}

void DesktopWidget::saveAllConfigurations()
{
    QJsonArray widgets;
    for (DesktopWidget* widget : s_instances) {
        if (!widget)
            continue;
        QJsonObject item;
        item.insert(QStringLiteral("id"), widget->m_instanceId);
        item.insert(QStringLiteral("type"), kindName(widget->m_cardKind));
        item.insert(QStringLiteral("variant"), widget->m_cardVariant);
        item.insert(QStringLiteral("x"), widget->x());
        item.insert(QStringLiteral("y"), widget->y());
        item.insert(QStringLiteral("visible"), widget->isVisible());
        widgets.append(item);
    }
    const QString path = configurationPath();
    QJsonObject root;
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(existing.readAll()).object();
        existing.close();
    }
    root.insert(QStringLiteral("version"), 4);
    root.insert(QStringLiteral("scale"), s_instances.isEmpty() ? 1.0 : s_instances.first()->m_widgetScale);
    root.insert(QStringLiteral("widgets"), widgets);
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.commit();
    }
}

QRect DesktopWidget::nearestGridRect(CardKind kind, int variant,
                                     const QPoint& requestedTopLeft, qreal scale,
                                     const DesktopWidget* ignored)
{
    const int cell = qMax(1, qRound(kGridBaseCell * scale));
    const int columnSpan = gridColumnSpan(kind, variant);
    const QSize roughSize(cell * columnSpan, cell);
    QScreen* screen = QGuiApplication::screenAt(
        requestedTopLeft + QPoint(roughSize.width() / 2, roughSize.height() / 2));
    if (!screen)
        screen = QGuiApplication::screenAt(requestedTopLeft);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return QRect(requestedTopLeft, roughSize);

    const QRect area = screen->availableGeometry();
    const int gap = gridGapForArea(area, scale);
    const QSize widgetSize = gridWidgetSize(kind, variant, cell, gap);
    const int pitch = cell + gap;
    // 所有外边缘都预留一份网格间隙，候选矩形的右侧与底部也必须满足。
    const int innerWidth = area.width() - gap * 2;
    const int innerHeight = area.height() - gap * 2;
    if (innerWidth < widgetSize.width() || innerHeight < widgetSize.height())
        return {};
    const int maxColumn = qMax(0, (innerWidth - widgetSize.width()) / pitch);
    const int maxRow = qMax(0, (innerHeight - widgetSize.height()) / pitch);
    const QPoint origin = gridOrigin(area, gap);
    QRect best;
    qint64 bestDistance = (std::numeric_limits<qint64>::max)();

    for (int row = 0; row <= maxRow; ++row) {
        for (int column = 0; column <= maxColumn; ++column) {
            const QRect candidate(origin.x() + column * pitch,
                                  origin.y() + row * pitch,
                                  widgetSize.width(), widgetSize.height());
            bool occupied = false;
            for (DesktopWidget* widget : s_instances) {
                // 隐藏卡片保留恢复位置，但不占用可见桌面网格。
                if (!widget || widget == ignored || !widget->isVisible())
                    continue;
                if (candidate.intersects(widget->geometry())) {
                    occupied = true;
                    break;
                }
            }
            if (occupied)
                continue;

            const qint64 dx = qint64(candidate.left()) - requestedTopLeft.x();
            const qint64 dy = qint64(candidate.top()) - requestedTopLeft.y();
            const qint64 distance = dx * dx + dy * dy;
            if (distance < bestDistance) {
                bestDistance = distance;
                best = candidate;
            }
        }
    }

    if (!best.isNull())
        return best;

    // 无可用位置时返回无效矩形，拖动退回原位，图库拒绝创建。
    return QRect();
}

QRect DesktopWidget::nearbyWidgetRect(CardKind kind, int variant,
                                      const QPoint& requestedTopLeft, qreal scale,
                                      const DesktopWidget* ignored)
{
    const int cell = qMax(1, qRound(kGridBaseCell * scale));
    QScreen* screen = QGuiApplication::screenAt(requestedTopLeft);
    if (!screen)
        screen = QGuiApplication::screenAt(requestedTopLeft + QPoint(cell / 2, cell / 2));
    if (!screen)
        return {};

    const QRect area = screen->availableGeometry();
    const int gap = gridGapForArea(area, scale);
    const QSize size = gridWidgetSize(kind, variant, cell, gap);
    const QRect requested(requestedTopLeft, size);
    const bool overlapping = overlapsVisibleWidget(requested, ignored);
    // 检测整条边与四角附近的吸附位置，提示轮廓保持固定对齐槽位。
    const int threshold = qMax(96, qRound(180 * scale));
    qint64 bestDistance = (std::numeric_limits<qint64>::max)();
    QRect best;

    for (DesktopWidget* neighbor : s_instances) {
        if (!neighbor || neighbor == ignored || !neighbor->isVisible()
            || QGuiApplication::screenAt(neighbor->geometry().center()) != screen)
            continue;
        const QRect beside = neighbor->geometry();
        const int verticalOverlap = qMax(0, qMin(requested.bottom(), beside.bottom())
                                          - qMax(requested.top(), beside.top()) + 1);
        const int horizontalOverlap = qMax(0, qMin(requested.right(), beside.right())
                                            - qMax(requested.left(), beside.left()) + 1);
        const bool besideEdge = verticalOverlap >= qMax(1, qMin(size.height(), beside.height()) / 4);
        const bool aboveOrBelow = horizontalOverlap >= qMax(1, qMin(size.width(), beside.width()) / 4);
        const QPoint candidates[] = {
            {beside.left() - gap - size.width(), beside.top()},
            {beside.left() - gap - size.width(), beside.bottom() - size.height() + 1},
            {beside.right() + gap + 1, beside.top()},
            {beside.right() + gap + 1, beside.bottom() - size.height() + 1},
            {beside.left(), beside.top() - gap - size.height()},
            {beside.right() - size.width() + 1, beside.top() - gap - size.height()},
            {beside.left(), beside.bottom() + gap + 1},
            {beside.right() - size.width() + 1, beside.bottom() + gap + 1},
            {beside.left() - gap - size.width(), beside.top() - gap - size.height()},
            {beside.right() + gap + 1, beside.top() - gap - size.height()},
            {beside.left() - gap - size.width(), beside.bottom() + gap + 1},
            {beside.right() + gap + 1, beside.bottom() + gap + 1}
        };
        for (int index = 0; index < 12; ++index) {
            const QPoint position = candidates[index];
            if (!overlapping) {
                const int dx = qAbs(position.x() - requestedTopLeft.x());
                const int dy = qAbs(position.y() - requestedTopLeft.y());
                if (index < 4) {
                    if (!besideEdge || dx > threshold)
                        continue;
                } else if (index < 8) {
                    if (!aboveOrBelow || dy > threshold)
                        continue;
                } else if (besideEdge || aboveOrBelow
                           || dx > threshold || dy > threshold) {
                    continue;
                }
            }
            const QRect candidate(position, size);
            if (!area.contains(candidate))
                continue;
            if (overlapsVisibleWidget(candidate, ignored))
                continue;
            const qint64 dx = qint64(position.x()) - requestedTopLeft.x();
            const qint64 dy = qint64(position.y()) - requestedTopLeft.y();
            const qint64 distance = dx * dx + dy * dy;
            if (distance < bestDistance) {
                bestDistance = distance;
                best = candidate;
            }
        }
    }
    if (best.isValid())
        return best;
    // 目标位置冲突时尝试空闲网格，不能把组件提交到其他卡片上方。
    return overlapping ? nearestGridRect(kind, variant, requestedTopLeft, scale, ignored)
                       : QRect();
}

bool DesktopWidget::overlapsVisibleWidget(const QRect& rect, const DesktopWidget* ignored)
{
    for (DesktopWidget* widget : s_instances) {
        if (widget && widget != ignored && widget->isVisible()
            && rect.intersects(widget->geometry()))
            return true;
    }
    return false;
}

void DesktopWidget::showGridPreview(CardKind kind, int variant, const QPoint& desktopPoint)
{
    AppSettings settings;
    const qreal scale = qBound<qreal>(0.40,
        settings.value(QStringLiteral("appearance/scale"), 1.0).toDouble(), 2.0);
    QScreen* screen = QGuiApplication::screenAt(desktopPoint);
    if (!screen) {
        hideGridPreview();
        return;
    }
    const QRect area = screen->availableGeometry();
    const int cell = qMax(1, qRound(kGridBaseCell * scale));
    const int gap = gridGapForArea(area, scale);
    const QSize size = gridWidgetSize(kind, variant, cell, gap);
    if (size.width() > area.width() || size.height() > area.height()) {
        hideGridPreview();
        return;
    }
    const QPoint desired = desktopPoint - QPoint(size.width() / 2, size.height() / 2);
    const QPoint freePosition(qBound(area.left(), desired.x(), area.right() - size.width() + 1),
                              qBound(area.top(), desired.y(), area.bottom() - size.height() + 1));
    showGridPreviewRect(nearbyWidgetRect(kind, variant, freePosition, scale));
}

void DesktopWidget::showGridPreviewRect(const QRect& rect)
{
    if (!rect.isValid()) {
        hideGridPreview();
        return;
    }
    gridOverlay()->display(rect);
}

void DesktopWidget::hideGridPreview()
{
    gridOverlay()->dismiss();
    gridFadeOverlay()->dismiss();
}

bool DesktopWidget::placeAtDesktopPoint(const QPoint& point)
{
    QScreen* screen = QGuiApplication::screenAt(point);
    if (!screen)
        return false;
    const QRect area = screen->availableGeometry();
    const int cell = qMax(1, qRound(kGridBaseCell * m_widgetScale));
    const QSize size = gridWidgetSize(m_cardKind, m_cardVariant, cell,
                                      gridGapForArea(area, m_widgetScale));
    if (size.width() > area.width() || size.height() > area.height())
        return false;
    const QPoint desired = point - QPoint(size.width() / 2, size.height() / 2);
    const QPoint freePosition(qBound(area.left(), desired.x(), area.right() - size.width() + 1),
                              qBound(area.top(), desired.y(), area.bottom() - size.height() + 1));
    const QRect snapped = nearbyWidgetRect(m_cardKind, m_cardVariant, freePosition, m_widgetScale, this);
    if (!snapped.isValid() && overlapsVisibleWidget(QRect(freePosition, size), this))
        return false;
    const QRect target = snapped.isValid() ? snapped : QRect(freePosition, size);
    m_restoringPosition = true;
    setFixedSize(target.size());
    move(target.topLeft());
    m_restoringPosition = false;
    m_hasSavedPosition = true;
    captureDesktopBackdrop();
    saveAllConfigurations();
    return true;
}

DesktopWidget* DesktopWidget::addWidget(CardKind kind, const QPoint& desktopPoint, int variant)
{
    auto* widget = new DesktopWidget(nullptr, kind, false, QString(), variant);
    widget->setAttribute(Qt::WA_DeleteOnClose, true);
    if (!widget->placeAtDesktopPoint(desktopPoint)) {
        delete widget;
        return nullptr;
    }
    widget->show();
    saveAllConfigurations();
    return widget;
}

QSize DesktopWidget::cardSize(CardKind kind, int variant, qreal scale, const QRect& workArea)
{
    scale = qBound<qreal>(.4, scale, 2.0);
    return gridWidgetSize(kind, variant, qMax(1, qRound(kGridBaseCell * scale)),
                          gridGapForArea(workArea, scale));
}

QImage DesktopWidget::renderPreview(CardKind kind, int variant, const QSize& size,
                                    qreal devicePixelRatio)
{
    if (size.isEmpty())
        return {};
    DesktopWidget preview(nullptr, kind, false,
                          QStringLiteral("gallery-preview"), variant, true);
    preview.setDesktopLayerEnabled(false);
    preview.setDesktopCaptureEnabled(false);
    preview.setAnimationEnabled(false);
    // 保留卡片真实网格比例，不强行拉伸到图库统一矩形。
    const QSize reference = gridWidgetSize(kind, variant, kGridBaseCell,
                                           qRound(kGridBaseGap));
    QSize renderSize = reference;
    renderSize.scale(size, Qt::KeepAspectRatio);
    renderSize = renderSize.expandedTo(QSize(1, 1));
    preview.m_widgetScale = qMin(renderSize.width() / qreal(reference.width()),
                             renderSize.height() / qreal(reference.height()));
    preview.setMinimumSize(0, 0);
    preview.setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    preview.resize(renderSize);

    // 未显示的 GL 窗口可能没有帧缓冲；直接向图像调用相同内容绘制，避免空白预览。
    const qreal dpr = qMax<qreal>(1.0, devicePixelRatio);
    QImage image(QSize(qMax(1, qRound(renderSize.width() * dpr)),
                      qMax(1, qRound(renderSize.height() * dpr))),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(36, 54, 76, 150));
    const qreal previewRadius = 34.0 * preview.m_widgetScale;
    painter.drawRoundedRect(QRectF(QPointF(), QSizeF(renderSize)), previewRadius, previewRadius);
    preview.paintOverlay(painter);
    painter.end();
    return image;
}

QList<DesktopWidget*> DesktopWidget::restoreWidgets()
{
    QList<DesktopWidget*> result;
    QFile file(configurationPath());
    if (!file.open(QIODevice::ReadOnly))
        return result;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject root = document.object();
    const int version = root.value(QStringLiteral("version")).toInt();
    const QJsonArray widgets = root.value(QStringLiteral("widgets")).toArray();
    bool primaryAssigned = false;
    for (const QJsonValue& value : widgets) {
        const QJsonObject item = value.toObject();
        const QString type = item.value(QStringLiteral("type")).toString();
        if (type != QStringLiteral("battery") && type != QStringLiteral("weather")
            && type != QStringLiteral("clock") && type != QStringLiteral("dictionary")
            && type != QStringLiteral("music"))
            continue;
        const CardKind kind = kindFromName(type);
        // 优先由电量组件持有托盘，配置次序变化不改变此规则。
        const bool primary = kind == CardKind::Battery && !primaryAssigned;
        const int storedVariant = item.contains(QStringLiteral("variant"))
            ? item.value(QStringLiteral("variant")).toInt() : defaultVariant(kind);
        auto* widget = new DesktopWidget(nullptr,
                                         kind,
                                         primary,
                                         item.value(QStringLiteral("id")).toString(),
                                         storedVariant);
        QPoint restoredPosition(item.value(QStringLiteral("x")).toInt(),
                                item.value(QStringLiteral("y")).toInt());
        if (version < 4 && item.contains(QStringLiteral("gridColumn"))
            && item.contains(QStringLiteral("gridRow"))) {
            QScreen* targetScreen = nullptr;
            const QString screenName = item.value(QStringLiteral("screen")).toString();
            for (QScreen* candidate : QGuiApplication::screens()) {
                if (candidate && candidate->name() == screenName) {
                    targetScreen = candidate;
                    break;
                }
            }
            if (!targetScreen)
                targetScreen = QGuiApplication::screenAt(restoredPosition);
            if (!targetScreen)
                targetScreen = QGuiApplication::primaryScreen();
            if (targetScreen) {
                const int cell = qMax(1, qRound(kGridBaseCell * widget->m_widgetScale));
                const QRect targetArea = targetScreen->availableGeometry();
                const int gap = gridGapForArea(targetArea, widget->m_widgetScale);
                const int pitch = cell + gap;
                restoredPosition = gridOrigin(targetArea, gap)
                    + QPoint(item.value(QStringLiteral("gridColumn")).toInt() * pitch,
                             item.value(QStringLiteral("gridRow")).toInt() * pitch);
            }
        }
        QRect restoredTarget;
        if (version < 4) {
            restoredTarget = nearestGridRect(kind, widget->m_cardVariant,
                                             restoredPosition, widget->m_widgetScale, widget);
        } else {
            const int cell = qMax(1, qRound(kGridBaseCell * widget->m_widgetScale));
            QScreen* screen = nullptr;
            int bestOverlap = 0;
            // 显示器变更后保存中心可能位于所有屏幕之外，需要重新选择目标屏幕。
            for (QScreen* candidate : QGuiApplication::screens()) {
                const QRect candidateArea = candidate->availableGeometry();
                const int candidateGap = gridGapForArea(candidateArea, widget->m_widgetScale);
                const QSize candidateSize = gridWidgetSize(
                    kind, widget->m_cardVariant, cell, candidateGap);
                const QRect overlap = candidateArea.intersected(
                    QRect(restoredPosition, candidateSize));
                const int pixels = overlap.isValid() ? overlap.width() * overlap.height() : 0;
                if (pixels > bestOverlap) {
                    bestOverlap = pixels;
                    screen = candidate;
                }
            }
            if (!screen)
                screen = QGuiApplication::primaryScreen();
            const QRect area = screen ? screen->availableGeometry() : QRect();
            const int gap = gridGapForArea(area, widget->m_widgetScale);
            const QSize size = gridWidgetSize(kind, widget->m_cardVariant, cell, gap);
            if (screen) {
                restoredPosition.setX(qBound(area.left(), restoredPosition.x(),
                    qMax(area.left(), area.right() - size.width() + 1)));
                restoredPosition.setY(qBound(area.top(), restoredPosition.y(),
                    qMax(area.top(), area.bottom() - size.height() + 1)));
            }
            restoredTarget = QRect(restoredPosition, size);
        }
        if (!restoredTarget.isValid()) {
            delete widget;
            continue;
        }
        primaryAssigned = primaryAssigned || primary;
        widget->m_restoringPosition = true;
        widget->setFixedSize(restoredTarget.size());
        widget->move(restoredTarget.topLeft());
        widget->m_restoringPosition = false;
        widget->m_hasSavedPosition = true;
        if (item.value(QStringLiteral("visible")).toBool(true))
            widget->show();
        result.append(widget);
    }
    if (!primaryAssigned && !result.isEmpty()) {
        // 旧配置即使没有电量卡片，也要指定一个托盘管理者。
        result.first()->m_ownsTray = true;
        result.first()->setupTrayIcon();
    }
    // 恢复旧网格配置后迁移为精确位置。
    if (!result.isEmpty())
        saveAllConfigurations();
    return result;
}

void DesktopWidget::showWidgetLibrary()
{
    // 所有卡片共用托盘管理者的图库，避免重复创建置顶捕获面板。
    if (!m_ownsTray) {
        for (DesktopWidget* widget : std::as_const(s_instances)) {
            if (widget && widget != this && widget->m_ownsTray) {
                widget->showWidgetLibrary();
                return;
            }
        }
    }
    if (!m_library) {
        // 置顶工具窗口不能由桌面卡片原生拥有。
        m_library = new WidgetLibraryDialog;
        connect(m_library, &WidgetLibraryDialog::widgetDropped, this,
                [](int encoded, const QPoint& globalPos) {
                    const CardKind kind = static_cast<CardKind>(qBound(0, encoded / 100, 4));
                    const int variant = encoded % 100;
                    if (!addWidget(kind, globalPos, variant))
                        QToolTip::showText(globalPos, QStringLiteral("当前屏幕没有足够的空位，可缩小组件后重试"));
                });
        connect(m_library, &WidgetLibraryDialog::widgetDragStarted, this,
                [this](int) {
                    if (!m_libraryDragHiddenWindows.isEmpty())
                        return;
                    QSet<WId> widgetWindows;
                    for (DesktopWidget* widget : std::as_const(s_instances)) {
                        if (widget && widget->isVisible())
                            widgetWindows.insert(widget->winId());
                    }
                    m_libraryDragHiddenWindows =
                        NativeWindows::hideApplicationWindows(widgetWindows);
                });
        connect(m_library, &WidgetLibraryDialog::widgetDragFinished, this,
                [this](int) {
                    NativeWindows::restoreApplicationWindows(m_libraryDragHiddenWindows);
                    m_libraryDragHiddenWindows.clear();
                });
        connect(m_library, &WidgetLibraryDialog::widgetDragPreview, this,
                [this](int encoded, const QPoint& globalPos, bool visible) {
                    if (visible)
                        showGridPreview(static_cast<CardKind>(qBound(0, encoded / 100, 4)),
                                        encoded % 100, globalPos);
                    else
                        hideGridPreview();
                });
    }
    // 图库可能比设置窗口存活更久，每次打开时重新同步全局显示配置。
    m_library->setInterfaceScale(m_widgetScale);
    m_library->setLiveBackdropEnabled(liveBackdropEnabled());
    m_library->setRenderScale(renderScale());
    m_library->setMaterialBlurStrength(materialBlurStrength());
    m_library->setGlassOpacity(glassOpacity());
    if (!m_library->isVisible()) {
        m_library->prepareBackdrop();
        m_library->show();
    }
    m_library->raise();
    m_library->activateWindow();
}

void DesktopWidget::refreshBattery()
{
    const int previousLevel = m_batteryLevel;
    const bool previousCharging = m_charging;
    const bool previousHasBattery = m_hasBattery;
    const QVector<ConnectedDevice> previousDevices = m_connectedDevices;
#ifdef Q_OS_WIN
    SYSTEM_POWER_STATUS status{};
    if (GetSystemPowerStatus(&status)) {
        m_hasBattery = status.BatteryFlag != 128 && status.BatteryLifePercent != 255;
        if (m_hasBattery)
            m_batteryLevel = qBound(0, static_cast<int>(status.BatteryLifePercent), 100);
        // 无电池且接交流电属于台式机适配器状态，不能显示虚构电量或充电标志。
        m_charging = m_hasBattery && status.ACLineStatus == 1;
    } else {
        m_hasBattery = false;
        m_charging = false;
    }
#else
    m_hasBattery = false;
    m_charging = false;
#endif

    // 外设连接变化较慢，首次立即读取，之后每三十秒刷新。
    if (m_devicePollCount == 0)
        refreshConnectedDevices();
    m_devicePollCount = (m_devicePollCount + 1) % 15;

    bool devicesChanged = previousDevices.size() != m_connectedDevices.size();
    if (!devicesChanged) {
        for (int i = 0; i < previousDevices.size(); ++i) {
            const ConnectedDevice& before = previousDevices.at(i);
            const ConnectedDevice& after = m_connectedDevices.at(i);
            if (before.name != after.name || before.level != after.level
                || before.iconKind != after.iconKind) {
                devicesChanged = true;
                break;
            }
        }
    }
    const bool changed = previousLevel != m_batteryLevel
                         || previousCharging != m_charging
                         || previousHasBattery != m_hasBattery
                         || devicesChanged;
    // 电源状态未变化时跳过重绘和托盘图标更新。
    if (changed)
        update();
    if (changed && m_trayIcon) {
        m_trayIcon->setIcon(createTrayIcon());
        m_trayIcon->setToolTip(m_hasBattery
                                   ? QStringLiteral("macdowsOS Widget  ·  %1%").arg(m_batteryLevel)
                                   : QStringLiteral("macdowsOS Widget  ·  电源适配器"));
    }
}

void DesktopWidget::refreshConnectedDevices()
{
    if (m_deviceWatcher.isRunning())
        return;
    // 设备发现可能阻塞数秒；电量卡片共用后台任务，结果交付界面线程，任务不捕获窗口指针。
    static QFuture<QVector<ConnectedDevice>> shared;
    static QElapsedTimer age;
    if (!age.isValid() || (age.elapsed() >= 30000 && shared.isFinished())) {
        shared = QtConcurrent::run(&DesktopWidget::queryConnectedDevices);
        age.restart();
    }
    m_deviceWatcher.setFuture(shared);
}

QVector<DesktopWidget::ConnectedDevice> DesktopWidget::queryConnectedDevices()
{
    QVector<ConnectedDevice> devices;
#ifdef Q_OS_WIN
    const QVector<WindowsPeripheralProperty> properties = windowsPeripheralProperties();
    QHash<QString, int> batteryLevels;
    QSet<QString> seen;
    const auto alreadySeen = [&seen](const QString& key) {
        if (seen.contains(key))
            return true;
        if (key.size() < 5)
            return false;
        for (const QString& existing : seen) {
            if (existing.contains(key) || key.contains(existing))
                return true;
        }
        return false;
    };

    // 设备发现补充旧蓝牙接口未返回的低功耗蓝牙和 HID 设备。
    for (const WindowsPeripheralProperty& property : properties) {
        const QString key = normalizedDeviceName(property.name);
        if (key.isEmpty())
            continue;
        if (property.level >= 0)
            batteryLevels.insert(key, property.level);
        if (!property.connected || devices.size() >= kMaxConnectedDevices || alreadySeen(key))
            continue;

        const QString description = property.name + QChar(' ') + property.category;
        const int iconKind = iconKindForBluetoothDevice(description, 0);
        const QString lowered = description.toLower();
        const bool internalBattery = lowered.contains(QStringLiteral("acpi"))
                                     || lowered.contains(QStringLiteral("control method battery"))
                                     || lowered.contains(QStringLiteral("电池"));
        // 只展示可识别的输入、音频配件或实际报告电量的设备，排除普通显示器和打印机。
        if (internalBattery || (iconKind == 4 && property.level < 0))
            continue;

        ConnectedDevice device;
        device.name = property.name;
        device.iconKind = iconKind;
        device.level = property.level;
        devices.append(device);
        seen.insert(key);
    }

    BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
    search.dwSize = sizeof(search);
    search.fReturnConnected = TRUE;
    search.fIssueInquiry = FALSE;
    search.cTimeoutMultiplier = 1;

    BLUETOOTH_DEVICE_INFO info{};
    info.dwSize = sizeof(info);
    HBLUETOOTH_DEVICE_FIND finder = BluetoothFindFirstDevice(&search, &info);
    if (finder && devices.size() < kMaxConnectedDevices) {
        do {
            if (!info.fConnected)
                continue;
            const QString name = QString::fromWCharArray(info.szName).trimmed();
            const QString key = normalizedDeviceName(name);
            if (key.isEmpty() || alreadySeen(key))
                continue;

            ConnectedDevice device;
            device.name = name;
            device.iconKind = iconKindForBluetoothDevice(name, info.ulClassofDevice);
            device.level = batteryLevels.value(key, -1);
            if (device.level < 0) {
                // 设备名可能带不同后缀，规范化后使用包含匹配，同时避免关联无关短名称。
                for (auto it = batteryLevels.cbegin(); it != batteryLevels.cend(); ++it) {
                    if (key.size() >= 5 && (key.contains(it.key()) || it.key().contains(key))) {
                        device.level = it.value();
                        break;
                    }
                }
            }
            devices.append(device);
            seen.insert(key);
        } while (devices.size() < kMaxConnectedDevices
                 && BluetoothFindNextDevice(finder, &info));
    }
    if (finder)
        BluetoothFindDeviceClose(finder);

#endif
    return devices;
}

void DesktopWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        if (m_cardKind == CardKind::Music) {
            event->accept();
            return;
        }
        if (m_cardKind == CardKind::Weather) {
            // 双击地点打开城市搜索，右键菜单提供同一入口。
            searchWeatherCity();
            event->accept();
            return;
        }
        // 主卡片双击重新排列组件，与托盘排列动作一致。
        if (m_ownsTray)
            arrangeGroup();
    }
    QOpenGLWidget::mouseDoubleClickEvent(event);
}

void DesktopWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragOrigin = pos();
        m_clickPending = true;
    }
    LiquidGlassWidget::mousePressEvent(event);
}

void DesktopWidget::windowDragStarted()
{
    NativeWindows::restoreApplicationWindows(m_dragHiddenWindows);
    m_dragHiddenWindows.clear();
    QSet<WId> widgetWindows;
    for (DesktopWidget* widget : std::as_const(s_instances)) {
        if (!widget || !widget->isVisible())
            continue;
        widgetWindows.insert(widget->winId());
    }
    m_dragHiddenWindows = NativeWindows::hideApplicationWindows(widgetWindows);
}

void DesktopWidget::windowDragFinished()
{
    m_clickPending = false;
    hideGridPreview();
    bool shouldSave = false;
    if (!m_restoringPosition) {
        const QRect target = nearbyWidgetRect(m_cardKind, m_cardVariant, pos(), m_widgetScale, this);
        m_restoringPosition = true;
        if (target.isValid()) {
            setFixedSize(target.size());
            move(target.topLeft());
        } else if (overlapsVisibleWidget(geometry(), this)) {
            move(m_dragOrigin);
        }
        m_restoringPosition = false;
        m_hasSavedPosition = true;
        shouldSave = true;
    }

    NativeWindows::restoreApplicationWindows(m_dragHiddenWindows);
    m_dragHiddenWindows.clear();
    // 先恢复普通应用窗口，再保存最终布局。
    if (shouldSave)
        saveAllConfigurations();
}

void DesktopWidget::mouseReleaseEvent(QMouseEvent* event)
{
    const bool clicked = event->button() == Qt::LeftButton && m_clickPending
                         && !windowDragging() && !mouseThroughEnabled();
    m_clickPending = false;
    LiquidGlassWidget::mouseReleaseEvent(event);
    if (!clicked)
        return;
    const QPointF point = event->position() / m_widgetScale;
    if (m_cardKind == CardKind::Music) {
        const auto regions = MusicCard::layout(QRectF(0, 0, width() / m_widgetScale, height() / m_widgetScale));
        auto* media = SystemMediaSession::instance();
        const auto& state = media->snapshot();
        if (state.canToggle && regions.toggle.contains(point))
            media->sendCommand(SystemMediaSession::Command::TogglePlayback);
        else if (state.canPrevious && regions.previous.contains(point))
            media->sendCommand(SystemMediaSession::Command::Previous);
        else if (state.canNext && regions.next.contains(point))
            media->sendCommand(SystemMediaSession::Command::Next);
        return;
    }
    if (m_cardKind == CardKind::Dictionary
        && QRectF(32, height() / m_widgetScale - 52, 82, 32).contains(point))
        refreshDictionary();
    else if (m_cardKind == CardKind::Weather
             && QRectF(36, 27, width() / m_widgetScale - 64, 32).contains(point))
        searchWeatherCity();
}

void DesktopWidget::contextMenuEvent(QContextMenuEvent* event)
{
    if (!event)
        return;

    QFont menuFont(pingFangFontFamily());
    menuFont.setPixelSize(16);
    applyFontSmoothing(menuFont);
    LiquidGlassMenu menu(menuFont, this);
    QAction* addAction = menu.addGlassAction(QStringLiteral("添加小组件…"), LiquidGlassMenu::Icon::Add);
    menu.addSeparator();
    QAction* cityAction = nullptr;
    if (m_cardKind == CardKind::Weather) {
        cityAction = menu.addGlassAction(QStringLiteral("搜索城市…"), LiquidGlassMenu::Icon::Search);
        menu.addSeparator();
    }
    QAction* removeAction = menu.addGlassAction(QStringLiteral("删除此组件"), LiquidGlassMenu::Icon::Remove);
    // 至少保留一个托盘管理者，避免进程运行却无法重新打开图库。
    removeAction->setEnabled(s_instances.size() > 1);
    QAction* selected = menu.exec(event->globalPos());
    if (selected == addAction) {
        showWidgetLibrary();
    } else if (cityAction && selected == cityAction) {
        searchWeatherCity();
    } else if (selected == removeAction && removeAction->isEnabled()) {
        const bool wasPrimary = m_ownsTray;
        if (m_trayIcon)
            m_trayIcon->hide();

        // 保存前先从占位集合移除，避免已删除卡片残留配置或阻挡放置。
        s_instances.removeAll(this);
        if (wasPrimary && !s_instances.isEmpty()) {
            DesktopWidget* successor = s_instances.first();
            successor->m_ownsTray = true;
            successor->setupTrayIcon();
        }
        for (DesktopWidget* widget : s_instances) {
            if (widget && widget->m_ownsTray) {
                widget->updateTrayVisibilityLabel();
                widget->updateRearrangeLabel();
                break;
            }
        }
        saveAllConfigurations();
        hideGridPreview();
        hide();
        deleteLater();
    }
    event->accept();
}

void DesktopWidget::moveEvent(QMoveEvent* event)
{
    LiquidGlassWidget::moveEvent(event);
    if (!m_restoringPosition && windowDragging())
        showGridPreviewRect(nearbyWidgetRect(m_cardKind, m_cardVariant, pos(), m_widgetScale, this));
}
