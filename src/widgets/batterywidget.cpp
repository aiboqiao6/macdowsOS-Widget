#include "widgets/batterywidget.h"
#include "ui/widgetlibrarydialog.h"
#include "rendering/responsivelayout.h"

#include <QApplication>
#include <QtConcurrent/QtConcurrentRun>
#include <QElapsedTimer>
#include <QAction>
#include <QDateTime>
#include <QDate>
#include <QTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEnterEvent>
#include <QGuiApplication>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QFormLayout>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegion>
#include <QScreen>
#include <QSet>
#include <QSysInfo>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStringList>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QToolTip>
#include <QVBoxLayout>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QInputDialog>
#include <QImage>
#include <QUrl>
#include <QUrlQuery>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QtMath>
#include <QSaveFile>
#include <QFile>
#include <QUuid>
#include <limits>
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

namespace {
int s_fontSmoothingLevel = 2;

void configureFontSmoothing(QFont& font, int level)
{
    switch (qBound(0, level, 3)) {
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

// The two reference cards use the same 1 px glass edge and different canvas
// sizes.  Keeping these dimensions explicit makes the layout deterministic on
// every DPI scale while the LiquidGlassWidget handles the actual backdrop.
constexpr int kOverviewWidth = 688;
constexpr int kOverviewHeight = 328;
constexpr qreal kDashboardRadius = 50.0;
constexpr qreal kDashboardCardRadius = 32.0;
const QRectF kDashboardBatteryRect(24.0, 24.0, 680.0, 340.0);
const QRectF kDashboardWeatherRect(724.0, 24.0, 292.0, 340.0);
const QRectF kDashboardClockRect(24.0, 384.0, 440.0, 292.0);
const QRectF kDashboardInfoRect(484.0, 384.0, 532.0, 292.0);

constexpr int kGridBaseCell = 328;
constexpr int kBatteryListRows = 6;
// The list includes the host row, so six displayed devices means at most
// five connected peripherals in addition to the current machine.
constexpr int kMaxDisplayedDevices = 6;
constexpr int kMaxConnectedDevices = kMaxDisplayedDevices - 1;
constexpr int kWeatherHourlySlots = 6;
constexpr int kWeatherDailySlots = 5;
// Section boundaries measured from the 2x2 reference card. Keeping them as
// normalized layout tokens makes their placement independent of card size,
// UI scale, and DPI.
constexpr qreal kWeatherSummaryDividerRatio = .278;
constexpr qreal kWeatherDailyDividerRatio = .539;
constexpr qreal kWeatherHourlyTopRatio = .298;
constexpr qreal kWeatherHourlyIconOffsetRatio = .083;
constexpr qreal kWeatherHourlyTemperatureOffsetRatio = .170;
constexpr qreal kWeatherLargeLocationFontRatio = .060;
constexpr qreal kWeatherLargeTemperatureFontRatio = .180;
constexpr qreal kWeatherCompactConditionFontRatio = .058;
constexpr qreal kWeatherWideConditionFontRatio = .052;
constexpr qreal kWeatherLargeConditionFontRatio = .050;
constexpr qreal kWeatherConditionGapRatio = .014;
constexpr qreal kWeatherLargeRangeFontRatio = .100;
constexpr qreal kWeatherLargeHourlyFontRatio = .053;
constexpr qreal kWeatherLargeDailyLabelFontRatio = .056;
constexpr qreal kWeatherLargeDailyTemperatureFontRatio = .055;
// Clock geometry is expressed as proportions of the available card so every
// retained style uses the same compact edge rhythm at any grid/DPI scale.
constexpr qreal kClockCardInsetRatio = .018;
constexpr qreal kClockFaceInsetRatio = .022;
constexpr qreal kClockDialInsetRatio = .040;
constexpr qreal kClockDialRadiusRatio = .455;
constexpr qreal kClockCityLabelHeightRatio = .130;
constexpr qreal kClockCityLabelGapRatio = .012;
// Grid pitch is the card size plus a small adaptive gutter. The gutter scales
// with the UI and with the shorter display edge so cards keep a consistent
// visual rhythm on both small and high-resolution screens.
constexpr qreal kGridBaseGap = 16.0;

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
        // This is also the family name embedded in pingfang0.ttf. If the
        // resource is unavailable, Qt will still resolve an installed face of
        // the same name rather than silently switching to a different stack.
        return QStringLiteral("PingFang SC");
    }();
    return family;
}

QFont displayFont(int pointSize, QFont::Weight weight = QFont::Normal)
{
    QFont font;
    font.setFamily(pingFangFamily());
    font.setPointSize(pointSize);
    font.setWeight(weight);
    configureFontSmoothing(font, s_fontSmoothingLevel);
    return font;
}

QJsonObject jsonObjectAfterMarker(const QByteArray& payload, const QByteArray& marker)
{
    const int markerIndex = payload.indexOf(marker);
    const int objectStart = markerIndex < 0 ? -1 : payload.indexOf('{', markerIndex);
    if (objectStart < 0)
        return {};

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (int i = objectStart; i < payload.size(); ++i) {
        const char ch = payload.at(i);
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
        } else if (ch == '{') {
            ++depth;
        } else if (ch == '}' && --depth == 0) {
            const QJsonDocument document = QJsonDocument::fromJson(
                payload.mid(objectStart, i - objectStart + 1));
            return document.isObject() ? document.object() : QJsonObject{};
        }
    }
    return {};
}

QString weatherValue(const QJsonValue& value)
{
    QString result;
    if (value.isString())
        result = value.toString().trimmed();
    else if (value.isDouble())
        result = QString::number(qRound(value.toDouble()));
    result.remove(QChar(0x2103));
    result.remove(QChar(0x00B0));
    return result.trimmed();
}

QString weatherTemperatureLabel(QString value)
{
    value = value.trimmed();
    value.remove(QChar(0x2103));
    value.remove(QChar(0x00B0));
    value = value.trimmed();
    return (value.isEmpty() ? QStringLiteral("--") : value) + QChar(0x00B0);
}

int numericWeatherCode(const QString& value)
{
    QString digits;
    for (const QChar ch : value) {
        if (ch.isDigit())
            digits.append(ch);
    }
    bool ok = false;
    const int code = digits.toInt(&ok);
    return ok ? code : -1;
}

QString chineseWeekdayLabel(const QDate& date)
{
    static const QStringList labels = {
        QStringLiteral("周一"), QStringLiteral("周二"), QStringLiteral("周三"),
        QStringLiteral("周四"), QStringLiteral("周五"), QStringLiteral("周六"),
        QStringLiteral("周日")
    };
    const int index = qBound(1, date.dayOfWeek(), 7) - 1;
    return labels.at(index);
}

QString normalizedWeekdayLabel(QString value)
{
    value = value.trimmed();
    if (!value.startsWith(QStringLiteral("星期")))
        return value;

    QString suffix = value.mid(2);
    if (suffix == QStringLiteral("天"))
        suffix = QStringLiteral("日");
    return suffix.isEmpty() ? value : QStringLiteral("周") + suffix;
}

int defaultVariant(BatteryWidget::CardKind kind)
{
    switch (kind) {
    case BatteryWidget::CardKind::Battery: return 1;    // horizontal status
    case BatteryWidget::CardKind::Weather: return 0;    // compact forecast
    case BatteryWidget::CardKind::Clock: return 0;      // light analog face
    case BatteryWidget::CardKind::Dictionary: return 1; // horizontal definition
    }
    return 0;
}

int variantCount(BatteryWidget::CardKind kind)
{
    return kind == BatteryWidget::CardKind::Clock ? 9 : 3;
}

int normalizedVariant(BatteryWidget::CardKind kind, int variant)
{
    if (variant < 0)
        return defaultVariant(kind);
    return qBound(0, variant, variantCount(kind) - 1);
}

int gridColumnSpan(BatteryWidget::CardKind kind, int variant)
{
    variant = normalizedVariant(kind, variant);
    if (kind == BatteryWidget::CardKind::Clock)
        return variant == 7 || variant == 8 ? 2 : 1;
    if (kind == BatteryWidget::CardKind::Battery)
        return variant == 1 || variant == 2 ? 2 : 1;
    if (kind == BatteryWidget::CardKind::Weather)
        return variant == 1 || variant == 2 ? 2 : 1;
    return variant == 1 ? 2 : 1;
}

int gridRowSpan(BatteryWidget::CardKind kind, int variant)
{
    const int normalized = normalizedVariant(kind, variant);
    if (kind == BatteryWidget::CardKind::Clock)
        return 1;
    return ((kind == BatteryWidget::CardKind::Battery
             || kind == BatteryWidget::CardKind::Weather)
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

QSize gridWidgetSize(BatteryWidget::CardKind kind, int variant, int cell, int gap)
{
    const int span = gridColumnSpan(kind, variant);
    const int rows = gridRowSpan(kind, variant);
    return QSize(cell * span + gap * (span - 1),
                 cell * rows + gap * (rows - 1));
}

QPoint gridOrigin(const QRect& area, int gap)
{
    // Keep the outer gutter identical to the gap between neighboring cells.
    // This makes the grid rhythm symmetrical instead of placing the first
    // card flush against the available-work-area edge.
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

    // Keep the placement hint readable without becoming an opaque white slab
    // over the live desktop.  The outline remains thick and fully white; the
    // native window opacity provides the requested translucent appearance for
    // both the current and fading positions.
    static constexpr qreal kOverlayOpacity = 0.68;

    void display(const QRect& target)
    {
        if (!target.isValid())
            return;

        m_hideAfterFade = false;
        m_opacityAnimation->stop();
        if (isVisible() && geometry() != target) {
            // Keep the previous outline in a separate transparent window so
            // it can fade independently while the new outline appears at its
            // destination immediately.
            gridFadeOverlay()->fadeFrom(geometry(), windowOpacity());
        }

        setGeometry(target);
        setWindowOpacity(kOverlayOpacity);
        if (!isVisible())
            show();

#ifdef Q_OS_WIN
        // The hint is only a placement aid. Keep it immediately behind the
        // dragged card without activating it.
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
        animateOpacity(windowOpacity(), 0.0, 235);
    }

    void dismiss()
    {
        if (!isVisible())
            return;
        m_hideAfterFade = true;
        animateOpacity(windowOpacity(), 0.0, 190);
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
        // This layered Windows category is the same source used by the
        // Devices UI and exposes System.Devices.BatteryLife when a Bluetooth
        // or HID driver publishes it.
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
                            // Keep battery records even when an older driver
                            // omits the Connected property; they can still be
                            // associated with a device found by Bluetooth API.
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

QString BatteryWidget::pingFangFontFamily()
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

void BatteryWidget::setGlobalFontSmoothing(int level)
{
    const int next = qBound(0, level, 3);
    s_fontSmoothingLevel = next;
    QSettings().setValue(QStringLiteral("appearance/fontSmoothing"), next);
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        QFont font = app->font();
        font.setFamily(pingFangFamily());
        configureFontSmoothing(font, next);
        app->setFont(font);
        for (QWidget* window : app->topLevelWidgets()) {
            if (window)
                window->update();
        }
    }
}

int BatteryWidget::globalFontSmoothing()
{
    return s_fontSmoothingLevel;
}

void BatteryWidget::applyFontSmoothing(QFont& font)
{
    configureFontSmoothing(font, s_fontSmoothingLevel);
}

QList<BatteryWidget*> BatteryWidget::s_instances;

BatteryWidget::BatteryWidget(QWidget* parent, CardKind kind, bool primary,
                             const QString& instanceId, int variant)
    : LiquidGlassWidget(parent), m_kind(kind), m_primary(primary),
      m_variant(normalizedVariant(kind, variant)),
      m_instanceId(instanceId.isEmpty()
                        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                        : instanceId)
{
    m_hostDeviceName = QSysInfo::machineHostName().trimmed();
    if (m_hostDeviceName.isEmpty())
        m_hostDeviceName = QStringLiteral("本机");
    s_instances.append(this);
    QSettings settings;
    m_uiScale = qBound<qreal>(0.40, settings.value(QStringLiteral("appearance/scale"), 1.0).toDouble(), 2.0);
    setGlassMargins(0);
    const int initialCell = qMax(1, qRound(kGridBaseCell * m_uiScale));
    QScreen* initialScreen = QGuiApplication::primaryScreen();
    const int initialGap = initialScreen
        ? gridGapForArea(initialScreen->availableGeometry(), m_uiScale)
        : qMax(4, qRound(kGridBaseGap * m_uiScale));
    setFixedSize(gridWidgetSize(m_kind, m_variant, initialCell, initialGap));
    setGlassRadius(34.0 * m_uiScale);
    configureGlass(3.8f, 1.32f, 4.5f, 3, 0.008f);
    // The clock's second hand uses sub-second interpolation. Keep its idle
    // cadence at the previous ~15 FPS while static cards can sleep at 5 FPS.
    setAnimationEnabled(m_kind == CardKind::Clock);
    const bool savedSystemBlur = settings.value(QStringLiteral("appearance/systemBlur"), false).toBool();
    const bool savedMouseThrough = settings.value(QStringLiteral("interaction/mouseThrough"), false).toBool();
    const bool savedLowPowerRefresh = settings.value(QStringLiteral("interaction/lowPowerRefresh"), false).toBool();
    const bool savedLiveBackdrop = settings.value(QStringLiteral("appearance/liveBackdrop"), false).toBool();
    const int savedFontSmoothing = qBound(0,
        settings.value(QStringLiteral("appearance/fontSmoothing"), 2).toInt(), 3);
    const int savedBlurStrength = settings.value(QStringLiteral("appearance/blurStrength"), 55).toInt();
    const qreal savedOpacity = settings.value(QStringLiteral("appearance/opacity"), 1.0).toDouble();
    const int savedBackend = settings.value(QStringLiteral("performance/renderBackend"), 0).toInt();
    const qreal savedRenderQuality = qBound<qreal>(0.5,
        settings.value(QStringLiteral("performance/renderScale"), 1.0).toDouble(), 1.5);
    setRenderBackend(static_cast<RenderBackend>(qBound(0, savedBackend, 2)));
    setRenderScale(float(savedRenderQuality));
    setSystemBlurEnabled(savedSystemBlur);
    setMaterialBlurStrength(savedBlurStrength);
    setMouseThroughEnabled(savedMouseThrough);
    setLowPowerRefreshEnabled(savedLowPowerRefresh);
    setLiveBackdropEnabled(savedLiveBackdrop);
    if (savedFontSmoothing != globalFontSmoothing())
        setGlobalFontSmoothing(savedFontSmoothing);
    setGlassOpacity(savedOpacity);
    setToolTip(m_kind == CardKind::Battery ? QStringLiteral("电量 · 拖动移动组件")
               : (m_kind == CardKind::Weather ? QStringLiteral("天气 · 拖动移动组件")
                  : (m_kind == CardKind::Clock ? QStringLiteral("时钟 · 拖动移动组件")
                     : QStringLiteral("词典 · 拖动移动组件"))));

    if (m_kind == CardKind::Battery) {
        connect(&m_refreshTimer, &QTimer::timeout, this, &BatteryWidget::refreshBattery);
        m_refreshTimer.start(2000);
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
    if (m_kind == CardKind::Weather) {
        m_weatherNetwork = new QNetworkAccessManager(this);
        connect(m_weatherNetwork, &QNetworkAccessManager::finished,
                this, &BatteryWidget::handleWeatherReply);
        m_weatherTimer.setTimerType(Qt::VeryCoarseTimer);
        m_weatherTimer.setInterval(10 * 60 * 1000);
        connect(&m_weatherTimer, &QTimer::timeout, this, &BatteryWidget::refreshWeather);
        m_weatherTimer.start();
        refreshWeather();
    }
    if (m_kind == CardKind::Dictionary) {
        m_dictionaryNetwork = new QNetworkAccessManager(this);
        connect(m_dictionaryNetwork, &QNetworkAccessManager::finished,
                this, &BatteryWidget::handleDictionaryReply);
        m_dictionaryTimer.setTimerType(Qt::VeryCoarseTimer);
        m_dictionaryTimer.setInterval(30 * 60 * 1000);
        connect(&m_dictionaryTimer, &QTimer::timeout,
                this, &BatteryWidget::refreshDictionary);
        m_dictionaryTimer.start();
        refreshDictionary();
    }

    // Settings are persisted once by the primary/tray instance. Other cards
    // poll the same keys so material, opacity, scale and input mode stay in
    // lockstep without duplicating a second settings window.
    m_settingsTimer.setTimerType(Qt::VeryCoarseTimer);
    // Settings changes from the dialog are pushed directly to every live
    // widget. The poll only covers edits made by another process, so a slower
    // interval avoids repeated QSettings reads without delaying normal UI
    // changes.
    m_settingsTimer.setInterval(5000);
    connect(&m_settingsTimer, &QTimer::timeout, this, [this]() {
        QSettings current;
        const QString savedLocation = current.value(QStringLiteral("weather/location"),
                                                    QStringLiteral("北京")).toString().trimmed();
        const QString savedCityId = current.value(QStringLiteral("weather/cityId"),
                                                   QStringLiteral("101010100")).toString().trimmed();
        if (m_kind == CardKind::Weather && !savedLocation.isEmpty()
            && (!savedCityId.isEmpty() && savedCityId != m_weatherCityId
                || savedLocation != m_weatherLocation)) {
            m_weatherLocation = savedLocation;
            m_weatherCityId = savedCityId;
            refreshWeather();
        }
        const qreal scale = qBound<qreal>(0.40, current.value(QStringLiteral("appearance/scale"), 1.0).toDouble(), 2.0);
        if (!qFuzzyCompare(scale, m_uiScale))
            applyScale(scale);
        const bool blur = current.value(QStringLiteral("appearance/systemBlur"), false).toBool();
        if (blur != systemBlurEnabled())
            setSystemBlurEnabled(blur);
        const int blurStrength = qBound(0, current.value(QStringLiteral("appearance/blurStrength"), 55).toInt(), 100);
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
        const bool liveBackdrop = current.value(QStringLiteral("appearance/liveBackdrop"), false).toBool();
        if (liveBackdrop != liveBackdropEnabled())
            setLiveBackdropEnabled(liveBackdrop);
        const int fontSmoothing = qBound(0,
            current.value(QStringLiteral("appearance/fontSmoothing"), 2).toInt(), 3);
        if (fontSmoothing != globalFontSmoothing())
            setGlobalFontSmoothing(fontSmoothing);
        const qreal renderQuality = qBound<qreal>(0.5,
            current.value(QStringLiteral("performance/renderScale"), 1.0).toDouble(), 1.5);
        if (qAbs(renderQuality - qreal(renderScale())) > 0.001)
            setRenderScale(float(renderQuality));
        const RenderBackend backend = static_cast<RenderBackend>(
            qBound(0, current.value(QStringLiteral("performance/renderBackend"), 0).toInt(), 2));
        if (backend != renderBackend())
            setRenderBackend(backend);
    });
    m_settingsTimer.start();

    if (!m_hasSavedPosition)
        moveToGroupPosition();
    captureDesktopBackdrop();
    if (m_kind == CardKind::Battery)
        refreshBattery();
    if (m_primary)
        setupTrayIcon();
}

BatteryWidget::~BatteryWidget()
{
    NativeWindows::restoreApplicationWindows(m_dragHiddenWindows);
    m_dragHiddenWindows.clear();
    NativeWindows::restoreApplicationWindows(m_libraryDragHiddenWindows);
    m_libraryDragHiddenWindows.clear();
    delete m_library;
    s_instances.removeAll(this);
}

void BatteryWidget::shutdown()
{
    const auto widgets = s_instances;
    qDeleteAll(widgets);
}

void BatteryWidget::showEvent(QShowEvent* event)
{
    LiquidGlassWidget::showEvent(event);
    for (BatteryWidget* widget : s_instances)
        widget->updateTrayVisibilityLabel();
}

void BatteryWidget::hideEvent(QHideEvent* event)
{
    LiquidGlassWidget::hideEvent(event);
    for (BatteryWidget* widget : s_instances)
        widget->updateTrayVisibilityLabel();
}

QIcon BatteryWidget::createTrayIcon() const
{
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawBatteryIcon(painter, QRectF(3, 7, 26, 18),
                    m_hasBattery ? m_level : -1, m_charging);
    return QIcon(pixmap);
}

void BatteryWidget::setupTrayIcon()
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
    connect(m_visibilityAction, &QAction::triggered, this, &BatteryWidget::toggleWidgetVisibility);

    auto* libraryAction = menu->addAction(QStringLiteral("添加小组件…"));
    connect(libraryAction, &QAction::triggered, this, &BatteryWidget::showWidgetLibrary);

    m_layoutAction = menu->addAction(QString());
    connect(m_layoutAction, &QAction::triggered, this, &BatteryWidget::toggleLayout);

    auto* settingsAction = menu->addAction(QStringLiteral("设置…"));
    connect(settingsAction, &QAction::triggered, this, &BatteryWidget::showSettingsDialog);

    m_startupAction = menu->addAction(QStringLiteral("开机自启动"));
    m_startupAction->setCheckable(true);
    m_startupAction->setChecked(isStartupEnabled());
    connect(m_startupAction, &QAction::toggled, this, &BatteryWidget::setStartupEnabled);

    menu->addSeparator();
    auto* aboutAction = menu->addAction(QStringLiteral("关于 macdowsOS Widget"));
    connect(aboutAction, &QAction::triggered, this, &BatteryWidget::showAboutDialog);
    menu->addSeparator();
    auto* quitAction = menu->addAction(QStringLiteral("退出"));
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setIcon(createTrayIcon());
    m_trayIcon->setToolTip(m_hasBattery
                               ? QStringLiteral("macdowsOS Widget  ·  %1%").arg(m_level)
                               : QStringLiteral("macdowsOS Widget  ·  电源适配器"));
    m_trayIcon->setContextMenu(menu);
    m_trayClickTimer.setSingleShot(true);
    m_trayClickTimer.setInterval(QApplication::doubleClickInterval());
    connect(&m_trayClickTimer, &QTimer::timeout, this, &BatteryWidget::toggleWidgetVisibility);
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
    updateLayoutLabel();
}

void BatteryWidget::updateTrayVisibilityLabel()
{
    if (!m_visibilityAction)
        return;
    bool anyVisible = false;
    for (BatteryWidget* widget : s_instances)
        anyVisible = anyVisible || widget->isVisible();
    m_visibilityAction->setText(anyVisible ? QStringLiteral("隐藏所有组件")
                                           : QStringLiteral("显示所有组件"));
}

void BatteryWidget::toggleWidgetVisibility()
{
    bool anyVisible = false;
    for (BatteryWidget* widget : s_instances)
        anyVisible = anyVisible || widget->isVisible();
    for (BatteryWidget* widget : s_instances) {
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

void BatteryWidget::updateLayoutLabel()
{
    if (m_layoutAction)
        m_layoutAction->setText(QStringLiteral("重新排列组件"));
}

void BatteryWidget::toggleLayout()
{
    arrangeGroup();
    saveAllConfigurations();
    updateLayoutLabel();
}

void BatteryWidget::applyScale(qreal scale)
{
    m_uiScale = qBound<qreal>(0.40, scale, 2.0);
    QSettings settings;
    settings.setValue(QStringLiteral("appearance/scale"), m_uiScale);
    setGlassRadius(34.0 * m_uiScale);
    const int cell = qMax(1, qRound(kGridBaseCell * m_uiScale));
    QScreen* currentScreen = QGuiApplication::screenAt(geometry().center());
    if (!currentScreen)
        currentScreen = QGuiApplication::primaryScreen();
    const int gap = currentScreen
        ? gridGapForArea(currentScreen->availableGeometry(), m_uiScale)
        : qMax(4, qRound(kGridBaseGap * m_uiScale));
    setFixedSize(gridWidgetSize(m_kind, m_variant, cell, gap));
    moveToGroupPosition();
    updateDashboardObjects();
    captureDesktopBackdrop();
    update();
}

void BatteryWidget::updateDashboardObjects()
{
    // Every window contains exactly one full-size glass object. The reusable
    // base owns its corner, mask and wallpaper crop; this class only paints
    // the card payload selected by m_kind.
    setGlassObjectVisible(0, true);
    setGlassObjectGeometry(0, QPointF(0, 0), QSizeF(width(), height()));
    setGlassObjectCornerRadius(0, float(34.0 * m_uiScale));
}

void BatteryWidget::moveToGroupPosition()
{
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;
    const QRect area = screen->availableGeometry();
    const int cell = qMax(1, qRound(kGridBaseCell * m_uiScale));
    const int gap = gridGapForArea(area, m_uiScale);
    const int pitch = cell + gap;
    // Count cells inside the two outer gutters. The equivalent expression is
    // floor((area.width() - gap) / pitch): n cells plus n-1 internal gaps,
    // with one additional gap reserved at the far edge.
    const int columns = qMax(1, (area.width() - gap) / pitch);
    const int rows = qMax(1, (area.height() - gap) / pitch);
    const int span = gridColumnSpan(m_kind, m_variant);
    int column = qMax(0, columns - span);
    const int rowSpan = gridRowSpan(m_kind, m_variant);
    int row = qMax(0, rows - rowSpan);
    switch (m_kind) {
    case CardKind::Battery:
        row = m_variant == 1 ? qMax(0, rows - 1) : qMax(0, rows - rowSpan); break;
    case CardKind::Weather:
        row = qMax(0, rows - rowSpan); break;
    case CardKind::Clock:
        row = qMax(0, rows - rowSpan); break;
    case CardKind::Dictionary:
        row = qMax(0, rows - 1); break;
    }
    const QRect target = nearestGridRect(m_kind, m_variant,
        gridOrigin(area, gap) + QPoint(column * pitch, row * pitch), m_uiScale, this);
    if (target.isValid()) {
        m_restoringPosition = true;
        setFixedSize(target.size());
        move(target.topLeft());
        m_restoringPosition = false;
    }
}

void BatteryWidget::arrangeGroup()
{
    // Vacate old cells first so the deterministic layout cannot be blocked by
    // the widgets' own previous arrangement.
    int offset = 0;
    QHash<BatteryWidget*, QPoint> previous;
    for (BatteryWidget* widget : s_instances) {
        previous.insert(widget, widget->pos());
        widget->m_restoringPosition = true;
        widget->move(-100000 - offset, -100000 - offset);
        widget->m_restoringPosition = false;
        offset += 8;
    }
    for (BatteryWidget* widget : s_instances) {
        widget->moveToGroupPosition();
        if (!QGuiApplication::screenAt(widget->geometry().center())) {
            // An overfull grid must never strand a card at the temporary
            // off-screen staging coordinate.
            for (BatteryWidget* restore : s_instances)
                restore->move(previous.value(restore));
            QToolTip::showText(QCursor::pos(), QStringLiteral("当前屏幕空间不足，请缩小组件后重新排列"));
            break;
        }
        widget->captureDesktopBackdrop();
    }
}

void BatteryWidget::syncGroupSettings(qreal scale, bool systemBlur, int blurStrength,
                                      qreal opacity, bool mouseThrough, bool lowPower,
                                      bool liveBackdrop, qreal renderQuality, int fontSmoothing,
                                      RenderBackend renderBackend)
{
    QSettings settings;
    settings.setValue(QStringLiteral("performance/renderBackend"), int(renderBackend));
    settings.setValue(QStringLiteral("performance/renderScale"), renderQuality);
    settings.setValue(QStringLiteral("appearance/liveBackdrop"), liveBackdrop);
    settings.setValue(QStringLiteral("appearance/fontSmoothing"), fontSmoothing);
    setGlobalFontSmoothing(fontSmoothing);
    // Changing material, opacity or input mode must not alter a user's
    // carefully placed cards. Reflow only when the scale actually changes,
    // because that is the one setting that changes the grid geometry.
    bool scaleChanged = false;
    for (BatteryWidget* widget : s_instances) {
        if (widget && !qFuzzyCompare(widget->m_uiScale, scale)) {
            scaleChanged = true;
            break;
        }
    }
    for (BatteryWidget* widget : s_instances) {
        if (scaleChanged)
            widget->applyScale(scale);
        widget->setSystemBlurEnabled(systemBlur);
        widget->setMaterialBlurStrength(blurStrength);
        widget->setGlassOpacity(opacity);
        widget->setMouseThroughEnabled(mouseThrough);
        widget->setLowPowerRefreshEnabled(lowPower);
        widget->setLiveBackdropEnabled(liveBackdrop);
        widget->setRenderScale(float(renderQuality));
        widget->setRenderBackend(renderBackend);
    }
    if (scaleChanged)
        arrangeGroup();
    saveAllConfigurations();
}

QString BatteryWidget::kindName(CardKind kind)
{
    switch (kind) {
    case CardKind::Battery: return QStringLiteral("battery");
    case CardKind::Weather: return QStringLiteral("weather");
    case CardKind::Clock: return QStringLiteral("clock");
    case CardKind::Dictionary: return QStringLiteral("dictionary");
    }
    return QStringLiteral("battery");
}

BatteryWidget::CardKind BatteryWidget::kindFromName(const QString& value)
{
    if (value == QStringLiteral("weather")) return CardKind::Weather;
    if (value == QStringLiteral("clock")) return CardKind::Clock;
    if (value == QStringLiteral("dictionary")) return CardKind::Dictionary;
    return CardKind::Battery;
}

QString BatteryWidget::configurationPath()
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(directory);
    return QDir(directory).filePath(QStringLiteral("widgets.json"));
}

void BatteryWidget::saveConfiguration() const
{
    saveAllConfigurations();
}

void BatteryWidget::saveAllConfigurations()
{
    QJsonArray widgets;
    for (BatteryWidget* widget : s_instances) {
        if (!widget)
            continue;
        QJsonObject item;
        item.insert(QStringLiteral("id"), widget->m_instanceId);
        item.insert(QStringLiteral("type"), kindName(widget->m_kind));
        item.insert(QStringLiteral("variant"), widget->m_variant);
        item.insert(QStringLiteral("x"), widget->x());
        item.insert(QStringLiteral("y"), widget->y());
        item.insert(QStringLiteral("visible"), widget->isVisible());
        QScreen* screen = QGuiApplication::screenAt(widget->geometry().center());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        if (screen) {
            const QRect area = screen->availableGeometry();
            const int cell = qMax(1, qRound(kGridBaseCell * widget->m_uiScale));
            const int gap = gridGapForArea(area, widget->m_uiScale);
            const int pitch = cell + gap;
            item.insert(QStringLiteral("screen"), screen->name());
            item.insert(QStringLiteral("gridColumn"),
                        qRound((widget->x() - (area.left() + gap)) / qreal(pitch)));
            item.insert(QStringLiteral("gridRow"),
                        qRound((widget->y() - (area.top() + gap)) / qreal(pitch)));
        }
        item.insert(QStringLiteral("columnSpan"), gridColumnSpan(widget->m_kind, widget->m_variant));
        item.insert(QStringLiteral("rowSpan"), gridRowSpan(widget->m_kind, widget->m_variant));
        widgets.append(item);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 3);
    root.insert(QStringLiteral("scale"), s_instances.isEmpty() ? 1.0 : s_instances.first()->m_uiScale);
    root.insert(QStringLiteral("gridCellWidth"), s_instances.isEmpty() ? kGridBaseCell
        : qRound(kGridBaseCell * s_instances.first()->m_uiScale));
    root.insert(QStringLiteral("gridCellHeight"), s_instances.isEmpty() ? kGridBaseCell
        : qRound(kGridBaseCell * s_instances.first()->m_uiScale));
    const qreal rootScale = s_instances.isEmpty() ? 1.0 : s_instances.first()->m_uiScale;
    QScreen* rootScreen = QGuiApplication::primaryScreen();
    root.insert(QStringLiteral("gridGap"), rootScreen
        ? gridGapForArea(rootScreen->availableGeometry(), rootScale)
        : qMax(4, qRound(kGridBaseGap * rootScale)));
    root.insert(QStringLiteral("widgets"), widgets);
    QSaveFile file(configurationPath());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.commit();
    }
}

QPoint BatteryWidget::snappedTopLeft(const QPoint& requested) const
{
    return nearestGridRect(m_kind, m_variant, requested, m_uiScale, this).topLeft();
}

QRect BatteryWidget::nearestGridRect(CardKind kind, int variant,
                                     const QPoint& requestedTopLeft, qreal scale,
                                     const BatteryWidget* ignored)
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
    // Reserve one gap on every outer edge, just as between cells. A candidate
    // is valid only when its right/bottom edge also stays inside that gutter.
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
            for (BatteryWidget* widget : s_instances) {
                // Hidden cards keep their last geometry for restoration, but
                // they must not reserve desktop cells while they are absent.
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

    // No valid occupancy remains. Returning an invalid rectangle lets a live
    // drag return to its origin and lets a library drop reject creation.
    return QRect();
}

void BatteryWidget::showGridPreview(CardKind kind, int variant, const QPoint& desktopPoint)
{
    QSettings settings;
    const qreal scale = qBound<qreal>(0.40,
        settings.value(QStringLiteral("appearance/scale"), 1.0).toDouble(), 2.0);
    QScreen* screen = QGuiApplication::screenAt(desktopPoint);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect area = screen ? screen->availableGeometry() : QRect();
    const int cell = qMax(1, qRound(kGridBaseCell * scale));
    const int gap = gridGapForArea(area, scale);
    const QSize size = gridWidgetSize(kind, variant, cell, gap);
    showGridPreviewRect(nearestGridRect(kind, variant,
        desktopPoint - QPoint(size.width() / 2, size.height() / 2), scale));
}

void BatteryWidget::showGridPreviewRect(const QRect& rect)
{
    if (!rect.isValid()) {
        hideGridPreview();
        return;
    }
    gridOverlay()->display(rect);
}

void BatteryWidget::hideGridPreview()
{
    gridOverlay()->dismiss();
    gridFadeOverlay()->dismiss();
}

bool BatteryWidget::placeAtDesktopPoint(const QPoint& point)
{
    // Center the card under the drop cursor, then snap its top-left to the
    // nearest scale-aware grid cell.
    const QPoint desired = point - QPoint(width() / 2, height() / 2);
    const QRect target = nearestGridRect(m_kind, m_variant, desired, m_uiScale, this);
    if (!target.isValid())
        return false;
    m_restoringPosition = true;
    setFixedSize(target.size());
    move(target.topLeft());
    m_restoringPosition = false;
    m_hasSavedPosition = true;
    captureDesktopBackdrop();
    saveAllConfigurations();
    return true;
}

BatteryWidget* BatteryWidget::addWidget(CardKind kind, const QPoint& desktopPoint, int variant)
{
    auto* widget = new BatteryWidget(nullptr, kind, false, QString(), variant);
    widget->setAttribute(Qt::WA_DeleteOnClose, true);
    if (!widget->placeAtDesktopPoint(desktopPoint)) {
        delete widget;
        return nullptr;
    }
    widget->show();
    saveAllConfigurations();
    return widget;
}

QImage BatteryWidget::renderPreview(CardKind kind, int variant, const QSize& size)
{
    if (size.isEmpty())
        return {};
    BatteryWidget preview(nullptr, kind, false,
                          QStringLiteral("gallery-preview"), variant);
    preview.setDesktopLayerEnabled(false);
    preview.setDesktopCaptureEnabled(false);
    preview.setAnimationEnabled(false);
    // Preserve the real grid aspect instead of stretching every style into
    // the gallery's common preview rectangle.
    const QSize reference = gridWidgetSize(kind, variant, kGridBaseCell,
                                           qRound(kGridBaseGap));
    QSize renderSize = reference;
    renderSize.scale(size, Qt::KeepAspectRatio);
    renderSize = renderSize.expandedTo(QSize(1, 1));
    // Supersample the shared renderer before the gallery scales it into its
    // tile. This keeps CJK and numeric glyph edges crisp instead of enlarging
    // a low-resolution intermediate image.
    renderSize *= 2;
    preview.m_uiScale = qMin(renderSize.width() / qreal(reference.width()),
                             renderSize.height() / qreal(reference.height()));
    preview.setMinimumSize(0, 0);
    preview.setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    preview.resize(renderSize);

    // Do not grab an unseen QOpenGLWidget: on some drivers its framebuffer is
    // not allocated until native exposure, yielding the blank cards reported
    // in the gallery. paintOverlay() is the exact desktop payload renderer and
    // is deterministic on an ordinary image paint device.
    QImage image(renderSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(36, 54, 76, 150));
    const qreal previewRadius = 34.0 * preview.m_uiScale;
    painter.drawRoundedRect(QRectF(image.rect()), previewRadius, previewRadius);
    preview.paintOverlay(painter);
    painter.end();
    return image;
}

QList<BatteryWidget*> BatteryWidget::restoreWidgets()
{
    QList<BatteryWidget*> result;
    QFile file(configurationPath());
    if (!file.open(QIODevice::ReadOnly))
        return result;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonArray widgets = document.object().value(QStringLiteral("widgets")).toArray();
    bool primaryAssigned = false;
    for (const QJsonValue& value : widgets) {
        const QJsonObject item = value.toObject();
        const QString type = item.value(QStringLiteral("type")).toString();
        if (type != QStringLiteral("battery") && type != QStringLiteral("weather")
            && type != QStringLiteral("clock") && type != QStringLiteral("dictionary"))
            continue;
        const CardKind kind = kindFromName(type);
        // Keep the tray/settings owner on the battery card even when the
        // configuration was reordered after adding custom cards.
        const bool primary = kind == CardKind::Battery && !primaryAssigned;
        const int storedVariant = item.contains(QStringLiteral("variant"))
            ? item.value(QStringLiteral("variant")).toInt() : defaultVariant(kind);
        auto* widget = new BatteryWidget(nullptr,
                                         kind,
                                         primary,
                                         item.value(QStringLiteral("id")).toString(),
                                         storedVariant);
        QPoint restoredPosition(item.value(QStringLiteral("x")).toInt(),
                                item.value(QStringLiteral("y")).toInt());
        if (item.contains(QStringLiteral("gridColumn"))
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
                const int cell = qMax(1, qRound(kGridBaseCell * widget->m_uiScale));
                const QRect targetArea = targetScreen->availableGeometry();
                const int gap = gridGapForArea(targetArea, widget->m_uiScale);
                const int pitch = cell + gap;
                restoredPosition = gridOrigin(targetArea, gap)
                    + QPoint(item.value(QStringLiteral("gridColumn")).toInt() * pitch,
                             item.value(QStringLiteral("gridRow")).toInt() * pitch);
            }
        }
        const QRect restoredTarget = nearestGridRect(kind, widget->m_variant, restoredPosition,
                                                     widget->m_uiScale, widget);
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
        // A legacy config without a battery still needs a tray owner.
        result.first()->m_primary = true;
        result.first()->setupTrayIcon();
    }
    // Migrate legacy x/y-only files to explicit grid coordinates once all
    // occupancy decisions are final.
    if (!result.isEmpty())
        saveAllConfigurations();
    return result;
}

void BatteryWidget::showWidgetLibrary()
{
    if (!m_library) {
        // A topmost tool must not be natively owned by a desktop card.
        m_library = new WidgetLibraryDialog;
        connect(m_library, &WidgetLibraryDialog::widgetDropped, this,
                [](int encoded, const QPoint& globalPos) {
                    const CardKind kind = static_cast<CardKind>(qBound(0, encoded / 100, 3));
                    const int variant = encoded % 100;
                    if (!addWidget(kind, globalPos, variant))
                        QToolTip::showText(globalPos, QStringLiteral("当前屏幕没有足够的空位，可缩小组件后重试"));
                });
        connect(m_library, &WidgetLibraryDialog::widgetDragStarted, this,
                [this](int) {
                    if (!m_libraryDragHiddenWindows.isEmpty())
                        return;
                    QSet<WId> widgetWindows;
                    for (BatteryWidget* widget : std::as_const(s_instances)) {
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
                        showGridPreview(static_cast<CardKind>(qBound(0, encoded / 100, 3)),
                                        encoded % 100, globalPos);
                    else
                        hideGridPreview();
                });
    }
    // The gallery can outlive a settings dialog. Reapply the current global
    // backdrop and quality settings every time it is opened.
    m_library->setLiveBackdropEnabled(liveBackdropEnabled());
    m_library->setRenderScale(renderScale());
    if (!m_library->isVisible()) {
        m_library->prepareBackdrop();
        m_library->show();
    }
    m_library->raise();
    m_library->activateWindow();
}

void BatteryWidget::refreshWeather()
{
    if (!m_weatherNetwork)
        return;

    ++m_weatherReplySerial;
    if (m_weatherReply)
        m_weatherReply->abort();
    const QString cityId = m_weatherCityId.isEmpty()
        ? QStringLiteral("101010100") : m_weatherCityId;
    QUrl url(QStringLiteral("https://d1.weather.com.cn/weather_index/%1.html").arg(cityId));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 macdowsOS Widget/1.0"));
    request.setRawHeader("Referer", "https://www.weather.com.cn/");
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    m_weatherLoading = true;
    // Keep the last complete snapshot on screen while the three weather
    // endpoints refresh. Replacing the description with a loading string or
    // clearing the hourly/daily arrays here caused a visible flash every
    // refresh (and was especially obvious on the wide cards).
    if (m_weatherTemperature.isEmpty()) {
        m_weatherDescription = QStringLiteral("获取天气中…");
        update();
    }
    QNetworkReply* reply = m_weatherNetwork->get(request);
    m_weatherReply = reply;
    reply->setProperty("weatherSerial", m_weatherReplySerial);
    // Avoid keeping a stale network reply alive indefinitely when a captive
    // portal or offline connection never completes.
    QTimer::singleShot(15000, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });
}

void BatteryWidget::handleWeatherReply(QNetworkReply* reply)
{
    if (!reply)
        return;
    // Search replies have their own dialog-scoped handler. They share the
    // network manager but must not be interpreted as weather payloads here.
    if (reply->property("weatherSearch").toBool())
        return;
    const int serial = reply->property("weatherSerial").toInt();
    if (serial != m_weatherReplySerial) {
        reply->deleteLater();
        return;
    }

    if (reply->property("weatherDaily").toBool()) {
        const QString html = QString::fromUtf8(reply->readAll());
        const QRegularExpression liExpression(
            QStringLiteral(R"(<li\b[^>]*class\s*=\s*["']sky[^"']*["'][^>]*>(.*?)</li>)"),
            QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);
        const QRegularExpression codeExpression(
            QStringLiteral(R"(<big\b[^>]*class\s*=\s*["'][^"']*\bd(\d{1,2})\b[^"']*["'])"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpression descriptionExpression(
            QStringLiteral(R"(<p\b[^>]*class\s*=\s*["'][^"']*\bwea\b[^"']*["'][^>]*>(.*?)</p>)"),
            QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);
        const QRegularExpression temperatureExpression(
            QStringLiteral(R"(<p\b[^>]*class\s*=\s*["'][^"']*\btem\b[^"']*["'][^>]*>.*?<span[^>]*>\s*(-?\d+)\s*</span>\s*/\s*<i[^>]*>\s*(-?\d+))"),
            QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);
        QVector<WeatherDay> days;
        QRegularExpressionMatchIterator iterator = liExpression.globalMatch(html);
        int index = 0;
        while (iterator.hasNext() && days.size() < 5) {
            const QString card = iterator.next().captured(1);
            // The first card is today. The reference 2×2 card intentionally
            // starts at tomorrow and shows the next five complete days.
            if (index++ == 0)
                continue;
            const QRegularExpressionMatch codeMatch = codeExpression.match(card);
            const QRegularExpressionMatch descriptionMatch =
                descriptionExpression.match(card);
            const QRegularExpressionMatch temperatureMatch =
                temperatureExpression.match(card);
            if (!temperatureMatch.hasMatch())
                continue;
            WeatherDay item;
            item.label = chineseWeekdayLabel(QDate::currentDate().addDays(days.size() + 1));
            item.high = temperatureMatch.captured(1).trimmed();
            item.low = temperatureMatch.captured(2).trimmed();
            item.description = descriptionMatch.hasMatch()
                ? descriptionMatch.captured(1).simplified() : QString();
            item.code = codeMatch.hasMatch() ? codeMatch.captured(1).toInt() : -1;
            item.night = false;
            days.append(item);
        }
        if (days.size() >= 4)
            m_weatherDays = days;
        reply->deleteLater();
        update();
        return;
    }

    if (reply->property("weatherHourly").toBool()) {
        const QByteArray payload = reply->readAll();
        const QJsonObject hourlyRoot = jsonObjectAfterMarker(payload, "var hour3data");
        QVector<WeatherHour> hours;
        const QJsonArray hourlyValues = hourlyRoot.value(QStringLiteral("1d")).toArray();
        for (const QJsonValue& value : hourlyValues) {
            const QStringList fields = value.toString().split(QLatin1Char(','));
            if (fields.size() < 4)
                continue;
            WeatherHour item;
            item.time = fields.at(0).trimmed();
            // The service prefixes the hour with the day (for example
            // "15日11时"). The reference cards show the compact hour only.
            const int hourMarker = item.time.lastIndexOf(QStringLiteral("日"));
            if (hourMarker >= 0)
                item.time = item.time.mid(hourMarker + 1).trimmed();
            item.code = numericWeatherCode(fields.at(1));
            item.description = fields.at(2).trimmed();
            item.temperature = weatherValue(fields.at(3));
            item.night = fields.at(1).trimmed().startsWith(QLatin1Char('n'));
            hours.append(item);
            if (hours.size() == 6)
                break;
        }
        if (!hours.isEmpty())
            m_weatherHours = hours;
        reply->deleteLater();
        update();
        return;
    }

    const QByteArray payload = reply->readAll();
    const QJsonObject current = jsonObjectAfterMarker(payload, "var dataSK");
    const QJsonObject forecastRoot = jsonObjectAfterMarker(payload, "var fc");
    const QJsonObject cityRoot = jsonObjectAfterMarker(payload, "var cityDZ");
    const bool ok = reply->error() == QNetworkReply::NoError
        && !current.isEmpty() && !forecastRoot.isEmpty();
    if (ok) {
        const QJsonObject info = cityRoot.value(QStringLiteral("weatherinfo")).toObject();
        const QString apiLocation = info.value(QStringLiteral("city")).toString().trimmed();
        // Keep the exact district label chosen in the search dialog (for
        // example “天心区” or “浦东新区”). The default city can be canonicalized
        // from the service, while a non-default city ID already carries the
        // user's precise selection and should not be shortened by the API.
        if (m_weatherLocation.isEmpty() || m_weatherCityId == QStringLiteral("101010100"))
            m_weatherLocation = apiLocation;
        if (m_weatherLocation.isEmpty())
            m_weatherLocation = current.value(QStringLiteral("cityname")).toString().trimmed();
        m_weatherTemperature = weatherValue(current.value(QStringLiteral("temp")));
        m_weatherDescription = current.value(QStringLiteral("weather")).toString().trimmed();
        if (m_weatherDescription.isEmpty())
            m_weatherDescription = info.value(QStringLiteral("weather")).toString().trimmed();
        m_weatherCode = numericWeatherCode(current.value(QStringLiteral("weathercode")).toString());
        if (m_weatherCode < 0)
            m_weatherCode = numericWeatherCode(info.value(QStringLiteral("weathercode")).toString());
        const QString nightCode = info.value(QStringLiteral("weathercoden")).toString();
        m_weatherNight = nightCode.startsWith(QLatin1Char('n'))
                      || QTime::currentTime().hour() < 6
                      || QTime::currentTime().hour() >= 18;

        QVector<WeatherDay> parsedDays;
        const QJsonArray days = forecastRoot.value(QStringLiteral("f")).toArray();
        for (const QJsonValue& value : days) {
            const QJsonObject day = value.toObject();
            WeatherDay item;
            item.label = normalizedWeekdayLabel(
                day.value(QStringLiteral("fj")).toString());
            if (item.label.isEmpty())
                item.label = day.value(QStringLiteral("fi")).toString().trimmed();
            item.high = weatherValue(day.value(QStringLiteral("fc")));
            item.low = weatherValue(day.value(QStringLiteral("fd")));
            item.description = day.value(QStringLiteral("fa")).toString().trimmed();
            item.code = numericWeatherCode(day.value(QStringLiteral("fa")).toString());
            item.night = day.value(QStringLiteral("fb")).toString().trimmed()
                             .startsWith(QLatin1Char('n'));
            parsedDays.append(item);
        }
        if (!parsedDays.isEmpty()) {
            m_weatherHigh = parsedDays.first().high;
            m_weatherLow = parsedDays.first().low;
            // Swap the complete list in one operation so daily rows never
            // render in a partially cleared state.
            m_weatherDays = std::move(parsedDays);
        }
        m_weatherLoading = false;
        m_weatherUpdated = QDateTime::currentDateTime();

        QNetworkRequest hourlyRequest(QUrl(
            QStringLiteral("https://www.weather.com.cn/weather1d/%1.shtml")
                .arg(m_weatherCityId)));
        hourlyRequest.setHeader(QNetworkRequest::UserAgentHeader,
                                QStringLiteral("Mozilla/5.0 macdowsOS Widget/1.0"));
        hourlyRequest.setRawHeader("Referer", "https://www.weather.com.cn/");
        hourlyRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                                   QNetworkRequest::AlwaysNetwork);
        QNetworkReply* hourlyReply = m_weatherNetwork->get(hourlyRequest);
        m_weatherReply = hourlyReply;
        hourlyReply->setProperty("weatherSerial", m_weatherReplySerial);
        hourlyReply->setProperty("weatherHourly", true);
        QTimer::singleShot(15000, hourlyReply, [hourlyReply]() {
            if (hourlyReply->isRunning())
                hourlyReply->abort();
        });

        // The index endpoint contains five days including today. The sample
        // 2×2 card starts at tomorrow and shows five rows, so also request
        // the site's seven-day page and use it when available.
        QNetworkRequest dailyRequest(QUrl(
            QStringLiteral("https://www.weather.com.cn/weather/%1.shtml")
                .arg(m_weatherCityId)));
        dailyRequest.setHeader(QNetworkRequest::UserAgentHeader,
                               QStringLiteral("Mozilla/5.0 macdowsOS Widget"));
        dailyRequest.setRawHeader("Referer", "https://www.weather.com.cn/");
        dailyRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                                  QNetworkRequest::AlwaysNetwork);
        QNetworkReply* dailyReply = m_weatherNetwork->get(dailyRequest);
        dailyReply->setProperty("weatherSerial", m_weatherReplySerial);
        dailyReply->setProperty("weatherDaily", true);
        QTimer::singleShot(15000, dailyReply, [dailyReply]() {
            if (dailyReply->isRunning())
                dailyReply->abort();
        });
    } else {
        m_weatherLoading = false;
        m_weatherDescription = m_weatherTemperature.isEmpty()
            ? QStringLiteral("天气暂不可用") : QStringLiteral("更新失败 · 显示上次天气");
    }
    reply->deleteLater();
    update();
}

void BatteryWidget::refreshDictionary()
{
    if (!m_dictionaryNetwork)
        return;
    ++m_dictionaryReplySerial;
    if (m_dictionaryReply)
        m_dictionaryReply->abort();
    // Pick from a small curated set of real words, then obtain the phonetic,
    // part of speech and definition from the online dictionary. This avoids a
    // second random-word service (which is often rate-limited) while keeping
    // each refresh genuinely random and fully data-backed.
    static const QStringList words = {
        QStringLiteral("serendipity"), QStringLiteral("luminous"),
        QStringLiteral("wanderlust"), QStringLiteral("ephemeral"),
        QStringLiteral("resilient"), QStringLiteral("curiosity"),
        QStringLiteral("harmony"), QStringLiteral("diligent"),
        QStringLiteral("nostalgia"), QStringLiteral("vivid"),
        QStringLiteral("whisper"), QStringLiteral("breeze")
    };
    m_dictionaryWord = words.at(int(QRandomGenerator::global()->bounded(words.size())));
    QNetworkRequest request(QUrl(QStringLiteral("https://api.dictionaryapi.dev/api/v2/entries/en/%1")
                                  .arg(QString::fromUtf8(QUrl::toPercentEncoding(m_dictionaryWord)))));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("macdowsOS Widget/1.0 (Qt)"));
    QNetworkReply* reply = m_dictionaryNetwork->get(request);
    m_dictionaryReply = reply;
    reply->setProperty("dictionarySerial", m_dictionaryReplySerial);
    reply->setProperty("dictionaryWord", m_dictionaryWord);
    m_dictionaryPhonetic.clear();
    m_dictionaryPartOfSpeech.clear();
    m_dictionaryDefinition = QStringLiteral("读取在线释义中…");
    update();
    QTimer::singleShot(15000, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });
}

void BatteryWidget::handleDictionaryReply(QNetworkReply* reply)
{
    if (!reply)
        return;
    const int serial = reply->property("dictionarySerial").toInt();
    if (serial != m_dictionaryReplySerial) {
        reply->deleteLater();
        return;
    }
    const QString word = reply->property("dictionaryWord").toString();
    m_dictionaryDefinition.clear();
    bool ok = reply->error() == QNetworkReply::NoError;
    const QJsonDocument document = ok ? QJsonDocument::fromJson(reply->readAll()) : QJsonDocument();
    const QJsonArray entries = document.isArray() ? document.array() : QJsonArray();
    if (ok && !entries.isEmpty()) {
        const QJsonObject entry = entries.first().toObject();
        m_dictionaryPhonetic = entry.value(QStringLiteral("phonetic")).toString();
        const QJsonArray phonetics = entry.value(QStringLiteral("phonetics")).toArray();
        if (m_dictionaryPhonetic.isEmpty() && !phonetics.isEmpty())
            m_dictionaryPhonetic = phonetics.first().toObject().value(QStringLiteral("text")).toString();
        const QJsonArray meanings = entry.value(QStringLiteral("meanings")).toArray();
        if (!meanings.isEmpty()) {
            const QJsonObject meaning = meanings.first().toObject();
            m_dictionaryPartOfSpeech = meaning.value(QStringLiteral("partOfSpeech")).toString();
            const QJsonArray definitions = meaning.value(QStringLiteral("definitions")).toArray();
            if (!definitions.isEmpty())
                m_dictionaryDefinition = definitions.first().toObject()
                                             .value(QStringLiteral("definition")).toString();
        }
    }
    if (!ok || m_dictionaryDefinition.isEmpty()) {
        static const QHash<QString, QString> localDefinitions = {
            {QStringLiteral("breeze"), QStringLiteral("微风；轻柔的风")},
            {QStringLiteral("diligent"), QStringLiteral("勤奋的；孜孜不倦的")},
            {QStringLiteral("serendipity"), QStringLiteral("意外发现美好事物的运气")},
            {QStringLiteral("luminous"), QStringLiteral("发光的；明亮的")},
            {QStringLiteral("wanderlust"), QStringLiteral("旅行癖；对远方的向往")},
            {QStringLiteral("ephemeral"), QStringLiteral("短暂的；转瞬即逝的")},
            {QStringLiteral("resilient"), QStringLiteral("有韧性的；能迅速恢复的")},
            {QStringLiteral("curiosity"), QStringLiteral("好奇心；求知欲")},
            {QStringLiteral("harmony"), QStringLiteral("和谐；协调")},
            {QStringLiteral("nostalgia"), QStringLiteral("怀旧；对往昔的眷恋")},
            {QStringLiteral("vivid"), QStringLiteral("生动的；鲜明的")},
            {QStringLiteral("whisper"), QStringLiteral("低语；轻声说话")}
        };
        m_dictionaryDefinition = localDefinitions.value(
            m_dictionaryWord, QStringLiteral("释义暂不可用，可点击“随机单词”重试"));
        if (m_dictionaryPartOfSpeech.isEmpty())
            m_dictionaryPartOfSpeech = QStringLiteral("词义速览");
    }
    if (!word.isEmpty())
        m_dictionaryWord = word;
    reply->deleteLater();
    update();
}

void BatteryWidget::searchWeatherCity()
{
    if (m_kind != CardKind::Weather)
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("搜索城市或区县"));
    dialog.setModal(true);
    dialog.resize(520, 440);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(10);
    auto* title = new QLabel(QStringLiteral("搜索城市、区或县"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size:18px;font-weight:600;"));
    layout->addWidget(title);
    auto* hint = new QLabel(QStringLiteral("输入简体中文名称，可精确选择区县；例如“海淀”“天心”“浦东新区”。"),
                            &dialog);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#9da8bd;"));
    layout->addWidget(hint);
    auto* searchRow = new QWidget(&dialog);
    auto* searchLayout = new QHBoxLayout(searchRow);
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->setSpacing(8);
    auto* edit = new QLineEdit(m_weatherLocation, searchRow);
    edit->setPlaceholderText(QStringLiteral("输入城市、区或县"));
    edit->selectAll();
    auto* searchButton = new QPushButton(QStringLiteral("搜索"), searchRow);
    searchLayout->addWidget(edit, 1);
    searchLayout->addWidget(searchButton);
    layout->addWidget(searchRow);
    auto* resultLabel = new QLabel(QStringLiteral("输入名称后点击搜索"), &dialog);
    resultLabel->setStyleSheet(QStringLiteral("color:#aeb9cb;"));
    layout->addWidget(resultLabel);
    auto* results = new QListWidget(&dialog);
    results->setAlternatingRowColors(false);
    layout->addWidget(results, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* chooseButton = buttons->addButton(QStringLiteral("使用所选地区"),
                                             QDialogButtonBox::AcceptRole);
    chooseButton->setEnabled(false);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(results, &QListWidget::itemSelectionChanged, &dialog,
            [results, chooseButton]() { chooseButton->setEnabled(results->currentItem()); });
    connect(results, &QListWidget::itemDoubleClicked, &dialog,
            [&dialog](QListWidgetItem*) { dialog.accept(); });
    connect(chooseButton, &QPushButton::clicked, &dialog, [&dialog, results]() {
        if (results->currentItem())
            dialog.accept();
    });

    QPointer<QNetworkReply> searchReply;
    const auto performSearch = [this, edit, results, resultLabel, searchButton,
                                chooseButton, &searchReply]() {
        const QString originalQuery = edit->text().trimmed();
        if (originalQuery.isEmpty()) {
            resultLabel->setText(QStringLiteral("请输入城市、区或县名称"));
            return;
        }
        QString query = originalQuery;
        if (query.size() > 1 && query.endsWith(QStringLiteral("市")))
            query.chop(1);
        if (query.size() > 1 && query.endsWith(QStringLiteral("区"))
            && !query.endsWith(QStringLiteral("新区")))
            query.chop(1);
        results->clear();
        chooseButton->setEnabled(false);
        resultLabel->setText(QStringLiteral("正在搜索…"));
        searchButton->setEnabled(false);
        if (searchReply)
            searchReply->abort();
        QUrl url(QStringLiteral("https://toy1.weather.com.cn/search"));
        QUrlQuery urlQuery;
        urlQuery.addQueryItem(QStringLiteral("cityname"), query);
        urlQuery.addQueryItem(QStringLiteral("_"),
                              QString::number(QDateTime::currentMSecsSinceEpoch()));
        url.setQuery(urlQuery);
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("Mozilla/5.0 macdowsOS Widget/1.0"));
        request.setRawHeader("Referer", "https://www.weather.com.cn/");
        request.setTransferTimeout(15000);
        QNetworkReply* issuedReply = m_weatherNetwork->get(request);
        searchReply = issuedReply;
        issuedReply->setProperty("weatherSearch", true);
        connect(issuedReply, &QNetworkReply::finished, results,
                [results, resultLabel, searchButton, chooseButton,
                 issuedReply, originalQuery, &searchReply]() {
            if (searchReply.data() != issuedReply) {
                issuedReply->deleteLater();
                return;
            }
            QNetworkReply* reply = issuedReply;
            searchReply = nullptr;
            const QByteArray payload = reply->readAll();
            const int open = payload.indexOf('[');
            const int close = payload.lastIndexOf(']');
            QJsonDocument document;
            if (reply->error() == QNetworkReply::NoError && open >= 0 && close > open)
                document = QJsonDocument::fromJson(payload.mid(open, close - open + 1));
            reply->deleteLater();
            const QJsonArray array = document.isArray() ? document.array() : QJsonArray{};
            QSet<QString> seen;
            for (const QJsonValue& value : array) {
                const QStringList fields = value.toObject().value(QStringLiteral("ref"))
                                               .toString().split(QLatin1Char('~'));
                if (fields.size() < 10)
                    continue;
                const QString cityId = fields.at(0).trimmed();
                const QString name = fields.at(2).trimmed();
                const QString parent = fields.at(9).trimmed();
                if (cityId.isEmpty() || name.isEmpty() || seen.contains(cityId))
                    continue;
                // The weather endpoint accepts official city/district IDs;
                // township and scenic-spot IDs are intentionally excluded.
                if (cityId.size() != 9 || cityId.endsWith(QLatin1Char('A')))
                    continue;
                QString displayName = name;
                // Weather.com.cn omits the administrative suffix for some
                // official district stations (for example 天心). Preserve
                // the suffix the user searched for so the selected location
                // remains an exact 区/县 label after the next refresh.
                if (cityId.size() == 9 && originalQuery.endsWith(QStringLiteral("区"))
                    && !displayName.endsWith(QStringLiteral("区")))
                    displayName += QStringLiteral("区");
                else if (cityId.size() == 9 && originalQuery.endsWith(QStringLiteral("县"))
                         && !displayName.endsWith(QStringLiteral("县")))
                    displayName += QStringLiteral("县");
                auto* item = new QListWidgetItem(
                    parent.isEmpty() ? displayName
                                     : QStringLiteral("%1  ·  %2").arg(displayName, parent), results);
                item->setData(Qt::UserRole, cityId);
                item->setData(Qt::UserRole + 1, displayName);
                seen.insert(cityId);
                if (results->count() == 30)
                    break;
            }
            searchButton->setEnabled(true);
            if (results->count() > 0) {
                results->setCurrentRow(0);
                chooseButton->setEnabled(true);
                resultLabel->setText(QStringLiteral("找到 %1 个结果，请选择准确地区")
                                         .arg(results->count()));
            } else {
                resultLabel->setText(QStringLiteral("没有找到区县级结果，请尝试去掉“市/区”后缀或输入上级城市"));
            }
        });
    };
    connect(searchButton, &QPushButton::clicked, &dialog, performSearch);
    connect(edit, &QLineEdit::returnPressed, &dialog, performSearch);

    const int result = dialog.exec();
    if (searchReply) {
        // abort() can synchronously emit finished(), whose handler clears
        // searchReply. Detach it before aborting and use a stable local guard.
        const QPointer<QNetworkReply> pending = searchReply;
        searchReply = nullptr;
        pending->abort();
        if (pending)
            pending->deleteLater();
    }
    if (result != QDialog::Accepted || !results->currentItem())
        return;
    const QString cityId = results->currentItem()->data(Qt::UserRole).toString();
    const QString city = results->currentItem()->data(Qt::UserRole + 1).toString();
    if (cityId.isEmpty() || city.isEmpty())
        return;
    QSettings settings;
    settings.setValue(QStringLiteral("weather/location"), city);
    settings.setValue(QStringLiteral("weather/cityId"), cityId);
    for (BatteryWidget* widget : s_instances) {
        if (!widget || widget->m_kind != CardKind::Weather)
            continue;
        widget->m_weatherLocation = city;
        widget->m_weatherCityId = cityId;
        widget->refreshWeather();
    }
}

QRectF BatteryWidget::drawWeatherIcon(QPainter& p, const QRectF& rect, bool night,
                                      int weatherCode) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Use the supplied weather artwork instead of synthesising generic
    // clouds and suns. The PNGs are bundled with the executable and have
    // pure-white RGB while retaining their original antialiased alpha.
    QString iconName = weatherCode >= 0 ? QString::number(weatherCode)
                                         : QStringLiteral("01");
    if (weatherCode >= 0 && weatherCode < 10)
        iconName = iconName.rightJustified(2, QLatin1Char('0'));
    if (night && (iconName == QStringLiteral("00")
                  || iconName == QStringLiteral("01")
                  || iconName == QStringLiteral("03")
                  || iconName == QStringLiteral("13")))
        iconName += QLatin1Char('n');

    static QHash<QString, QImage> iconCache;
    auto it = iconCache.constFind(iconName);
    if (it == iconCache.cend()) {
        QImage source(QStringLiteral(":/weather/%1.png").arg(iconName));
        if (source.isNull())
            source.load(QStringLiteral(":/weather/01.png"));
        source = source.convertToFormat(QImage::Format_ARGB32);

        // The source sheets use a 256 px transparent canvas. Cropping that
        // padding is essential: without it, the actual glyph is too small and
        // its visual centre drifts, especially in the hourly and daily rows.
        int left = source.width();
        int top = source.height();
        int right = -1;
        int bottom = -1;
        for (int y = 0; y < source.height(); ++y) {
            const QRgb* scan = reinterpret_cast<const QRgb*>(source.constScanLine(y));
            for (int x = 0; x < source.width(); ++x) {
                if (qAlpha(scan[x]) == 0)
                    continue;
                left = qMin(left, x);
                top = qMin(top, y);
                right = qMax(right, x);
                bottom = qMax(bottom, y);
            }
        }
        if (right >= left && bottom >= top)
            source = source.copy(QRect(left, top, right - left + 1, bottom - top + 1));
        it = iconCache.constFind(iconCache.insert(iconName, source).key());
    }

    const QRectF paintedBounds = ResponsiveLayout::drawImageFitted(p, rect, it.value());
    p.restore();
    return paintedBounds;
}

void BatteryWidget::drawAnalogClock(QPainter& p, const QRectF& rect, int hourOffset) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const ResponsiveLayout::Metrics metrics(rect);
    const QPointF c = rect.center();
    const qreal r = qMin(rect.width(), rect.height()) * kClockDialRadiusRatio;
    p.setPen(QPen(QColor(247, 250, 255, 72), metrics.stroke(p, .004)));
    p.setBrush(QColor(255, 255, 255, 16));
    p.drawEllipse(c, r, r);
    for (int i = 0; i < 60; ++i) {
        const qreal a = (i / 60.0) * 2.0 * M_PI - M_PI / 2.0;
        const qreal outer = r - metrics.size(.012);
        const qreal inner = r - metrics.size((i % 5 == 0) ? .052 : .028);
        p.setPen(QPen(QColor(245, 249, 255, (i % 5 == 0) ? 210 : 100),
                      metrics.stroke(p, (i % 5 == 0) ? .008 : .004),
                      Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c + QPointF(std::cos(a), std::sin(a)) * inner,
                   c + QPointF(std::cos(a), std::sin(a)) * outer);
    }
    p.setFont(displayFont(qMax(13, qRound(r * .23)), QFont::DemiBold));
    p.setPen(QColor(238, 243, 253, 218));
    for (int hour = 1; hour <= 12; ++hour) {
        const qreal a = (hour / 12.0) * 2.0 * M_PI - M_PI / 2.0;
        const QPointF pos = c + QPointF(std::cos(a), std::sin(a))
                          * (r - metrics.size(.108));
        const QString label = QString::number(hour);
        const qreal labelBox = metrics.size(.104);
        ResponsiveLayout::drawSingleLine(p, QRectF(pos.x() - labelBox * .5,
                                                    pos.y() - labelBox * .5,
                                                    labelBox, labelBox),
                       Qt::AlignCenter, label);
    }
    const QTime time = QTime::currentTime().addSecs(hourOffset * 3600);
    const auto hand = [&p, c, r](qreal angle, qreal length, qreal width, const QColor& color) {
        p.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c, c + QPointF(std::cos(angle), std::sin(angle)) * length);
    };
    const qreal sec = (time.second() + time.msec() / 1000.0) / 60.0 * 2.0 * M_PI - M_PI / 2.0;
    const qreal min = (time.minute() + time.second() / 60.0) / 60.0 * 2.0 * M_PI - M_PI / 2.0;
    const qreal hour = ((time.hour() % 12) + time.minute() / 60.0) / 12.0 * 2.0 * M_PI - M_PI / 2.0;
    hand(hour, r * .52, metrics.stroke(p, .020), QColor(250, 252, 255, 230));
    hand(min, r * .73, metrics.stroke(p, .013), QColor(250, 252, 255, 220));
    hand(sec, r * .80, metrics.stroke(p, .006), QColor(255, 174, 172, 230));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 195, 194, 240));
    p.drawEllipse(c, metrics.size(.016), metrics.size(.016));
    p.restore();
}

void BatteryWidget::drawDigitalClock(QPainter& p, const QRectF& rect,
                                     const QString& city, int hourOffset,
                                     bool transparentFace) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const ResponsiveLayout::Metrics metrics(rect);
    const qreal faceInset = metrics.size(kClockFaceInsetRatio);
    const QRectF face = rect.adjusted(faceInset, faceInset,
                                      -faceInset, -faceInset);
    if (!transparentFace) {
        p.setPen(QPen(QColor(255, 255, 255, 150), metrics.stroke(p, .004)));
        p.setBrush(QColor(248, 249, 251, 238));
        p.drawRoundedRect(face, qMin(face.width(), face.height()) * .16,
                          qMin(face.width(), face.height()) * .16);
    }
    const QDateTime value = QDateTime::currentDateTime().addSecs(hourOffset * 3600);
    const bool wideLayout = face.width() >= face.height() * 1.35;
    p.setPen(transparentFace ? QColor(246, 249, 255, 238)
                             : QColor(14, 18, 24, 240));
    p.setFont(displayFont(qMax(22, qRound(face.height()
                                          * (wideLayout ? .38 : .23))),
                          QFont::DemiBold));
    const QRectF timeArea = wideLayout
        ? QRectF(face.left() + face.width() * .04,
                 face.top() + face.height() * .06,
                 face.width() * .92, face.height() * .66)
        : face.adjusted(face.width() * .08, face.height() * .20,
                        -face.width() * .08, -face.height() * .22);
    ResponsiveLayout::drawSingleLine(p, timeArea, Qt::AlignCenter,
                                     value.toString(QStringLiteral("HH:mm")));
    p.setPen(transparentFace ? QColor(220, 229, 244, 220)
                             : QColor(111, 119, 133, 210));
    p.setFont(displayFont(qMax(8, qRound(face.height()
                                         * (wideLayout ? .085 : .075))),
                          QFont::Normal));
    const QRectF locationArea = wideLayout
        ? QRectF(face.left(), face.top() + face.height() * .72,
                 face.width(), face.height() * .18)
        : face.adjusted(0, face.height() * .73, 0, -face.height() * .08);
    ResponsiveLayout::drawSingleLine(
        p, locationArea, Qt::AlignCenter,
        city.isEmpty() ? QStringLiteral("LOCAL") : city.toUpper());
    // A circular tick ring is appropriate for the square card, but on a wide
    // card it constrains the payload to the short edge and leaves most of the
    // horizontal surface empty. The wide layout deliberately uses the full
    // measured text area instead.
    if (wideLayout) {
        p.restore();
        return;
    }
    p.setPen(QPen(transparentFace ? QColor(235, 242, 255, 150)
                                 : QColor(74, 81, 92, 130), metrics.stroke(p, .003),
                  Qt::SolidLine, Qt::RoundCap));
    for (int i = 0; i < 48; ++i) {
        const qreal angle = (i / 48.0) * 2.0 * M_PI - M_PI / 2.0;
        const qreal outer = qMin(face.width(), face.height()) * .43;
        const qreal inner = outer - metrics.size((i % 4 == 0) ? .020 : .010);
        const QPointF c = face.center();
        p.drawLine(c + QPointF(std::cos(angle), std::sin(angle)) * inner,
                   c + QPointF(std::cos(angle), std::sin(angle)) * outer);
    }
    p.restore();
}

void BatteryWidget::drawClockFace(QPainter& p, const QRectF& rect, int style,
                                  const QString& city, int hourOffset) const
{
    const ResponsiveLayout::Metrics metrics(rect);
    if (style == 2 || style == 8) {
        drawDigitalClock(p, rect, city, hourOffset, true);
        return;
    }
    if (style == 5 || style == 6 || style == 7) {
        p.save();
        const int count = style == 5 ? 4 : (style == 6 ? 3 : 4);
        const int cols = style == 5 ? 2 : (style == 6 ? 3 : 4);
        const qreal gap = rect.width() * .05;
        const qreal cellW = (rect.width() - gap * (cols - 1)) / cols;
        const qreal cellH = style == 5 ? (rect.height() - gap) / 2.0 : rect.height();
        const QStringList cities = { QStringLiteral("东京"), QStringLiteral("北京"),
                                     QStringLiteral("巴黎"), QStringLiteral("纽约") };
        const int offsets[] = { 9, 8, 1, -5 };
        for (int i = 0; i < count; ++i) {
            const QRectF cell(rect.left() + (i % cols) * (cellW + gap),
                              rect.top() + (i / cols) * (cellH + gap), cellW, cellH);
            p.setPen(QPen(QColor(255, 255, 255, 72), metrics.stroke(p, .003)));
            p.setBrush(QColor(32, 37, 48, style == 7 ? 198 : 182));
            p.drawRoundedRect(cell, qMin(cellW, cellH) * .16, qMin(cellW, cellH) * .16);
            drawAnalogClock(p, cell.adjusted(cellW * .08, cellH * .08,
                                             -cellW * .08, -cellH * .08), offsets[i]);
            p.setPen(QColor(225, 232, 244, 185));
            p.setFont(displayFont(qMax(8, qRound(cellH * .07)), QFont::Normal));
            ResponsiveLayout::drawSingleLine(p, QRectF(cell.left(), cell.bottom() - cellH * .19,
                                     cell.width(), cellH * .12),
                           Qt::AlignCenter, cities.at(i));
        }
        p.restore();
        return;
    }

    p.save();
    const qreal faceInset = metrics.size(kClockFaceInsetRatio);
    const QRectF face = rect.adjusted(faceInset, faceInset,
                                      -faceInset, -faceInset);
    if (style == 1 || style == 4) {
        p.setPen(QPen(QColor(255, 255, 255, 50), metrics.stroke(p, .003)));
        p.setBrush(QColor(26, 30, 38, 215));
        p.drawRoundedRect(face, qMin(face.width(), face.height()) * .16,
                          qMin(face.width(), face.height()) * .16);
    }
    QRectF dialArea = face;
    QRectF cityArea;
    if (!city.isEmpty()) {
        const qreal labelHeight = rect.height() * kClockCityLabelHeightRatio;
        const qreal labelGap = rect.height() * kClockCityLabelGapRatio;
        cityArea = QRectF(face.left(), face.bottom() - labelHeight,
                          face.width(), labelHeight);
        dialArea.setBottom(cityArea.top() - labelGap);
    }
    const qreal dialInset = metrics.size(kClockDialInsetRatio);
    const QRectF dial = style == 1 || style == 4
        ? dialArea.adjusted(dialInset, dialInset, -dialInset, -dialInset)
        : dialArea;
    drawAnalogClock(p, dial, hourOffset);
    if (!city.isEmpty()) {
        p.setPen(style == 1 || style == 4 ? QColor(235, 239, 247, 205)
                                         : QColor(89, 98, 112, 205));
        p.setFont(displayFont(qMax(8, qRound(rect.height() * .065)), QFont::DemiBold));
        ResponsiveLayout::drawSingleLine(p, cityArea, Qt::AlignCenter, city);
    }
    p.restore();
}

void BatteryWidget::drawDashboardInfo(QPainter& p, const QRectF& card) const
{
    const QRectF content = card.adjusted(28, 25, -28, -24);
    p.setFont(displayFont(11, QFont::DemiBold));
    p.setPen(QColor(205, 215, 232, 190));
    ResponsiveLayout::drawSingleLine(p, QRectF(content.left(), content.top(), 180, 24),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("TODAY"));
    p.setFont(displayFont(30, QFont::DemiBold));
    p.setPen(QColor(248, 251, 255, 235));
    ResponsiveLayout::drawSingleLine(p, QRectF(content.left(), content.top() + 34,
                             content.width(), 52),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QDate::currentDate().toString(QStringLiteral("MMMM d")));
    p.setFont(displayFont(14, QFont::Normal));
    p.setPen(QColor(184, 195, 216, 175));
    ResponsiveLayout::drawSingleLine(p, QRectF(content.left(), content.top() + 90,
                             content.width(), 24),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QDate::currentDate().toString(QStringLiteral("dddd · yyyy")));

    const QRectF line(content.left(), content.top() + 139, content.width(), 1);
    p.fillRect(line, QColor(255, 255, 255, 28));
    p.setFont(displayFont(12, QFont::Normal));
    p.setPen(QColor(190, 201, 221, 170));
    ResponsiveLayout::drawSingleLine(p, QRectF(content.left(), content.top() + 158,
                             content.width() * .55, 24),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("电源状态"));
    p.setPen(QColor(255, 255, 255, m_charging ? 235 : 205));
    ResponsiveLayout::drawSingleLine(p, QRectF(content.left(), content.top() + 186,
                             content.width() * .55, 28),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   m_hasBattery ? (m_charging ? QStringLiteral("正在充电")
                                              : QStringLiteral("使用电池"))
                                : QStringLiteral("已接入电源"));
    p.setPen(QColor(190, 201, 221, 170));
    ResponsiveLayout::drawSingleLine(p, QRectF(content.left() + content.width() * .55,
                             content.top() + 158, content.width() * .45, 24),
                   Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("设备"));
    p.setPen(QColor(237, 242, 250, 205));
    ResponsiveLayout::drawSingleLine(p, QRectF(content.left() + content.width() * .55,
                             content.top() + 186, content.width() * .45, 28),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("%1 项已连接").arg(m_connectedDevices.size()));
}

void BatteryWidget::showSettingsDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("macdowsOS Widget 设置"));
    dialog.setModal(true);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background:#171a21; color:#eef2fa; }"
        "QLabel { color:#eef2fa; }"
        "QComboBox { background:#252a35; color:#eef2fa; border:1px solid #596273; border-radius:6px; padding:5px 10px; }"
        "QCheckBox { color:#eef2fa; spacing:8px; }"
        "QDialogButtonBox QPushButton { background:#303746; color:#eef2fa; border:1px solid #68738a; border-radius:6px; padding:6px 18px; }"
        "QDialogButtonBox QPushButton:hover { background:#424c60; }"));

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 18);
    layout->setSpacing(14);
    auto* title = new QLabel(QStringLiteral("显示、材质与启动"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size:18px; font-weight:600;"));
    layout->addWidget(title);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFormAlignment(Qt::AlignTop);
    auto* scaleBox = new QComboBox(&dialog);
    scaleBox->addItem(QStringLiteral("微小（40%）"), 0.40);
    scaleBox->addItem(QStringLiteral("小（55%）"), 0.55);
    scaleBox->addItem(QStringLiteral("标准（70%）"), 0.70);
    scaleBox->addItem(QStringLiteral("舒适（85%）"), 0.85);
    scaleBox->addItem(QStringLiteral("原始（100%）"), 1.00);
    scaleBox->addItem(QStringLiteral("大（115%）"), 1.15);
    scaleBox->addItem(QStringLiteral("特大（135%）"), 1.35);
    scaleBox->addItem(QStringLiteral("超大（160%）"), 1.60);
    scaleBox->addItem(QStringLiteral("高分屏（200%）"), 2.00);
    int selected = 4;
    for (int i = 0; i < scaleBox->count(); ++i) {
        if (qFuzzyCompare(scaleBox->itemData(i).toDouble(), m_uiScale)) {
            selected = i;
            break;
        }
    }
    scaleBox->setCurrentIndex(selected);
    form->addRow(QStringLiteral("显示大小"), scaleBox);

    auto* materialBox = new QComboBox(&dialog);
    materialBox->addItem(QStringLiteral("液态玻璃（折射 + 模糊）"), false);
    materialBox->addItem(QStringLiteral("仅模糊（Windows 10 / 11）"), true);
    materialBox->setCurrentIndex(systemBlurEnabled() ? 1 : 0);
    form->addRow(QStringLiteral("背景材质"), materialBox);

    auto* liveBackdrop = new QCheckBox(QStringLiteral("实时渲染窗口后的内容"), &dialog);
    liveBackdrop->setChecked(liveBackdropEnabled());
    form->addRow(QStringLiteral("背景采样"), liveBackdrop);

    auto* renderBox = new QComboBox(&dialog);
    renderBox->addItem(QStringLiteral("自动（推荐）"), int(RenderBackend::AutoBackend));
    renderBox->addItem(QStringLiteral("GPU · OpenGL Shader"), int(RenderBackend::GpuBackend));
    renderBox->addItem(QStringLiteral("CPU · 软件缓存模糊"), int(RenderBackend::CpuBackend));
    renderBox->setCurrentIndex(renderBox->findData(int(renderBackend())));
    form->addRow(QStringLiteral("渲染方式"), renderBox);

    auto* qualityBox = new QComboBox(&dialog);
    qualityBox->addItem(QStringLiteral("节能（60% 设备像素）"), 0.60);
    qualityBox->addItem(QStringLiteral("均衡（80% 设备像素）"), 0.80);
    qualityBox->addItem(QStringLiteral("原生（100% 设备像素）"), 1.00);
    qualityBox->addItem(QStringLiteral("高质量（125% 超采样，推荐）"), 1.25);
    qualityBox->addItem(QStringLiteral("极致（150% 超采样）"), 1.50);
    int qualityIndex = 2;
    for (int i = 0; i < qualityBox->count(); ++i) {
        if (qAbs(qualityBox->itemData(i).toDouble() - qreal(renderScale())) < 0.001) {
            qualityIndex = i;
            break;
        }
    }
    qualityBox->setCurrentIndex(qualityIndex);
    form->addRow(QStringLiteral("渲染清晰度"), qualityBox);

    auto* fontSmoothingBox = new QComboBox(&dialog);
    fontSmoothingBox->addItem(QStringLiteral("关闭"), 0);
    fontSmoothingBox->addItem(QStringLiteral("标准（灰阶）"), 1);
    fontSmoothingBox->addItem(QStringLiteral("清晰（推荐）"), 2);
    fontSmoothingBox->addItem(QStringLiteral("最高（字形超采样）"), 3);
    fontSmoothingBox->setCurrentIndex(
        qMax(0, fontSmoothingBox->findData(globalFontSmoothing())));
    form->addRow(QStringLiteral("字体抗锯齿"), fontSmoothingBox);

    auto* locationRow = new QWidget(&dialog);
    auto* locationLayout = new QHBoxLayout(locationRow);
    locationLayout->setContentsMargins(0, 0, 0, 0);
    locationLayout->setSpacing(8);
    auto* locationEdit = new QLineEdit(m_weatherLocation, locationRow);
    locationEdit->setReadOnly(true);
    locationEdit->setPlaceholderText(QStringLiteral("请搜索并选择区县"));
    auto* locationButton = new QPushButton(QStringLiteral("搜索区县…"), locationRow);
    locationLayout->addWidget(locationEdit, 1);
    locationLayout->addWidget(locationButton);
    form->addRow(QStringLiteral("天气地区"), locationRow);
    connect(locationButton, &QPushButton::clicked, this, [this, locationEdit]() {
        searchWeatherCity();
        locationEdit->setText(m_weatherLocation);
    });

    auto* blurRow = new QWidget(&dialog);
    auto* blurLayout = new QHBoxLayout(blurRow);
    blurLayout->setContentsMargins(0, 0, 0, 0);
    blurLayout->setSpacing(8);
    auto* blurSlider = new QSlider(Qt::Horizontal, blurRow);
    blurSlider->setRange(0, 100);
    blurSlider->setValue(materialBlurStrength());
    auto* blurSpin = new QSpinBox(blurRow);
    blurSpin->setRange(0, 100);
    blurSpin->setSuffix(QStringLiteral("%"));
    blurSpin->setValue(blurSlider->value());
    blurLayout->addWidget(blurSlider, 1);
    blurLayout->addWidget(blurSpin);
    connect(blurSlider, &QSlider::valueChanged,
            blurSpin, &QSpinBox::setValue);
    connect(blurSpin, qOverload<int>(&QSpinBox::valueChanged),
            blurSlider, &QSlider::setValue);
    form->addRow(QStringLiteral("模糊程度"), blurRow);

    auto* opacityRow = new QWidget(&dialog);
    auto* opacityLayout = new QHBoxLayout(opacityRow);
    opacityLayout->setContentsMargins(0, 0, 0, 0);
    opacityLayout->setSpacing(8);
    auto* opacitySlider = new QSlider(Qt::Horizontal, opacityRow);
    opacitySlider->setRange(5, 100);
    opacitySlider->setValue(qBound(5, qRound(glassOpacity() * 100.0), 100));
    auto* opacitySpin = new QSpinBox(opacityRow);
    opacitySpin->setRange(5, 100);
    opacitySpin->setSuffix(QStringLiteral("%"));
    opacitySpin->setValue(opacitySlider->value());
    opacityLayout->addWidget(opacitySlider, 1);
    opacityLayout->addWidget(opacitySpin);
    connect(opacitySlider, &QSlider::valueChanged,
            opacitySpin, &QSpinBox::setValue);
    connect(opacitySpin, qOverload<int>(&QSpinBox::valueChanged),
            opacitySlider, &QSlider::setValue);
    form->addRow(QStringLiteral("透明度"), opacityRow);

    auto* mouseThrough = new QCheckBox(QStringLiteral("鼠标穿透（组件不接收点击）"), &dialog);
    mouseThrough->setChecked(mouseThroughEnabled());
    form->addRow(QString(), mouseThrough);

    auto* lowPower = new QCheckBox(QStringLiteral("节能刷新（仅鼠标穿透时生效，4 FPS）"), &dialog);
    lowPower->setChecked(lowPowerRefreshEnabled());
    lowPower->setEnabled(mouseThrough->isChecked());
    connect(mouseThrough, &QCheckBox::toggled,
            lowPower, &QWidget::setEnabled);
    form->addRow(QString(), lowPower);

    auto* startup = new QCheckBox(QStringLiteral("登录 Windows 时自动启动"), &dialog);
    startup->setChecked(isStartupEnabled());
    form->addRow(QString(), startup);
    layout->addLayout(form);

    auto* hint = new QLabel(QStringLiteral("关闭实时背景时保持当前的壁纸采样；开启后会持续渲染每个组件正下方的窗口与组件。清晰模式按屏幕原生像素渲染，可改善高 DPI 下的模糊和锯齿。"), &dialog);
    hint->setStyleSheet(QStringLiteral("color:#9da8bd;"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        setStartupEnabled(startup->isChecked());
        syncGroupSettings(scaleBox->currentData().toDouble(),
                          materialBox->currentData().toBool(),
                          blurSlider->value(),
                          opacitySlider->value() / 100.0,
                          mouseThrough->isChecked(),
                          lowPower->isChecked() && mouseThrough->isChecked(),
                          liveBackdrop->isChecked(),
                          qualityBox->currentData().toDouble(),
                          fontSmoothingBox->currentData().toInt(),
                          static_cast<RenderBackend>(renderBox->currentData().toInt()));
        if (m_startupAction) {
            QSignalBlocker blocker(m_startupAction);
            m_startupAction->setChecked(startup->isChecked());
        }
    }
}

bool BatteryWidget::isStartupEnabled() const
{
#ifdef Q_OS_WIN
    QSettings runKey(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                    QSettings::NativeFormat);
    return runKey.contains(QStringLiteral("macdowsOS Widget"))
           || runKey.contains(QStringLiteral("macdowsOSBattery"));
#else
    return false;
#endif
}

void BatteryWidget::setStartupEnabled(bool enabled)
{
#ifdef Q_OS_WIN
    QSettings runKey(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                    QSettings::NativeFormat);
    constexpr auto kStartupName = "macdowsOS Widget";
    if (enabled) {
        const QString path = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        runKey.setValue(QString::fromLatin1(kStartupName), QStringLiteral("\"%1\"").arg(path));
    } else {
        runKey.remove(QString::fromLatin1(kStartupName));
        runKey.remove(QStringLiteral("macdowsOSBattery"));
    }
    runKey.sync();
#else
    Q_UNUSED(enabled);
#endif
}

void BatteryWidget::showAboutDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("关于 macdowsOS Widget"));
    dialog.setModal(true);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background:#171a21; color:#eef2fa; }"
        "QLabel { color:#eef2fa; }"
        "QDialogButtonBox QPushButton { background:#303746; color:#eef2fa; border:1px solid #68738a; border-radius:6px; padding:6px 18px; }"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(28, 24, 28, 20);
    auto* title = new QLabel(QStringLiteral("macdowsOS Widget"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size:20px; font-weight:600;"));
    layout->addWidget(title);
    auto* text = new QLabel(QStringLiteral("雾蓝回针MistBlueSt 版本 1.0.0 beta2 版本号 20260921100b2"), &dialog);
    text->setStyleSheet(QStringLiteral("color:#aab5c8; line-height:1.4;"));
    text->setWordWrap(true);
    layout->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void BatteryWidget::refreshBattery()
{
    const int previousLevel = m_level;
    const bool previousCharging = m_charging;
    const bool previousHasBattery = m_hasBattery;
    const QVector<ConnectedDevice> previousDevices = m_connectedDevices;
#ifdef Q_OS_WIN
    SYSTEM_POWER_STATUS status{};
    if (GetSystemPowerStatus(&status)) {
        m_hasBattery = status.BatteryFlag != 128 && status.BatteryLifePercent != 255;
        if (m_hasBattery)
            m_level = qBound(0, static_cast<int>(status.BatteryLifePercent), 100);
        // AC power without a battery is an adapter-only desktop state, not a
        // charging battery.  Keep the UI honest instead of showing a stale
        // percentage or a fabricated lightning indicator.
        m_charging = m_hasBattery && status.ACLineStatus == 1;
    } else {
        m_hasBattery = false;
        m_charging = false;
    }
#else
    m_hasBattery = false;
    m_charging = false;
#endif

    // Device arrival/removal changes less often than the system charge level.
    // Refresh it immediately, then every 30 seconds without inventing values.
    if (m_deviceRefreshTick == 0)
        refreshConnectedDevices();
    m_deviceRefreshTick = (m_deviceRefreshTick + 1) % 15;

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
    const bool changed = previousLevel != m_level
                         || previousCharging != m_charging
                         || previousHasBattery != m_hasBattery
                         || devicesChanged;
    // Avoid waking OpenGL and rebuilding the tray pixmap every two seconds
    // when Windows reports exactly the same state. A real battery/accessory
    // change still reaches the UI on the same polling tick as before.
    if (changed)
        update();
    if (changed && m_trayIcon) {
        m_trayIcon->setIcon(createTrayIcon());
        m_trayIcon->setToolTip(m_hasBattery
                                   ? QStringLiteral("macdowsOS Widget  ·  %1%").arg(m_level)
                                   : QStringLiteral("macdowsOS Widget  ·  电源适配器"));
    }
}

void BatteryWidget::refreshConnectedDevices()
{
    if (m_deviceWatcher.isRunning())
        return;
    // Function Discovery and Bluetooth enumeration can block for seconds.
    // Share one future across battery cards and deliver results on the GUI
    // thread; the task captures no widget and safely outlives card deletion.
    static QFuture<QVector<ConnectedDevice>> shared;
    static QElapsedTimer age;
    if (!age.isValid() || (age.elapsed() >= 30000 && shared.isFinished())) {
        shared = QtConcurrent::run(&BatteryWidget::queryConnectedDevices);
        age.restart();
    }
    m_deviceWatcher.setFuture(shared);
}

QVector<BatteryWidget::ConnectedDevice> BatteryWidget::queryConnectedDevices()
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

    // Function Discovery also covers Bluetooth Low Energy and HID devices
    // which are not always returned by the legacy Bluetooth enumeration API.
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
        // The Devices category includes monitors and printers.  Only show
        // recognisable input/audio accessories, or another device that
        // actually publishes its own battery percentage.
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
                // Device display names and Bluetooth radio names occasionally
                // differ by a suffix.  A normalized containment match covers
                // that case without associating unrelated short names.
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

void BatteryWidget::drawBatteryIcon(QPainter& painter, const QRectF& rect, int level, bool charging) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const ResponsiveLayout::Metrics metrics(rect);

    const qreal radius = rect.height() * 0.21;
    const qreal edge = metrics.stroke(painter, .012);
    QRectF body = rect.adjusted(edge, edge, -rect.height() * 0.105, -edge);
    QPainterPath bodyPath;
    bodyPath.addRoundedRect(body, radius, radius);

    QLinearGradient shell(0, body.top(), 0, body.bottom());
    shell.setColorAt(0.0, QColor(255, 255, 255, 220));
    shell.setColorAt(0.45, QColor(221, 227, 236, 165));
    shell.setColorAt(1.0, QColor(157, 166, 180, 160));
    painter.fillPath(bodyPath, shell);

    const qreal inset = metrics.size(.09);
    QRectF levelRect = body.adjusted(inset, inset, -inset, -inset);
    levelRect.setWidth(levelRect.width() * level / 100.0);
    if (level > 0) {
        QPainterPath levelPath;
        levelPath.addRoundedRect(levelRect, radius * 0.68, radius * 0.68);
        QLinearGradient fill(levelRect.topLeft(), levelRect.bottomRight());
        if (level <= 20) {
            fill.setColorAt(0.0, QColor(255, 142, 91));
            fill.setColorAt(1.0, QColor(235, 58, 82));
        } else if (charging) {
            fill.setColorAt(0.0, QColor(255, 255, 255, 235));
            fill.setColorAt(1.0, QColor(255, 255, 255, 235));
        } else {
            fill.setColorAt(0.0, QColor(255, 255, 255, 235));
            fill.setColorAt(1.0, QColor(255, 255, 255, 235));
        }
        painter.fillPath(levelPath, fill);
    }

    QRectF terminal(body.right() + metrics.size(.02),
                    body.center().y() - rect.height() * .16,
                    rect.height() * .105, rect.height() * .32);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(208, 215, 225, 190));
    painter.drawRoundedRect(terminal, terminal.width() * .5, terminal.width() * .5);

    painter.setPen(QPen(QColor(255, 255, 255, 160),
                        metrics.stroke(painter, .012)));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(bodyPath);

    if (charging) {
        QPainterPath bolt;
        const qreal cx = body.center().x();
        const qreal cy = body.center().y();
        bolt.moveTo(cx + rect.width() * .015, cy - rect.height() * .24);
        bolt.lineTo(cx - rect.width() * .095, cy + rect.height() * .015);
        bolt.lineTo(cx - rect.width() * .005, cy + rect.height() * .015);
        bolt.lineTo(cx - rect.width() * .065, cy + rect.height() * .25);
        bolt.lineTo(cx + rect.width() * .12, cy - rect.height() * .055);
        bolt.lineTo(cx + rect.width() * .025, cy - rect.height() * .055);
        bolt.closeSubpath();
        painter.setBrush(QColor(255, 255, 255, 235));
        painter.setPen(Qt::NoPen);
        painter.drawPath(bolt);
    }
    painter.restore();
}

void BatteryWidget::drawDeviceRing(QPainter& p, const QPointF& center, qreal radius,
                                   int level, int iconKind, bool connected,
                                   bool showCaption, qreal strokeMultiplier) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF ring(center.x() - radius, center.y() - radius,
                      radius * 2.0, radius * 2.0);
    const ResponsiveLayout::Metrics metrics(ring);
    const qreal stroke = metrics.stroke(p, .0525)
                       * qMax<qreal>(0.1, strokeMultiplier);

    QPen track(QColor(238, 242, 250, connected ? 38 : 24), stroke,
               Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setBrush(Qt::NoBrush);
    p.setPen(track);
    p.drawEllipse(ring);

    const int clamped = qBound(0, level, 100);
    if (connected && level >= 0 && clamped > 0) {
        const qreal span = -360.0 * clamped / 100.0;
        QPen progress(QColor(255, 255, 255, 242), stroke,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(progress);
        constexpr qreal startAngle = 90.0;
        p.drawArc(ring, qRound(startAngle * 16.0), qRound(span * 16.0));

    }

    // Neutral line icons echo the laptop and mouse marks from the reference;
    // unavailable accessories intentionally remain as empty glass rings.
    if (iconKind == 0) {
        const QRectF screen(center.x() - radius * .34, center.y() - radius * .19,
                            radius * .68, radius * .44);
        p.setPen(QPen(QColor(226, 232, 244, connected ? 165 : 55), stroke * .18,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(screen, radius * .06, radius * .06);
        p.drawLine(QPointF(center.x() - radius * .45, center.y() + radius * .29),
                   QPointF(center.x() + radius * .45, center.y() + radius * .29));
        p.drawLine(QPointF(center.x() - radius * .28, center.y() + radius * .15),
                   QPointF(center.x() + radius * .28, center.y() + radius * .15));
    } else if (iconKind == 1) {
        const QRectF mouse(center.x() - radius * .19, center.y() - radius * .40,
                           radius * .38, radius * .74);
        p.setPen(QPen(QColor(226, 232, 244, connected ? 165 : 55), stroke * .18,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(QColor(226, 232, 244, connected ? 138 : 45));
        p.drawRoundedRect(mouse, radius * .18, radius * .18);
        p.drawLine(QPointF(center.x(), mouse.top() + radius * .08),
                   QPointF(center.x(), mouse.top() + radius * .29));
    } else if (iconKind == 2 && connected) {
        const QRectF keyboard(center.x() - radius * .39, center.y() - radius * .21,
                              radius * .78, radius * .42);
        p.setPen(QPen(QColor(226, 232, 244, 165), stroke * .18,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(QColor(226, 232, 244, 34));
        p.drawRoundedRect(keyboard, radius * .07, radius * .07);
        p.drawLine(keyboard.bottomLeft() + QPointF(radius * .12, -radius * .10),
                   keyboard.bottomRight() + QPointF(-radius * .12, -radius * .10));
    } else if (iconKind == 3 && connected) {
        p.setPen(QPen(QColor(226, 232, 244, 165), stroke * .20,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(center.x() - radius * .31, center.y() - radius * .31,
                         radius * .62, radius * .62), 0, 180 * 16);
        p.drawLine(QPointF(center.x() - radius * .31, center.y()),
                   QPointF(center.x() - radius * .31, center.y() + radius * .28));
        p.drawLine(QPointF(center.x() + radius * .31, center.y()),
                   QPointF(center.x() + radius * .31, center.y() + radius * .28));
    } else if (iconKind == 4 && connected) {
        p.setPen(QPen(QColor(226, 232, 244, 165), stroke * .18));
        p.setBrush(QColor(226, 232, 244, 34));
        p.drawRoundedRect(QRectF(center.x() - radius * .24, center.y() - radius * .34,
                                 radius * .48, radius * .68), radius * .10, radius * .10);
    }

    // Empty slots stay visually empty; only a live device gets a caption.
    if (showCaption && (connected || iconKind == 0)) {
        p.setFont(displayFont(qMax(1, qRound(radius * .44)), QFont::Normal));
        p.setPen(QColor(235, 240, 250, connected ? 182 : 66));
        ResponsiveLayout::drawSingleLine(p, QRectF(center.x() - radius * .72,
                                 center.y() + radius * 1.55,
                                 radius * 1.44, radius * .85),
                       Qt::AlignHCenter | Qt::AlignTop,
                       !connected ? QStringLiteral("—")
                                  : (level >= 0
                                         ? QString::number(clamped) + QChar('%')
                                         : QStringLiteral("已连接")));
    }
    p.restore();
}

void BatteryWidget::paintOverlay(QPainter& p)
{
    const qreal scale = qMax<qreal>(0.1, m_uiScale);
    p.save();
    p.scale(scale, scale);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, globalFontSmoothing() > 0);

    // Geometry is expressed in the reference (100%) coordinate system and
    // scaled exactly once.  The glass object itself occupies the whole
    // widget, so drawing an inset card here would create a visible backdrop
    // ring around the actual SDF surface.
    const QSizeF logicalSize(width() / scale, height() / scale);
    const QRectF card(QPointF(0.0, 0.0), logicalSize);
    const qreal radius = 34.0;

    // Keep all CPU-painted highlights inside the same rounded silhouette as
    // the shader.  The desktop outside the UI remains untouched/transparent.
    QPainterPath cardClip;
    cardClip.addRoundedRect(card, radius, radius);
    p.setClipPath(cardClip, Qt::IntersectClip);

    if (m_dashboardMode) {
        paintGlassSurfaceFrame(p, kDashboardBatteryRect, kDashboardCardRadius);
        paintGlassSurfaceFrame(p, kDashboardWeatherRect, kDashboardCardRadius);
        paintGlassSurfaceFrame(p, kDashboardClockRect, kDashboardCardRadius);
        paintGlassSurfaceFrame(p, kDashboardInfoRect, kDashboardCardRadius);

        const QRectF battery = kDashboardBatteryRect;
        const QRectF batteryContent = battery.adjusted(28, 24, -28, -18);
        p.setFont(displayFont(11, QFont::DemiBold));
        p.setPen(QColor(213, 221, 237, 205));
        ResponsiveLayout::drawSingleLine(p, QRectF(batteryContent.left(), batteryContent.top(), 180, 22),
                       Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("BATTERY"));
        p.setFont(displayFont(12, QFont::Normal));
        p.setPen(QColor(177, 189, 211, 155));
        ResponsiveLayout::drawSingleLine(p, QRectF(batteryContent.right() - 170,
                                 batteryContent.top(), 170, 22),
                       Qt::AlignRight | Qt::AlignVCenter,
                       m_charging ? QStringLiteral("正在充电")
                                  : QStringLiteral("实时同步"));
        const qreal ringY = battery.top() + 215.0;
        const qreal firstX = battery.left() + 88.0;
        const qreal spacing = 166.0;
        drawDeviceRing(p, QPointF(firstX, ringY), 47.0,
                       m_hasBattery ? m_level : -1, 0, m_hasBattery);
        for (int slot = 0; slot < 3; ++slot) {
            const bool connected = slot < m_connectedDevices.size();
            const ConnectedDevice device = connected ? m_connectedDevices.at(slot)
                                                      : ConnectedDevice{};
            drawDeviceRing(p, QPointF(firstX + spacing * (slot + 1), ringY), 47.0,
                           device.level, connected ? device.iconKind : -1, connected);
        }

        const QRectF weather = kDashboardWeatherRect;
        const QRectF weatherContent = weather.adjusted(26, 24, -24, -20);
        p.setFont(displayFont(11, QFont::DemiBold));
        p.setPen(QColor(213, 221, 237, 205));
        ResponsiveLayout::drawSingleLine(p, QRectF(weatherContent.left(), weatherContent.top(),
                                 weatherContent.width(), 22),
                       Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("WEATHER"));
        p.setFont(displayFont(12, QFont::Normal));
        p.setPen(QColor(177, 189, 211, 155));
        ResponsiveLayout::drawSingleLine(p, QRectF(weatherContent.left(), weatherContent.top() + 30,
                                 weatherContent.width(), 22),
                       Qt::AlignLeft | Qt::AlignVCenter, m_weatherLocation);
        drawWeatherIcon(p, QRectF(weatherContent.left() + 8, weatherContent.top() + 70, 102, 96),
                        m_weatherNight, m_weatherCode);
        const QString temp = weatherTemperatureLabel(m_weatherTemperature);
        p.setFont(displayFont(40, QFont::DemiBold));
        p.setPen(QColor(249, 251, 255, 238));
        ResponsiveLayout::drawSingleLine(p, QRectF(weatherContent.left() + 120,
                                 weatherContent.top() + 74,
                                 weatherContent.width() - 120, 54),
                       Qt::AlignLeft | Qt::AlignVCenter, temp);
        p.setFont(displayFont(13, QFont::Normal));
        p.setPen(QColor(207, 216, 234, 195));
        const QString desc = m_weatherDescription.isEmpty()
                                 ? QStringLiteral("获取天气中…") : m_weatherDescription;
        ResponsiveLayout::drawSingleLine(p, QRectF(weatherContent.left(), weatherContent.top() + 177,
                                 weatherContent.width(), 24),
                       Qt::AlignLeft | Qt::AlignVCenter, desc);
        p.setFont(displayFont(11, QFont::Normal));
        p.setPen(QColor(173, 185, 209, 165));
        const QString range = (m_weatherHigh.isEmpty() || m_weatherLow.isEmpty())
                                  ? QStringLiteral("今日  -- / --")
                                  : QStringLiteral("今日  %1° / %2°").arg(m_weatherHigh, m_weatherLow);
        ResponsiveLayout::drawSingleLine(p, QRectF(weatherContent.left(), weatherContent.top() + 218,
                                 weatherContent.width(), 22),
                       Qt::AlignLeft | Qt::AlignVCenter, range);

        const QRectF clock = kDashboardClockRect;
        const QRectF clockContent = clock.adjusted(24, 20, -24, -14);
        p.setFont(displayFont(11, QFont::DemiBold));
        p.setPen(QColor(213, 221, 237, 205));
        ResponsiveLayout::drawSingleLine(p, QRectF(clockContent.left(), clockContent.top(), 180, 22),
                       Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("CLOCK"));
        drawAnalogClock(p, QRectF(clockContent.left() + 4, clockContent.top() + 26,
                                  clockContent.height() - 18, clockContent.height() - 18));
        p.setFont(displayFont(28, QFont::DemiBold));
        p.setPen(QColor(247, 250, 255, 232));
        ResponsiveLayout::drawSingleLine(p, QRectF(clockContent.left() + 250,
                                 clockContent.top() + 77, 155, 42),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       QTime::currentTime().toString(QStringLiteral("HH:mm")));
        p.setFont(displayFont(13, QFont::Normal));
        p.setPen(QColor(179, 191, 213, 170));
        ResponsiveLayout::drawSingleLine(p, QRectF(clockContent.left() + 252,
                                 clockContent.top() + 127, 150, 24),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       QDate::currentDate().toString(QStringLiteral("yyyy / MM / dd")));

        drawDashboardInfo(p, kDashboardInfoRect);
        p.restore();
        return;
    }

    paintGlassSurfaceFrame(p, card, radius);

    // Independent card windows intentionally paint only their own payload;
    // blur, rounded clipping and wallpaper sampling are provided by the base.
    if (m_kind == CardKind::Battery && (m_variant == 0 || m_variant == 1)) {
        const bool hasMouse = !m_connectedDevices.isEmpty();
        const ConnectedDevice mouse = hasMouse ? m_connectedDevices.first() : ConnectedDevice{};
        // The reference uses a laptop + mouse pair followed by two empty
        // accessory slots. Keep the empty slots as quiet glass rings, and
        // never synthesize a percentage for a device Windows did not report.
            const qreal ringRadius = qMin(card.width(), card.height()) * .190;
            if (m_variant == 0) {
            // Bring the four rings closer to the card edges while retaining
            // equal breathing room on all sides and between neighboring rings.
            const qreal left = card.left() + card.width() * .265;
            const qreal right = card.left() + card.width() * .735;
            const qreal top = card.top() + card.height() * .265;
            const qreal bottom = card.top() + card.height() * .735;
            drawDeviceRing(p, QPointF(left, top), ringRadius,
                           m_hasBattery ? m_level : -1, 0, m_hasBattery,
                           false, 1.30);
            drawDeviceRing(p, QPointF(right, top), ringRadius,
                           hasMouse ? mouse.level : -1,
                           hasMouse ? mouse.iconKind : -1, hasMouse,
                           false, 1.30);
            drawDeviceRing(p, QPointF(left, bottom), ringRadius,
                           -1, -1, false, false, 1.30);
            drawDeviceRing(p, QPointF(right, bottom), ringRadius,
                           -1, -1, false, false, 1.30);
        } else {
            const qreal sideInset = card.width() * .145;
            const qreal firstX = card.left() + sideInset;
            const qreal spacing = (card.width() - sideInset * 2.0) / 3.0;
            const qreal ringY = card.top() + card.height() * .42;
            drawDeviceRing(p, QPointF(firstX, ringY), ringRadius,
                           m_hasBattery ? m_level : -1, 0, m_hasBattery);
            for (int slot = 0; slot < 3; ++slot) {
                const bool connected = slot < m_connectedDevices.size();
                const ConnectedDevice device = connected ? m_connectedDevices.at(slot)
                                                          : ConnectedDevice{};
                drawDeviceRing(p, QPointF(firstX + spacing * (slot + 1), ringY),
                               ringRadius, device.level,
                               connected ? device.iconKind : -1, connected);
            }
        }
        p.restore();
        return;
    }

    if (m_kind == CardKind::Battery && m_variant == 2) {
        // The 2x2 list follows the compact reference rows. Keep a modest
        // breathing margin around the list while avoiding the oversized
        // typography that made the previous version feel crowded.
        const ResponsiveLayout::Metrics cardMetrics(card);
        const qreal padding = cardMetrics.size(.036);
        const QRectF content = card.adjusted(padding, padding, -padding, -padding);
        const auto row = [&p](const QRectF& area, const QString& name,
                              int level, int iconKind) {
            const ResponsiveLayout::Metrics rowMetrics(area);
            const qreal s = area.height();
            p.setPen(QPen(QColor(244, 248, 255, 210),
                          rowMetrics.stroke(p, .015), Qt::SolidLine,
                          Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            const QPointF iconCenter(area.left() + s * .365, area.center().y());
            if (iconKind == 0) {
                p.drawRoundedRect(QRectF(iconCenter.x() - s * .163,
                                         iconCenter.y() - s * .106,
                                         s * .327, s * .212),
                                  s * .019, s * .019);
                p.drawLine(QPointF(iconCenter.x() - s * .231,
                                   iconCenter.y() + s * .154),
                           QPointF(iconCenter.x() + s * .231,
                                   iconCenter.y() + s * .154));
            } else if (iconKind == 1) {
                p.drawRoundedRect(QRectF(iconCenter.x() - s * .096,
                                         iconCenter.y() - s * .163,
                                         s * .192, s * .327),
                                  s * .077, s * .077);
                p.drawLine(QPointF(iconCenter.x(), iconCenter.y() - s * .106),
                           QPointF(iconCenter.x(), iconCenter.y() - s * .019));
            } else if (iconKind == 2) {
                p.drawRoundedRect(QRectF(iconCenter.x() - s * .192,
                                         iconCenter.y() - s * .115,
                                         s * .385, s * .231),
                                  s * .029, s * .029);
                p.drawLine(QPointF(iconCenter.x() - s * .135,
                                   iconCenter.y() + s * .048),
                           QPointF(iconCenter.x() + s * .135,
                                   iconCenter.y() + s * .048));
            } else if (iconKind == 3) {
                p.drawArc(QRectF(iconCenter.x() - s * .163,
                                 iconCenter.y() - s * .163,
                                 s * .327, s * .327), 0, 180 * 16);
                p.drawLine(QPointF(iconCenter.x() - s * .163, iconCenter.y()),
                           QPointF(iconCenter.x() - s * .163,
                                   iconCenter.y() + s * .135));
                p.drawLine(QPointF(iconCenter.x() + s * .163, iconCenter.y()),
                           QPointF(iconCenter.x() + s * .163,
                                   iconCenter.y() + s * .135));
            } else {
                p.drawRoundedRect(QRectF(iconCenter.x() - s * .115,
                                         iconCenter.y() - s * .163,
                                         s * .231, s * .327),
                                  s * .038, s * .038);
            }
            p.setPen(QColor(246, 249, 255, 235));
            p.setFont(displayFont(qMax(1, qRound(s * .192)), QFont::DemiBold));
            const int nameWidth = qMax(qRound(s * .77),
                                       qRound(area.width() - s * 2.31));
            ResponsiveLayout::drawSingleLine(
                p, QRectF(area.left() + s * .942, area.top(),
                          nameWidth, area.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(p.font()).elidedText(name, Qt::ElideRight, nameWidth));
            p.setFont(displayFont(qMax(1, qRound(s * .173)), QFont::Normal));
            p.setPen(QColor(246, 249, 255, 235));
            ResponsiveLayout::drawSingleLine(
                p, QRectF(area.right() - s * 1.471, area.top(),
                          s * .50, area.height()),
                Qt::AlignRight | Qt::AlignVCenter,
                level >= 0 ? QString::number(qBound(0, level, 100)) + QChar('%')
                           : QStringLiteral("—"));
            p.setPen(QPen(QColor(255, 255, 255, 235),
                          rowMetrics.stroke(p, .019), Qt::SolidLine,
                          Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            const QRectF batteryRect(area.right() - s * .769,
                                     area.center().y() - s * .077,
                                     s * .346, s * .154);
            p.drawRoundedRect(batteryRect, s * .038, s * .038);
            if (level >= 0) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(255, 255, 255, 235));
                const qreal inset = s * .038;
                const qreal fillWidth = (batteryRect.width() - inset * 2.0)
                    * qBound(0, level, 100) / 100.0;
                if (fillWidth > 0.0)
                    p.drawRoundedRect(QRectF(batteryRect.left() + inset,
                                             batteryRect.top() + s * .029,
                                             fillWidth,
                                             batteryRect.height() - s * .058),
                                      s * .019, s * .019);
            }
            p.setPen(QPen(QColor(255, 255, 255, 235),
                          rowMetrics.stroke(p, .019), Qt::SolidLine,
                          Qt::RoundCap, Qt::RoundJoin));
            p.drawLine(QPointF(area.right() - s * .385,
                               area.center().y() - s * .029),
                       QPointF(area.right() - s * .327,
                               area.center().y() - s * .029));
        };
        // Six equal, compact bands match the reference list without
        // changing its outer glass margins. Only rows backed by live devices
        // render payload; the remaining bands stay empty.
        const qreal rowHeight = content.height() / kBatteryListRows;
        const QRectF firstRow(content.left(), content.top(), content.width(), rowHeight);
        const QRectF secondRow(content.left(), content.top() + rowHeight,
                               content.width(), rowHeight);
        row(firstRow, m_hostDeviceName,
            m_hasBattery ? m_level : -1, 0);
        for (int slot = 0; slot < qMin(kBatteryListRows - 1,
                                       int(m_connectedDevices.size())); ++slot) {
            const ConnectedDevice& device = m_connectedDevices.at(slot);
            const QRectF deviceRow(content.left(), secondRow.top() + rowHeight * slot,
                                   content.width(), rowHeight);
            row(deviceRow,
                device.name.isEmpty() ? QStringLiteral("已连接设备") : device.name,
                device.level, device.iconKind);
        }
        p.setPen(QPen(QColor(255, 255, 255, 34),
                      cardMetrics.stroke(p, .0015)));
        for (int i = 1; i < kBatteryListRows; ++i) {
            const qreal y = content.top() + rowHeight * i;
            p.drawLine(QPointF(content.left() + rowHeight * .942, y),
                       QPointF(content.right(), y));
        }
        p.restore();
        return;
    }

    if (m_kind == CardKind::Weather) {
        // Weather cards share one reference coordinate system.  All margins,
        // slots and type sizes derive from the card's short edge, so a 1×1,
        // 1×2 or 2×2 card keeps the same visual proportions at every scale.
        const qreal shortEdge = qMin(card.width(), card.height());
        // A 2×2 card uses the same typography and icon scale as one grid
        // cell; only its information capacity grows. Using the full 2×2 edge
        // would double every glyph and is exactly the mismatch seen before.
        const qreal unit = qMax<qreal>(1.0,
            m_variant == 2 ? shortEdge * .5 : shortEdge);
        const qreal xPad = unit * .11;
        const qreal yPad = unit * .095;
        const QRectF content = card.adjusted(xPad, yPad, -xPad, -yPad);
        const qreal w = content.width();
        const qreal h = content.height();
        const ResponsiveLayout::Metrics weatherMetrics(card);
        const auto weatherFont = [unit](qreal ratio, QFont::Weight weight) {
            return displayFont(qMax(8, qRound(unit * ratio)), weight);
        };
        const auto elided = [&p](const QString& value, const QFont& font,
                                 qreal width) {
            return QFontMetrics(font).elidedText(value, Qt::ElideRight,
                                                 qMax(1, qRound(width)));
        };
        const QString temp = weatherTemperatureLabel(m_weatherTemperature);
        const QString description = m_weatherDescription.isEmpty()
            ? QStringLiteral("获取天气中…") : m_weatherDescription;
        const auto drawLocation = [&p, unit, &elided](const QRectF& area,
                                                       const QString& value) {
            const QString label = elided(value, p.font(), area.width() - unit * .08);
            ResponsiveLayout::drawSingleLine(p, area, Qt::AlignLeft | Qt::AlignVCenter, label);
            const qreal textWidth = QFontMetricsF(p.font()).horizontalAdvance(label);
            const qreal size = unit * .052;
            const QPointF anchor(area.left() + textWidth + unit * .020,
                                 area.center().y());
            QPainterPath marker;
            marker.moveTo(anchor.x(), anchor.y() - size * .10);
            marker.lineTo(anchor.x() + size, anchor.y() - size * .42);
            marker.lineTo(anchor.x() + size * .64, anchor.y() + size * .52);
            marker.lineTo(anchor.x() + size * .44, anchor.y() + size * .14);
            marker.closeSubpath();
            p.save();
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(224, 231, 241, 210));
            p.drawPath(marker);
            p.restore();
        };
        const auto drawHighLow = [&p, &weatherFont, unit](
                                                          const QRectF& area,
                                                          const QString& high,
                                                          const QString& low,
                                                          qreal valueFontRatio) {
            const qreal groupWidth = area.width() * .50;
            const auto group = [&p, &weatherFont, unit, valueFontRatio](const QRectF& box,
                                                        const QString& label,
                                                        const QString& value) {
                const qreal labelWidth = box.width() * .25;
                p.setFont(weatherFont(.040, QFont::DemiBold));
                p.setPen(QColor(205, 213, 226, 205));
                const qreal half = box.height() * .50;
                ResponsiveLayout::drawSingleLine(p, QRectF(box.left(), box.top(), labelWidth, half),
                                Qt::AlignCenter, label.left(1));
                ResponsiveLayout::drawSingleLine(p, QRectF(box.left(), box.top() + half,
                                       labelWidth, half),
                                Qt::AlignCenter, label.mid(1, 1));
                p.setFont(weatherFont(valueFontRatio, QFont::Normal));
                p.setPen(QColor(235, 240, 247, 225));
                ResponsiveLayout::drawSingleLine(p, QRectF(box.left() + labelWidth + unit * .006,
                                       box.top(), box.width() - labelWidth,
                                       box.height()),
                                Qt::AlignLeft | Qt::AlignVCenter,
                                weatherTemperatureLabel(value));
            };
            group(QRectF(area.left(), area.top(), groupWidth, area.height()),
                  QStringLiteral("最高"), high);
            group(QRectF(area.left() + groupWidth, area.top(),
                         groupWidth, area.height()),
                  QStringLiteral("最低"), low);
        };

        if (m_variant == 0) {
            p.setFont(weatherFont(.075, QFont::DemiBold));
            p.setPen(QColor(240, 244, 250, 228));
            drawLocation(QRectF(content.left(), content.top(), w, h * .105),
                         m_weatherLocation);

            // The reference 1×1 card is a vertical stack: location, large
            // temperature, icon/description, then a two-column high/low row.
            p.setFont(weatherFont(.22, QFont::Normal));
            p.setPen(QColor(248, 250, 253, 242));
            const QRectF temperatureArea(content.left(), content.top() + h * .17,
                                         w, h * .27);
            const QRectF temperatureBounds = ResponsiveLayout::drawSingleLine(
                p, temperatureArea, Qt::AlignLeft | Qt::AlignVCenter, temp);
            const QRectF highLowArea(content.left(), content.top() + h * .755,
                                     w, h * .17);
            const qreal iconSize = unit * .115;
            const qreal flowGap = unit * .018;
            const qreal descriptionHeight = h * .10;
            const qreal preferredIconTop = content.top() + h * .44;
            const qreal latestIconTop = highLowArea.top() - flowGap
                                      - descriptionHeight - flowGap - iconSize;
            const qreal iconTop = qMin(latestIconTop,
                qMax(preferredIconTop, temperatureBounds.bottom() + flowGap));
            const QRectF iconArea(content.left(), iconTop, iconSize, iconSize);
            drawWeatherIcon(p, iconArea,
                            m_weatherNight, m_weatherCode);
            p.setFont(weatherFont(kWeatherCompactConditionFontRatio,
                                  QFont::DemiBold));
            p.setPen(QColor(235, 239, 246, 220));
            // A short label is centred to the icon's visual column, while a
            // longer condition expands from the same leading edge. This
            // avoids a special-case offset for single-character states.
            const qreal measuredDescriptionWidth =
                QFontMetricsF(p.font()).tightBoundingRect(description).width();
            const qreal descriptionWidth = qMin(
                w * .72, qMax(iconSize, measuredDescriptionWidth));
            const QRectF descriptionArea(content.left(), iconArea.bottom() + flowGap,
                                         descriptionWidth, descriptionHeight);
            ResponsiveLayout::drawSingleLine(
                p, descriptionArea, Qt::AlignCenter,
                elided(description, p.font(), descriptionArea.width()));
            drawHighLow(highLowArea, m_weatherHigh, m_weatherLow, .115);
            p.restore();
            return;
        }

        if (m_variant == 1) {
            p.setFont(weatherFont(.068, QFont::DemiBold));
            p.setPen(QColor(240, 244, 250, 228));
            drawLocation(QRectF(content.left(), content.top(), w * .56, h * .105),
                         m_weatherLocation);
            p.setFont(weatherFont(.205, QFont::Normal));
            p.setPen(QColor(248, 250, 253, 242));
            ResponsiveLayout::drawSingleLine(p, QRectF(content.left(), content.top() + h * .17,
                                   w * .43, h * .27),
                            Qt::AlignLeft | Qt::AlignVCenter, temp);
            const qreal summaryWidth = w * .37;
            const qreal summaryLeft = content.right() - summaryWidth;
            const qreal iconSize = unit * .14;
            const qreal conditionWidth = iconSize * 1.35;
            const QRectF conditionColumn(content.right() - conditionWidth,
                                         content.top(), conditionWidth, h * .27);
            const QRectF summaryIcon(conditionColumn.center().x() - iconSize * .5,
                                     content.top() - unit * .050,
                                     iconSize, iconSize);
            const QRectF paintedSummaryIcon = drawWeatherIcon(
                p, summaryIcon, m_weatherNight, m_weatherCode);
            p.setFont(weatherFont(kWeatherWideConditionFontRatio,
                                  QFont::DemiBold));
            p.setPen(QColor(232, 237, 245, 220));
            const qreal conditionTextTop = paintedSummaryIcon.bottom()
                                           + unit * kWeatherConditionGapRatio;
            const QRectF conditionText(
                conditionColumn.left(), conditionTextTop,
                conditionColumn.width(),
                qMax<qreal>(0.0, conditionColumn.bottom() - conditionTextTop));
            ResponsiveLayout::drawSingleLine(
                p, conditionText, Qt::AlignCenter,
                elided(description, p.font(), conditionText.width()));
            drawHighLow(QRectF(summaryLeft, content.top() + h * .28,
                               summaryWidth, h * .15),
                        m_weatherHigh, m_weatherLow, .115);
            const qreal stripTop = content.top() + h * .535;
            const qreal stripHeight = h * .40;
            const int hourCount = qMin(kWeatherHourlySlots, m_weatherHours.size());
            if (hourCount > 0) {
                // Keep the six-column rhythm for a partial network response;
                // populated cells must not drift as later hours arrive.
                const qreal cellW = w / kWeatherHourlySlots;
                for (int i = 0; i < hourCount; ++i) {
                    const WeatherHour& hour = m_weatherHours.at(i);
                    const QRectF cell(content.left() + cellW * i, stripTop,
                                      cellW, stripHeight);
                    p.setFont(weatherFont(.060, QFont::Normal));
                    p.setPen(QColor(193, 204, 222, 190));
                    ResponsiveLayout::drawSingleLine(p, QRectF(cell.left(), cell.top(), cell.width(),
                                           stripHeight * .18),
                                    Qt::AlignCenter, hour.time);
                    const qreal icon = unit * .14;
                    drawWeatherIcon(p, QRectF(cell.center().x() - icon * .5,
                                              cell.top() + stripHeight * .24,
                                              icon, icon * .84),
                                    hour.night, hour.code);
                    p.setFont(weatherFont(.060, QFont::DemiBold));
                    p.setPen(QColor(235, 240, 247, 218));
                    ResponsiveLayout::drawSingleLine(p, QRectF(cell.left(),
                                           cell.top() + stripHeight * .88,
                                           cell.width(), stripHeight * .18),
                                    Qt::AlignCenter,
                                    weatherTemperatureLabel(hour.temperature));
                }
            }
            p.restore();
            return;
        }

        p.setFont(weatherFont(kWeatherLargeLocationFontRatio, QFont::DemiBold));
        p.setPen(QColor(241, 245, 251, 232));
        drawLocation(QRectF(content.left(), content.top(), w * .56, unit * .11),
                     m_weatherLocation);
        p.setFont(weatherFont(kWeatherLargeTemperatureFontRatio, QFont::Normal));
        p.setPen(QColor(249, 251, 254, 242));
        ResponsiveLayout::drawSingleLine(p, QRectF(content.left(), content.top() + unit * .155,
                               w * .45, unit * .25),
                        Qt::AlignLeft | Qt::AlignVCenter, temp);
        // Match the reference card's compact weather summary: the monochrome
        // icon and description sit above the high/low range at the right.
        const qreal summaryWidth = w * .35;
        const qreal summaryLeft = content.right() - summaryWidth;
        const qreal iconSize = unit * .14;
        const qreal conditionWidth = iconSize * 1.35;
        const QRectF conditionColumn(content.right() - conditionWidth,
                                     content.top(), conditionWidth, unit * .22);
        const QRectF summaryIcon(conditionColumn.center().x() - iconSize * .5,
                                 content.top() + unit * .025,
                                 iconSize, iconSize * .84);
        const QRectF paintedSummaryIcon = drawWeatherIcon(
            p, summaryIcon, m_weatherNight, m_weatherCode);
        p.setFont(weatherFont(kWeatherLargeConditionFontRatio, QFont::Normal));
        p.setPen(QColor(202, 211, 226, 195));
        const qreal conditionTextTop = paintedSummaryIcon.bottom()
                                       + unit * kWeatherConditionGapRatio;
        const QRectF conditionText(conditionColumn.left(), conditionTextTop,
                                   conditionColumn.width(),
                                   qMax<qreal>(0.0,
                                       conditionColumn.bottom()
                                       - conditionTextTop));
        ResponsiveLayout::drawSingleLine(
            p, conditionText, Qt::AlignCenter,
            elided(description, p.font(), conditionText.width()));
        drawHighLow(QRectF(summaryLeft, content.top() + unit * .230,
                           summaryWidth, unit * .15),
                    m_weatherHigh, m_weatherLow,
                    kWeatherLargeRangeFontRatio);
        // The 2×2 reference reserves roughly the upper third for the current
        // conditions, the next quarter for six hourly cells, and the rest
        // for five daily rows. Express those anchors as card proportions so
        // the composition stays identical at every UI scale.
        const qreal cardHeight = card.height();
        const qreal summaryDividerY = card.top()
                                      + cardHeight * kWeatherSummaryDividerRatio;
        const qreal hourlyTop = card.top()
                                + cardHeight * kWeatherHourlyTopRatio;
        ResponsiveLayout::drawLine(
            p, weatherMetrics, QPointF(content.left(), summaryDividerY),
            QPointF(content.right(), summaryDividerY),
            QColor(255, 255, 255, 105), .003);
        const int hourCount = qMin(kWeatherHourlySlots, m_weatherHours.size());
        if (hourCount > 0) {
            const qreal cellW = w / kWeatherHourlySlots;
            for (int i = 0; i < hourCount; ++i) {
                const WeatherHour& hour = m_weatherHours.at(i);
                const QRectF cell(content.left() + cellW * i, hourlyTop,
                                  cellW, cardHeight * .20);
                p.setFont(weatherFont(kWeatherLargeHourlyFontRatio, QFont::Normal));
                p.setPen(QColor(196, 206, 222, 195));
                ResponsiveLayout::drawSingleLine(p, QRectF(cell.left(), cell.top(), cell.width(),
                                       cardHeight * .055),
                                Qt::AlignCenter, hour.time);
                const qreal icon = unit * .14;
                drawWeatherIcon(p, QRectF(cell.center().x() - icon * .5,
                                          hourlyTop + cardHeight
                                                      * kWeatherHourlyIconOffsetRatio,
                                          icon, icon * .82),
                                hour.night, hour.code);
                p.setFont(weatherFont(kWeatherLargeHourlyFontRatio, QFont::DemiBold));
                p.setPen(QColor(241, 245, 251, 225));
                ResponsiveLayout::drawSingleLine(p, QRectF(cell.left(),
                                       hourlyTop + cardHeight
                                                   * kWeatherHourlyTemperatureOffsetRatio,
                                       cell.width(), cardHeight * .055),
                                Qt::AlignCenter,
                                weatherTemperatureLabel(hour.temperature));
            }
        }
        // Redraw the upper separator after the hourly glyphs.  Some OpenGL
        // paint engines clear the antialiased edge when a later text batch
        // touches the same scanline; drawing it last keeps the reference
        // divider consistently visible at every DPI.
        ResponsiveLayout::drawLine(
            p, weatherMetrics, QPointF(content.left(), summaryDividerY),
            QPointF(content.right(), summaryDividerY),
            QColor(255, 255, 255, 105), .003);
        const qreal dividerY = card.top()
                               + cardHeight * kWeatherDailyDividerRatio;
        ResponsiveLayout::drawLine(
            p, weatherMetrics, QPointF(content.left(), dividerY),
            QPointF(content.right(), dividerY),
            QColor(255, 255, 255, 105), .003);
        const int dayStart = !m_weatherDays.isEmpty()
                             && m_weatherDays.first().label == QStringLiteral("今天")
                           ? 1 : 0;
        const int dayCount = qMin(kWeatherDailySlots,
                                  qMax(0, m_weatherDays.size() - dayStart));
        const qreal rowTop = card.top() + cardHeight * .56;
        // Keep the five-row rhythm for partial responses as well, otherwise a
        // single available day expands through the complete lower section.
        const qreal rowH = qMax<qreal>(
            0.0, (content.bottom() - rowTop) / kWeatherDailySlots);

        // All visible days share one numeric scale. This makes the bar's
        // horizontal position meaningful instead of drawing an identical
        // separator in every row.
        const auto numericTemperature = [](QString value, qreal* result) {
            value.remove(QChar(0x2103));
            value.remove(QChar(0x00B0));
            bool ok = false;
            const qreal number = value.trimmed().toDouble(&ok);
            if (ok && result)
                *result = number;
            return ok;
        };
        qreal scaleMinimum = std::numeric_limits<qreal>::max();
        qreal scaleMaximum = std::numeric_limits<qreal>::lowest();
        bool hasTemperatureScale = false;
        for (int i = 0; i < dayCount; ++i) {
            const WeatherDay& day = m_weatherDays.at(i + dayStart);
            qreal value = 0.0;
            if (numericTemperature(day.low, &value)) {
                scaleMinimum = qMin(scaleMinimum, value);
                scaleMaximum = qMax(scaleMaximum, value);
                hasTemperatureScale = true;
            }
            if (numericTemperature(day.high, &value)) {
                scaleMinimum = qMin(scaleMinimum, value);
                scaleMaximum = qMax(scaleMaximum, value);
                hasTemperatureScale = true;
            }
        }
        const qreal temperatureSpan = scaleMaximum - scaleMinimum;
        for (int i = 0; i < dayCount; ++i) {
            const WeatherDay& day = m_weatherDays.at(i + dayStart);
            const QRectF row(content.left(), rowTop + rowH * i, content.width(), rowH);
            p.setFont(weatherFont(kWeatherLargeDailyLabelFontRatio,
                                  QFont::DemiBold));
            p.setPen(QColor(226, 232, 242, 210));
            ResponsiveLayout::drawSingleLine(p, QRectF(row.left(), row.top(), w * .12, row.height()),
                            Qt::AlignLeft | Qt::AlignVCenter,
                            normalizedWeekdayLabel(day.label));
            const qreal icon = unit * .13;
            drawWeatherIcon(p, QRectF(row.left() + w * .13,
                                      row.center().y() - icon * .5,
                                      icon, icon * .88),
                            day.night, day.code);
            p.setFont(weatherFont(kWeatherLargeDailyTemperatureFontRatio,
                                  QFont::DemiBold));
            p.setPen(QColor(219, 227, 239, 204));
            ResponsiveLayout::drawSingleLine(p, QRectF(row.left() + w * .30, row.top(),
                                   w * .12, row.height()),
                            Qt::AlignLeft | Qt::AlignVCenter,
                            weatherTemperatureLabel(day.low));
            const QRectF rangeArea(row.left() + w * .47,
                                   row.center().y() - row.height() * .12,
                                   row.width() - w * .59,
                                   row.height() * .24);
            qreal low = 0.0;
            qreal high = 0.0;
            const bool hasLow = numericTemperature(day.low, &low);
            const bool hasHigh = numericTemperature(day.high, &high);
            if (hasTemperatureScale && hasLow && hasHigh) {
                const qreal lowRatio = qFuzzyIsNull(temperatureSpan)
                    ? .5 : (low - scaleMinimum) / temperatureSpan;
                const qreal highRatio = qFuzzyIsNull(temperatureSpan)
                    ? .5 : (high - scaleMinimum) / temperatureSpan;
                ResponsiveLayout::drawRangeBar(
                    p, weatherMetrics, rangeArea, lowRatio, highRatio,
                    QColor(255, 255, 255, 28),
                    QColor(238, 242, 248, 185), .010, .035);
            } else {
                ResponsiveLayout::drawLine(
                    p, weatherMetrics,
                    QPointF(rangeArea.left(), rangeArea.center().y()),
                    QPointF(rangeArea.right(), rangeArea.center().y()),
                    QColor(255, 255, 255, 28), .010);
            }
            p.setPen(QColor(219, 227, 239, 204));
            ResponsiveLayout::drawSingleLine(p, QRectF(row.right() - w * .11, row.top(),
                                   w * .11, row.height()),
                            Qt::AlignRight | Qt::AlignVCenter,
                            weatherTemperatureLabel(day.high));
        }
        p.restore();
        return;
    }

    if (m_kind == CardKind::Clock) {
        const ResponsiveLayout::Metrics clockMetrics(card);
        const qreal inset = clockMetrics.size(kClockCardInsetRatio);
        drawClockFace(p, card.adjusted(inset, inset, -inset, -inset), m_variant,
                      (m_variant == 3 || m_variant == 4) ? QStringLiteral("北京") : QString(),
                      m_variant == 4 ? 8 : 0);
        p.restore();
        return;
    }

    if (m_kind == CardKind::Dictionary) {
        const ResponsiveLayout::Metrics dictionaryMetrics(card);
        if (m_variant == 0 || m_variant == 2) {
            const QRectF content = card.adjusted(28, 26, -28, -24);
            p.setFont(displayFont(m_variant == 0 ? 26 : 30, QFont::DemiBold));
            p.setPen(QColor(247, 250, 255, 238));
            ResponsiveLayout::drawSingleLine(
                p, QRectF(content.left(), content.top(), content.width(), 42),
                Qt::AlignLeft | Qt::AlignVCenter,
                m_dictionaryWord.isEmpty() ? QStringLiteral("加载中…")
                                           : m_dictionaryWord);
            p.setFont(displayFont(13, QFont::Normal));
            p.setPen(QColor(191, 203, 224, 205));
            ResponsiveLayout::drawSingleLine(
                p, QRectF(content.left(), content.top() + 52, content.width(), 24),
                Qt::AlignLeft | Qt::AlignVCenter,
                m_dictionaryPhonetic.isEmpty() ? QStringLiteral("每日单词")
                                               : m_dictionaryPhonetic);
            p.setFont(displayFont(11, QFont::Normal));
            p.setPen(QColor(170, 184, 211, 185));
            ResponsiveLayout::drawSingleLine(
                p, QRectF(content.left(), content.bottom() - 30,
                          content.width(), 20),
                Qt::AlignLeft | Qt::AlignVCenter,
                m_variant == 0 ? QStringLiteral("点击刷新")
                               : QStringLiteral("在线词典"));
            p.restore();
            return;
        }
        const QRectF content = card.adjusted(32, 26, -30, -24);
        p.setFont(displayFont(34, QFont::DemiBold));
        p.setPen(QColor(244, 247, 254, 232));
        ResponsiveLayout::drawSingleLine(
            p, QRectF(content.left(), content.top(), content.width(), 42),
            Qt::AlignLeft | Qt::AlignVCenter,
            m_dictionaryWord.isEmpty() ? QStringLiteral("加载中…") : m_dictionaryWord);
        p.setFont(displayFont(18, QFont::Normal));
        p.setPen(QColor(218, 226, 242, 220));
        const QString phonetic = m_dictionaryPhonetic.isEmpty()
                                     ? QStringLiteral("词典数据") : m_dictionaryPhonetic;
        ResponsiveLayout::drawSingleLine(
            p, QRectF(content.left(), content.top() + 46, content.width(), 28),
            Qt::AlignLeft | Qt::AlignVCenter, phonetic);
        p.setFont(displayFont(16, QFont::Normal));
        ResponsiveLayout::drawSingleLine(
            p, QRectF(content.left(), content.top() + 82, content.width(), 28),
            Qt::AlignLeft | Qt::AlignVCenter,
            m_dictionaryPartOfSpeech.isEmpty() ? QStringLiteral("在线词典")
                                               : m_dictionaryPartOfSpeech);
        ResponsiveLayout::drawSingleLine(
            p, QRectF(content.left(), content.top() + 118, content.width(), 42),
            Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(p.font()).elidedText(m_dictionaryDefinition,
                                              Qt::ElideRight, int(content.width())));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(198, 210, 238, 105));
        const qreal barWidth = qMin<qreal>(content.width() - 110, 560.0);
        for (int i = 0; i < 20; ++i)
            p.drawRoundedRect(QRectF(content.left() + i * (barWidth / 20.0),
                                     content.top() + 164, barWidth / 20.0 - 4, 20), 4, 4);
        p.setPen(QPen(QColor(215, 226, 250, 180),
                      dictionaryMetrics.stroke(p, .003)));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(content.left(), card.bottom() - 52, 82, 32), 6, 6);
        p.setFont(displayFont(14, QFont::Normal));
        ResponsiveLayout::drawSingleLine(p, QRectF(content.left(), card.bottom() - 52,
                                             82, 32),
                                   Qt::AlignCenter, QStringLiteral("随机单词"));
        p.restore();
        return;
    }

    if (m_overview) {
        // Four-device overview card (laptop, mouse, and two empty accessory
        // slots), matching the spacing and hierarchy of the supplied image.
        const qreal ringY = card.top() + 132.0;
        const qreal firstX = card.left() + 96.0;
        const qreal spacing = 161.0;
        drawDeviceRing(p, QPointF(firstX, ringY), 60.0,
                       m_hasBattery ? m_level : -1, 0, m_hasBattery);
        for (int slot = 0; slot < 3; ++slot) {
            const bool connected = slot < m_connectedDevices.size();
            const ConnectedDevice device = connected ? m_connectedDevices.at(slot)
                                                      : ConnectedDevice{};
            drawDeviceRing(p, QPointF(firstX + spacing * (slot + 1), ringY), 60.0,
                           device.level, connected ? device.iconKind : -1, connected);
        }
        p.restore();
        return;
    }

    const ResponsiveLayout::Metrics detailMetrics(card);
    const QRectF content = card.adjusted(detailMetrics.size(.073),
                                         detailMetrics.size(.061),
                                         -detailMetrics.size(.073),
                                         -detailMetrics.size(.061));

    // Detail card: header, large battery percentage, status and metadata.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, m_charging ? 235 : 170));
    p.drawEllipse(QRectF(content.left(), content.top() + 3, 7, 7));
    p.setFont(displayFont(10, QFont::DemiBold));
    p.setPen(QColor(208, 217, 235, 210));
    ResponsiveLayout::drawSingleLine(p, QRectF(content.left() + 15,
                                         content.top() - 1, 120, 18),
                               Qt::AlignLeft | Qt::AlignVCenter,
                               QStringLiteral("BATTERY"));
    p.setFont(displayFont(10, QFont::Normal));
    p.setPen(QColor(177, 188, 211, 135));
    ResponsiveLayout::drawSingleLine(
        p, QRectF(content.right() - 125, content.top() - 1, 125, 18),
        Qt::AlignRight | Qt::AlignVCenter,
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));

    drawBatteryIcon(p, QRectF(content.left(), content.top() + 43, 104, 61),
                    m_hasBattery ? m_level : -1, m_charging);

    p.setFont(displayFont(52, QFont::DemiBold));
    p.setPen(QColor(249, 251, 255));
    ResponsiveLayout::drawSingleLine(
        p, QRectF(content.left() + 125, content.top() + 29, 150, 65),
        Qt::AlignLeft | Qt::AlignVCenter,
        m_hasBattery ? QString::number(m_level) : QStringLiteral("—"));
    if (m_hasBattery) {
        p.setFont(displayFont(21, QFont::Normal));
        p.setPen(QColor(196, 205, 222, 210));
        ResponsiveLayout::drawSingleLine(p, QRectF(content.left() + 210,
                                             content.top() + 40, 55, 30),
                                   Qt::AlignLeft | Qt::AlignVCenter,
                                   QStringLiteral("%"));
    }

    const QString state = !m_hasBattery ? QStringLiteral("POWER ADAPTER")
                                        : (m_charging ? QStringLiteral("CHARGING") : QStringLiteral("ON BATTERY"));
    p.setFont(displayFont(11, QFont::Medium));
    p.setPen(QColor(255, 255, 255, m_charging ? 235 : 188));
    ResponsiveLayout::drawSingleLine(p, QRectF(content.left() + 128,
                                         content.top() + 90, 155, 22),
                               Qt::AlignLeft | Qt::AlignVCenter, state);

    p.setPen(QPen(QColor(255, 255, 255, 25),
                  detailMetrics.stroke(p, .003)));
    p.drawLine(content.left(), content.bottom() - detailMetrics.size(.082),
               content.right(), content.bottom() - detailMetrics.size(.082));
    p.setFont(displayFont(10, QFont::Normal));
    p.setPen(QColor(161, 173, 199, 150));
    ResponsiveLayout::drawSingleLine(
        p, QRectF(content.left(), content.bottom() - 21, content.width(), 18),
        Qt::AlignLeft | Qt::AlignVCenter,
        m_charging ? QStringLiteral("Power source  ·  Connected")
                   : QStringLiteral("Power source  ·  Internal battery"));
    p.setPen(QColor(161, 173, 199, 122));
    ResponsiveLayout::drawSingleLine(
        p, QRectF(content.left(), content.bottom() - 21, content.width(), 18),
        Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("Updated now"));
    p.restore();
}

void BatteryWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        if (m_kind == CardKind::Weather) {
            // The location label acts as the city search affordance. Keep the
            // same Qt input dialog available from the context menu as well.
            searchWeatherCity();
            event->accept();
            return;
        }
        // A double click no longer changes a composite layout: each card is
        // an independent desktop widget. The tray action can re-align them.
        if (m_primary)
            arrangeGroup();
    }
    QOpenGLWidget::mouseDoubleClickEvent(event);
}

void BatteryWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragOrigin = pos();
        m_clickPending = true;
    }
    LiquidGlassWidget::mousePressEvent(event);
}

void BatteryWidget::windowDragStarted()
{
    NativeWindows::restoreApplicationWindows(m_dragHiddenWindows);
    m_dragHiddenWindows.clear();
    QSet<WId> widgetWindows;
    for (BatteryWidget* widget : std::as_const(s_instances)) {
        if (!widget || !widget->isVisible())
            continue;
        widgetWindows.insert(widget->winId());
    }
    m_dragHiddenWindows = NativeWindows::hideApplicationWindows(widgetWindows);
}

void BatteryWidget::windowDragFinished()
{
    m_clickPending = false;
    hideGridPreview();
    bool shouldSave = false;
    if (!m_restoringPosition) {
        const QRect target = nearestGridRect(m_kind, m_variant, pos(), m_uiScale, this);
        m_restoringPosition = true;
        if (target.isValid()) {
            setFixedSize(target.size());
            move(target.topLeft());
        } else {
            move(m_dragOrigin);
        }
        m_restoringPosition = false;
        m_hasSavedPosition = true;
        shouldSave = true;
    }

    NativeWindows::restoreApplicationWindows(m_dragHiddenWindows);
    m_dragHiddenWindows.clear();
    // Persist only after ordinary application windows have been restored.
    if (shouldSave)
        saveAllConfigurations();
}

void BatteryWidget::mouseReleaseEvent(QMouseEvent* event)
{
    const bool clicked = event->button() == Qt::LeftButton && m_clickPending
                         && !windowDragging() && !mouseThroughEnabled();
    m_clickPending = false;
    LiquidGlassWidget::mouseReleaseEvent(event);
    if (!clicked)
        return;
    const QPointF point = event->position() / m_uiScale;
    if (m_kind == CardKind::Dictionary
        && QRectF(32, height() / m_uiScale - 52, 82, 32).contains(point))
        refreshDictionary();
    else if (m_kind == CardKind::Weather
             && QRectF(36, 27, width() / m_uiScale - 64, 32).contains(point))
        searchWeatherCity();
}

void BatteryWidget::contextMenuEvent(QContextMenuEvent* event)
{
    if (!event)
        return;

    QMenu menu(this);
    QAction* cityAction = nullptr;
    if (m_kind == CardKind::Weather) {
        cityAction = menu.addAction(QStringLiteral("搜索城市…"));
        menu.addSeparator();
    }
    QAction* removeAction = menu.addAction(QStringLiteral("删除此组件"));
    // Keep one card alive as the tray/settings owner. This avoids leaving a
    // running process with no way to reopen the widget library after the last
    // card is removed.
    removeAction->setEnabled(s_instances.size() > 1);
    QAction* selected = menu.exec(event->globalPos());
    if (selected == cityAction) {
        searchWeatherCity();
    } else if (selected == removeAction && removeAction->isEnabled()) {
        const bool wasPrimary = m_primary;
        if (m_trayIcon)
            m_trayIcon->hide();

        // Remove from the occupancy set before persisting, so the deleted
        // card cannot remain in widgets.json or block a future drop.
        s_instances.removeAll(this);
        if (wasPrimary && !s_instances.isEmpty()) {
            BatteryWidget* successor = s_instances.first();
            successor->m_primary = true;
            successor->setupTrayIcon();
        }
        for (BatteryWidget* widget : s_instances) {
            if (widget && widget->m_primary) {
                widget->updateTrayVisibilityLabel();
                widget->updateLayoutLabel();
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

void BatteryWidget::moveEvent(QMoveEvent* event)
{
    LiquidGlassWidget::moveEvent(event);
    if (!m_restoringPosition && windowDragging())
        showGridPreviewRect(nearestGridRect(m_kind, m_variant, pos(), m_uiScale, this));
}
