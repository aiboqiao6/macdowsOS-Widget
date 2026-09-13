#include "batterywidget.h"
#include "widgetlibrarydialog.h"

#include <QApplication>
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
#include <QComboBox>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRegion>
#include <QScreen>
#include <QSet>
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
#include <QUrl>
#include <QUrlQuery>
#include <QRandomGenerator>
#include <QtMath>
#include <QSaveFile>
#include <QFile>
#include <QUuid>
#include <limits>

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
// Grid pitch is the card size plus a small adaptive gutter. The gutter scales
// with the UI and with the shorter display edge so cards keep a consistent
// visual rhythm on both small and high-resolution screens.
constexpr qreal kGridBaseGap = 16.0;

QFont displayFont(int pointSize, QFont::Weight weight = QFont::Normal)
{
    QFont font;
    // Explicit fallback keeps CJK labels on the same baseline as Latin text.
    font.setFamilies({QStringLiteral("Segoe UI Variable Display"),
                      QStringLiteral("Microsoft YaHei UI"),
                      QStringLiteral("Segoe UI")});
    font.setPointSize(pointSize);
    font.setWeight(weight);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

int gridColumnSpan(BatteryWidget::CardKind kind)
{
    return (kind == BatteryWidget::CardKind::Battery
            || kind == BatteryWidget::CardKind::Dictionary) ? 2 : 1;
}

int gridGapForArea(const QRect& area, qreal scale)
{
    if (!area.isValid())
        return qMax(4, qRound(kGridBaseGap * scale));

    const qreal shortestEdge = qreal(qMin(area.width(), area.height()));
    const qreal screenFactor = qBound<qreal>(0.72, shortestEdge / 1080.0, 1.65);
    return qBound(4, qRound(kGridBaseGap * scale * screenFactor), 28);
}

QSize gridWidgetSize(BatteryWidget::CardKind kind, int cell, int gap)
{
    const int span = gridColumnSpan(kind);
    return QSize(cell * span + gap * (span - 1), cell);
}

QPoint gridOrigin(const QRect& area, int gap)
{
    // Keep the outer gutter identical to the gap between neighboring cells.
    // This makes the grid rhythm symmetrical instead of placing the first
    // card flush against the available-work-area edge.
    return area.topLeft() + QPoint(gap, gap);
}

class GridSnapOverlay final : public QWidget
{
public:
    GridSnapOverlay()
    {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                       | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void display(const QRect& target)
    {
        if (geometry() != target)
            setGeometry(target);
        if (!isVisible())
            show();
        raise();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal radius = qMin<qreal>(28.0, qMin(width(), height()) * .12);
        const QRectF bounds = QRectF(rect()).adjusted(3.0, 3.0, -3.0, -3.0);

        painter.setBrush(QColor(105, 174, 255, 20));
        painter.setPen(QPen(QColor(91, 164, 255, 58), 5.0));
        painter.drawRoundedRect(bounds, radius, radius);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(225, 241, 255, 235), 1.35));
        painter.drawRoundedRect(bounds, radius, radius);
    }
};

GridSnapOverlay* gridOverlay()
{
    static GridSnapOverlay* overlay = new GridSnapOverlay;
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

QList<BatteryWidget*> BatteryWidget::s_instances;

BatteryWidget::BatteryWidget(QWidget* parent, CardKind kind, bool primary,
                             const QString& instanceId)
    : LiquidGlassWidget(parent), m_kind(kind), m_primary(primary),
      m_instanceId(instanceId.isEmpty()
                       ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                       : instanceId)
{
    s_instances.append(this);
    QSettings settings;
    m_uiScale = qBound<qreal>(0.40, settings.value(QStringLiteral("appearance/scale"), 1.0).toDouble(), 1.35);
    setGlassMargins(0);
    const int initialCell = qMax(1, qRound(kGridBaseCell * m_uiScale));
    QScreen* initialScreen = QGuiApplication::primaryScreen();
    const int initialGap = initialScreen
        ? gridGapForArea(initialScreen->availableGeometry(), m_uiScale)
        : qMax(4, qRound(kGridBaseGap * m_uiScale));
    setFixedSize(gridWidgetSize(m_kind, initialCell, initialGap));
    setGlassRadius(34.0 * m_uiScale);
    configureGlass(3.8f, 1.32f, 4.5f, 3, 0.008f);
    // The clock's second hand uses sub-second interpolation. Keep its idle
    // cadence at the previous ~15 FPS while static cards can sleep at 5 FPS.
    setAnimationEnabled(m_kind == CardKind::Clock);
    const bool savedSystemBlur = settings.value(QStringLiteral("appearance/systemBlur"), false).toBool();
    const bool savedMouseThrough = settings.value(QStringLiteral("interaction/mouseThrough"), false).toBool();
    const bool savedLowPowerRefresh = settings.value(QStringLiteral("interaction/lowPowerRefresh"), false).toBool();
    const int savedBlurStrength = settings.value(QStringLiteral("appearance/blurStrength"), 55).toInt();
    const qreal savedOpacity = settings.value(QStringLiteral("appearance/opacity"), 1.0).toDouble();
    const int savedBackend = settings.value(QStringLiteral("performance/renderBackend"), 0).toInt();
    setRenderBackend(static_cast<RenderBackend>(qBound(0, savedBackend, 2)));
    setSystemBlurEnabled(savedSystemBlur);
    setMaterialBlurStrength(savedBlurStrength);
    setMouseThroughEnabled(savedMouseThrough);
    setLowPowerRefreshEnabled(savedLowPowerRefresh);
    setGlassOpacity(savedOpacity);
    setToolTip(m_kind == CardKind::Battery ? QStringLiteral("电量 · 拖动移动组件")
               : (m_kind == CardKind::Weather ? QStringLiteral("天气 · 拖动移动组件")
                  : (m_kind == CardKind::Clock ? QStringLiteral("时钟 · 拖动移动组件")
                     : QStringLiteral("词典 · 拖动移动组件"))));

    if (m_kind == CardKind::Battery) {
        connect(&m_refreshTimer, &QTimer::timeout, this, &BatteryWidget::refreshBattery);
        m_refreshTimer.start(2000);
    }

    m_weatherLocation = settings.value(QStringLiteral("weather/location"),
                                       QStringLiteral("Beijing")).toString().trimmed();
    if (m_weatherLocation.isEmpty())
        m_weatherLocation = QStringLiteral("Beijing");
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
    m_settingsTimer.setInterval(1500);
    connect(&m_settingsTimer, &QTimer::timeout, this, [this]() {
        QSettings current;
        const QString savedLocation = current.value(QStringLiteral("weather/location"),
                                                    QStringLiteral("Beijing")).toString().trimmed();
        if (m_kind == CardKind::Weather && !savedLocation.isEmpty()
            && savedLocation != m_weatherLocation) {
            m_weatherLocation = savedLocation;
            refreshWeather();
        }
        const qreal scale = qBound<qreal>(0.40, current.value(QStringLiteral("appearance/scale"), 1.0).toDouble(), 1.35);
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
    s_instances.removeAll(this);
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
    connect(m_trayIcon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                    toggleWidgetVisibility();
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
            widget->moveToGroupPosition();
            widget->captureDesktopBackdrop();
            widget->show();
        }
    }
    updateTrayVisibilityLabel();
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
    m_uiScale = qBound<qreal>(0.40, scale, 1.35);
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
    setFixedSize(gridWidgetSize(m_kind, cell, gap));
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
    int column = 0;
    int row = qMax(0, rows - 1);
    switch (m_kind) {
    case CardKind::Battery:
        column = qMax(0, columns - 2); row = qMax(0, rows - 2); break;
    case CardKind::Weather:
        column = qMax(0, columns - 2); break;
    case CardKind::Clock:
        column = qMax(0, columns - 1); break;
    case CardKind::Dictionary:
        column = qMax(0, columns - 4); break;
    }
    const QRect target = nearestGridRect(m_kind,
        gridOrigin(area, gap) + QPoint(column * pitch, row * pitch), m_uiScale, this);
    if (target.isValid()) {
        m_restoringPosition = true;
        move(target.topLeft());
        m_restoringPosition = false;
    }
}

void BatteryWidget::arrangeGroup()
{
    // Vacate old cells first so the deterministic layout cannot be blocked by
    // the widgets' own previous arrangement.
    int offset = 0;
    for (BatteryWidget* widget : s_instances) {
        widget->m_restoringPosition = true;
        widget->move(-100000 - offset, -100000 - offset);
        widget->m_restoringPosition = false;
        offset += 8;
    }
    for (BatteryWidget* widget : s_instances) {
        widget->moveToGroupPosition();
        widget->captureDesktopBackdrop();
    }
}

void BatteryWidget::syncGroupSettings(qreal scale, bool systemBlur, int blurStrength,
                                      qreal opacity, bool mouseThrough, bool lowPower,
                                      RenderBackend renderBackend)
{
    QSettings().setValue(QStringLiteral("performance/renderBackend"), int(renderBackend));
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
        item.insert(QStringLiteral("columnSpan"), gridColumnSpan(widget->m_kind));
        item.insert(QStringLiteral("rowSpan"), 1);
        widgets.append(item);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 2);
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
    return nearestGridRect(m_kind, requested, m_uiScale, this).topLeft();
}

QRect BatteryWidget::nearestGridRect(CardKind kind, const QPoint& requestedTopLeft,
                                     qreal scale, const BatteryWidget* ignored)
{
    const int cell = qMax(1, qRound(kGridBaseCell * scale));
    const int columnSpan = gridColumnSpan(kind);
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
    const QSize widgetSize = gridWidgetSize(kind, cell, gap);
    const int pitch = cell + gap;
    // Reserve one gap on every outer edge, just as between cells. A candidate
    // is valid only when its right/bottom edge also stays inside that gutter.
    const int innerWidth = area.width() - gap * 2;
    const int innerHeight = area.height() - gap * 2;
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
                if (!widget || widget == ignored)
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

void BatteryWidget::showGridPreview(CardKind kind, const QPoint& desktopPoint)
{
    QSettings settings;
    const qreal scale = qBound<qreal>(0.40,
        settings.value(QStringLiteral("appearance/scale"), 1.0).toDouble(), 1.35);
    QScreen* screen = QGuiApplication::screenAt(desktopPoint);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect area = screen ? screen->availableGeometry() : QRect();
    const int cell = qMax(1, qRound(kGridBaseCell * scale));
    const int gap = gridGapForArea(area, scale);
    const QSize size = gridWidgetSize(kind, cell, gap);
    showGridPreviewRect(nearestGridRect(kind,
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
    gridOverlay()->hide();
}

bool BatteryWidget::placeAtDesktopPoint(const QPoint& point)
{
    // Center the card under the drop cursor, then snap its top-left to the
    // nearest scale-aware grid cell.
    const QPoint desired = point - QPoint(width() / 2, height() / 2);
    const QRect target = nearestGridRect(m_kind, desired, m_uiScale, this);
    if (!target.isValid())
        return false;
    m_restoringPosition = true;
    move(target.topLeft());
    m_restoringPosition = false;
    m_hasSavedPosition = true;
    captureDesktopBackdrop();
    saveAllConfigurations();
    return true;
}

BatteryWidget* BatteryWidget::addWidget(CardKind kind, const QPoint& desktopPoint)
{
    auto* widget = new BatteryWidget(nullptr, kind, false);
    widget->setAttribute(Qt::WA_DeleteOnClose, true);
    if (!widget->placeAtDesktopPoint(desktopPoint)) {
        delete widget;
        return nullptr;
    }
    widget->show();
    saveAllConfigurations();
    return widget;
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
        const CardKind kind = kindFromName(item.value(QStringLiteral("type")).toString());
        // Keep the tray/settings owner on the battery card even when the
        // configuration was reordered after adding custom cards.
        const bool primary = kind == CardKind::Battery && !primaryAssigned;
        auto* widget = new BatteryWidget(nullptr,
                                         kind,
                                         primary,
                                         item.value(QStringLiteral("id")).toString());
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
        const QRect restoredTarget = nearestGridRect(kind, restoredPosition,
                                                     widget->m_uiScale, widget);
        if (!restoredTarget.isValid()) {
            delete widget;
            continue;
        }
        primaryAssigned = primaryAssigned || primary;
        widget->m_restoringPosition = true;
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
        m_library = new WidgetLibraryDialog(this);
        connect(m_library, &WidgetLibraryDialog::widgetDropped, this,
                [](int kind, const QPoint& globalPos) {
                    addWidget(static_cast<CardKind>(qBound(0, kind, 3)), globalPos);
                });
        connect(m_library, &WidgetLibraryDialog::widgetDragPreview, this,
                [](int kind, const QPoint& globalPos, bool visible) {
                    if (visible)
                        showGridPreview(static_cast<CardKind>(qBound(0, kind, 3)),
                                        globalPos);
                    else
                        hideGridPreview();
                });
    }
    m_library->prepareBackdrop();
    m_library->show();
    m_library->raise();
    m_library->activateWindow();
}

void BatteryWidget::refreshWeather()
{
    if (!m_weatherNetwork)
        return;

    ++m_weatherReplySerial;
    QUrl url(QStringLiteral("https://wttr.in/%1").arg(QString::fromUtf8(QUrl::toPercentEncoding(m_weatherLocation))));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("format"), QStringLiteral("j1"));
    query.addQueryItem(QStringLiteral("lang"), QStringLiteral("zh"));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("macdowsOS Widget/1.0 (Qt)"));
    m_weatherLoading = true;
    m_weatherDescription = QStringLiteral("获取天气中…");
    update();
    QNetworkReply* reply = m_weatherNetwork->get(request);
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
    const int serial = reply->property("weatherSerial").toInt();
    if (serial != m_weatherReplySerial) {
        reply->deleteLater();
        return;
    }

    bool ok = reply->error() == QNetworkReply::NoError;
    QJsonDocument document;
    if (ok)
        document = QJsonDocument::fromJson(reply->readAll());
    if (ok && !document.isObject())
        ok = false;

    if (ok) {
        const QJsonObject root = document.object();
        const auto valueString = [](const QJsonValue& value) {
            return value.isString() ? value.toString()
                                    : (value.isDouble() ? QString::number(value.toDouble()) : QString());
        };
        const QJsonArray current = root.value(QStringLiteral("current_condition")).toArray();
        const QJsonObject now = current.isEmpty() ? QJsonObject{} : current.first().toObject();
        const QJsonArray forecast = root.value(QStringLiteral("weather")).toArray();
        const QJsonObject today = forecast.isEmpty() ? QJsonObject{} : forecast.first().toObject();
        m_weatherTemperature = valueString(now.value(QStringLiteral("temp_C")));
        const QJsonArray chineseDescription = now.value(QStringLiteral("lang_zh")).toArray();
        m_weatherDescription = chineseDescription.isEmpty() ? QString()
                                   : chineseDescription.first().toObject()
                                         .value(QStringLiteral("value")).toString();
        if (m_weatherDescription.isEmpty())
        {
            const QJsonArray englishDescription = now.value(QStringLiteral("weatherDesc")).toArray();
            m_weatherDescription = englishDescription.isEmpty() ? QString()
                                       : englishDescription.first().toObject()
                                             .value(QStringLiteral("value")).toString();
        }
        m_weatherHigh = valueString(today.value(QStringLiteral("maxtempC")));
        m_weatherLow = valueString(today.value(QStringLiteral("mintempC")));
        const int hour = QTime::currentTime().hour();
        m_weatherNight = hour < 6 || hour >= 18;
        m_weatherLoading = false;
        m_weatherUpdated = QDateTime::currentDateTime();
    } else {
        m_weatherLoading = false;
        m_weatherTemperature.clear();
        m_weatherHigh.clear();
        m_weatherLow.clear();
        m_weatherDescription = QStringLiteral("天气暂不可用");
        m_weatherUpdated = QDateTime::currentDateTime();
    }
    reply->deleteLater();
    update();
}

void BatteryWidget::refreshDictionary()
{
    if (!m_dictionaryNetwork)
        return;
    ++m_dictionaryReplySerial;
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
    reply->setProperty("dictionarySerial", m_dictionaryReplySerial);
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
    bool accepted = false;
    const QString city = QInputDialog::getText(this, QStringLiteral("搜索城市"),
                                                QStringLiteral("城市或地区："),
                                                QLineEdit::Normal, m_weatherLocation,
                                                &accepted).trimmed();
    if (!accepted || city.isEmpty() || city == m_weatherLocation)
        return;
    m_weatherLocation = city;
    QSettings().setValue(QStringLiteral("weather/location"), m_weatherLocation);
    for (BatteryWidget* widget : s_instances) {
        if (!widget || widget->m_kind != CardKind::Weather)
            continue;
        widget->m_weatherLocation = m_weatherLocation;
        widget->refreshWeather();
    }
}

void BatteryWidget::drawWeatherIcon(QPainter& p, const QRectF& rect, bool night) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPointF sun(rect.left() + rect.width() * .34, rect.top() + rect.height() * .36);
    const qreal radius = rect.width() * .19;
    if (!night) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 216, 126, 225));
        p.drawEllipse(sun, radius, radius);
        p.setPen(QPen(QColor(255, 228, 158, 185), qMax<qreal>(1.5, rect.width() * .025),
                      Qt::SolidLine, Qt::RoundCap));
        for (int i = 0; i < 8; ++i) {
            const qreal a = i * M_PI / 4.0;
            const QPointF a0 = sun + QPointF(std::cos(a), std::sin(a)) * (radius * 1.45);
            const QPointF a1 = sun + QPointF(std::cos(a), std::sin(a)) * (radius * 1.95);
            p.drawLine(a0, a1);
        }
    } else {
        // Build a crescent path instead of using CompositionMode_Clear, so
        // the icon never punches transparent holes through the glass card.
        QPainterPath moon;
        moon.addEllipse(QPointF(rect.left() + rect.width() * .39,
                                rect.top() + rect.height() * .34),
                        radius * 1.15, radius * 1.15);
        QPainterPath cut;
        cut.addEllipse(QPointF(rect.left() + rect.width() * .49,
                               rect.top() + rect.height() * .27),
                       radius * 1.10, radius * 1.10);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(220, 232, 255, 220));
        p.drawPath(moon.subtracted(cut));
    }
    QPainterPath cloud;
    cloud.moveTo(rect.left() + rect.width() * .17, rect.bottom() - rect.height() * .22);
    cloud.cubicTo(rect.left() + rect.width() * .12, rect.bottom() - rect.height() * .48,
                  rect.left() + rect.width() * .31, rect.bottom() - rect.height() * .55,
                  rect.left() + rect.width() * .40, rect.bottom() - rect.height() * .39);
    cloud.cubicTo(rect.left() + rect.width() * .47, rect.bottom() - rect.height() * .64,
                  rect.left() + rect.width() * .75, rect.bottom() - rect.height() * .58,
                  rect.left() + rect.width() * .77, rect.bottom() - rect.height() * .32);
    cloud.cubicTo(rect.left() + rect.width() * .95, rect.bottom() - rect.height() * .34,
                  rect.left() + rect.width() * .94, rect.bottom() - rect.height() * .08,
                  rect.left() + rect.width() * .73, rect.bottom() - rect.height() * .08);
    cloud.lineTo(rect.left() + rect.width() * .26, rect.bottom() - rect.height() * .08);
    cloud.cubicTo(rect.left() + rect.width() * .08, rect.bottom() - rect.height() * .08,
                  rect.left() + rect.width() * .07, rect.bottom() - rect.height() * .22,
                  rect.left() + rect.width() * .17, rect.bottom() - rect.height() * .22);
    cloud.closeSubpath();
    p.setPen(QPen(QColor(241, 246, 255, 205), qMax<qreal>(1.0, rect.width() * .018)));
    p.setBrush(QColor(231, 240, 255, 130));
    p.drawPath(cloud);
    p.restore();
}

void BatteryWidget::drawAnalogClock(QPainter& p, const QRectF& rect) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPointF c = rect.center();
    // Keep a clear, even breathing margin around the dial. A radius near half
    // the card makes the outline touch the rounded glass corners after DPI or
    // scale changes.
    const qreal r = qMin(rect.width(), rect.height()) * .415;
    p.setPen(QPen(QColor(247, 250, 255, 72), 1.0));
    p.setBrush(QColor(255, 255, 255, 16));
    p.drawEllipse(c, r, r);
    for (int i = 0; i < 60; ++i) {
        const qreal a = (i / 60.0) * 2.0 * M_PI - M_PI / 2.0;
        const qreal outer = r - 3.0;
        const qreal inner = r - ((i % 5 == 0) ? 13.0 : 7.0);
        p.setPen(QPen(QColor(245, 249, 255, (i % 5 == 0) ? 210 : 100),
                      (i % 5 == 0) ? 2.0 : 1.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c + QPointF(std::cos(a), std::sin(a)) * inner,
                   c + QPointF(std::cos(a), std::sin(a)) * outer);
    }
    p.setFont(displayFont(qMax(13, qRound(r * .23)), QFont::DemiBold));
    p.setPen(QColor(238, 243, 253, 218));
    for (int hour = 1; hour <= 12; ++hour) {
        const qreal a = (hour / 12.0) * 2.0 * M_PI - M_PI / 2.0;
        const QPointF pos = c + QPointF(std::cos(a), std::sin(a)) * (r - 27.0);
        const QString label = QString::number(hour);
        p.drawText(QRectF(pos.x() - 13, pos.y() - 13, 26, 26),
                   Qt::AlignCenter, label);
    }
    const QTime time = QTime::currentTime();
    const auto hand = [&p, c, r](qreal angle, qreal length, qreal width, const QColor& color) {
        p.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c, c + QPointF(std::cos(angle), std::sin(angle)) * length);
    };
    const qreal sec = (time.second() + time.msec() / 1000.0) / 60.0 * 2.0 * M_PI - M_PI / 2.0;
    const qreal min = (time.minute() + time.second() / 60.0) / 60.0 * 2.0 * M_PI - M_PI / 2.0;
    const qreal hour = ((time.hour() % 12) + time.minute() / 60.0) / 12.0 * 2.0 * M_PI - M_PI / 2.0;
    hand(hour, r * .52, 5.0, QColor(250, 252, 255, 230));
    hand(min, r * .73, 3.2, QColor(250, 252, 255, 220));
    hand(sec, r * .80, 1.4, QColor(255, 174, 172, 230));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 195, 194, 240));
    p.drawEllipse(c, 4.0, 4.0);
    p.restore();
}

void BatteryWidget::drawDashboardInfo(QPainter& p, const QRectF& card) const
{
    const QRectF content = card.adjusted(28, 25, -28, -24);
    p.setFont(displayFont(11, QFont::DemiBold));
    p.setPen(QColor(205, 215, 232, 190));
    p.drawText(QRectF(content.left(), content.top(), 180, 24), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("TODAY"));
    p.setFont(displayFont(30, QFont::DemiBold));
    p.setPen(QColor(248, 251, 255, 235));
    p.drawText(QRectF(content.left(), content.top() + 34, content.width(), 52),
               Qt::AlignLeft | Qt::AlignVCenter,
               QDate::currentDate().toString(QStringLiteral("MMMM d")));
    p.setFont(displayFont(14, QFont::Normal));
    p.setPen(QColor(184, 195, 216, 175));
    p.drawText(QRectF(content.left(), content.top() + 90, content.width(), 24),
               Qt::AlignLeft | Qt::AlignVCenter,
               QDate::currentDate().toString(QStringLiteral("dddd · yyyy")));

    const QRectF line(content.left(), content.top() + 139, content.width(), 1);
    p.fillRect(line, QColor(255, 255, 255, 28));
    p.setFont(displayFont(12, QFont::Normal));
    p.setPen(QColor(190, 201, 221, 170));
    p.drawText(QRectF(content.left(), content.top() + 158, content.width() * .55, 24),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("电源状态"));
    p.setPen(m_charging ? QColor(123, 236, 177, 220) : QColor(237, 242, 250, 205));
    p.drawText(QRectF(content.left(), content.top() + 186, content.width() * .55, 28),
               Qt::AlignLeft | Qt::AlignVCenter,
               m_hasBattery ? (m_charging ? QStringLiteral("正在充电") : QStringLiteral("使用电池"))
                            : QStringLiteral("已接入电源"));
    p.setPen(QColor(190, 201, 221, 170));
    p.drawText(QRectF(content.left() + content.width() * .55, content.top() + 158,
                      content.width() * .45, 24), Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("设备"));
    p.setPen(QColor(237, 242, 250, 205));
    p.drawText(QRectF(content.left() + content.width() * .55, content.top() + 186,
                      content.width() * .45, 28), Qt::AlignRight | Qt::AlignVCenter,
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

    auto* renderBox = new QComboBox(&dialog);
    renderBox->addItem(QStringLiteral("自动（推荐）"), int(RenderBackend::AutoBackend));
    renderBox->addItem(QStringLiteral("GPU · OpenGL Shader"), int(RenderBackend::GpuBackend));
    renderBox->addItem(QStringLiteral("CPU · 软件缓存模糊"), int(RenderBackend::CpuBackend));
    renderBox->setCurrentIndex(renderBox->findData(int(renderBackend())));
    form->addRow(QStringLiteral("渲染方式"), renderBox);

    auto* locationEdit = new QLineEdit(m_weatherLocation, &dialog);
    locationEdit->setPlaceholderText(QStringLiteral("例如 Beijing、Shanghai、Tokyo"));
    form->addRow(QStringLiteral("天气城市"), locationEdit);

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

    auto* lowPower = new QCheckBox(QStringLiteral("鼠标穿透时降低刷新率（4 FPS，节省性能）"), &dialog);
    lowPower->setChecked(lowPowerRefreshEnabled());
    lowPower->setEnabled(mouseThrough->isChecked());
    connect(mouseThrough, &QCheckBox::toggled, lowPower, &QWidget::setEnabled);
    form->addRow(QString(), lowPower);

    auto* startup = new QCheckBox(QStringLiteral("登录 Windows 时自动启动"), &dialog);
    startup->setChecked(isStartupEnabled());
    form->addRow(QString(), startup);
    layout->addLayout(form);

    auto* hint = new QLabel(QStringLiteral("仅模糊模式不折射；CPU 模式使用降采样软件缓存，GPU 模式使用 OpenGL Shader。组件库背景由 Windows DWM 实时合成背后窗口内容。"), &dialog);
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
                          lowPower->isChecked(),
                          static_cast<RenderBackend>(renderBox->currentData().toInt()));
        const QString location = locationEdit->text().trimmed();
        if (!location.isEmpty() && location != m_weatherLocation) {
            m_weatherLocation = location;
            QSettings().setValue(QStringLiteral("weather/location"), m_weatherLocation);
            for (BatteryWidget* widget : s_instances) {
                if (widget->m_kind == CardKind::Weather) {
                    widget->m_weatherLocation = m_weatherLocation;
                    widget->refreshWeather();
                }
            }
        }
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
    auto* text = new QLabel(QStringLiteral("Qt 6 · Liquid Glass desktop widget\n"
                                           "实时读取 Windows 电量与已连接外设。\n"
                                           "玻璃材质由 QtGlassFlow 驱动，背景仅采样当前壁纸。"), &dialog);
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
    m_deviceRefreshTick = (m_deviceRefreshTick + 1) % 6;

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
    m_connectedDevices.clear();
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
        if (!property.connected || m_connectedDevices.size() >= 3 || alreadySeen(key))
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
        m_connectedDevices.append(device);
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
    if (finder) {
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
            m_connectedDevices.append(device);
            seen.insert(key);
        } while (m_connectedDevices.size() < 3
                 && BluetoothFindNextDevice(finder, &info));
        BluetoothFindDeviceClose(finder);
    }

    setToolTip(QStringLiteral("电量 · 拖动移动组件"));
#endif
}

void BatteryWidget::drawBatteryIcon(QPainter& painter, const QRectF& rect, int level, bool charging) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    const qreal radius = rect.height() * 0.21;
    QRectF body = rect.adjusted(1.0, 1.0, -rect.height() * 0.105, -1.0);
    QPainterPath bodyPath;
    bodyPath.addRoundedRect(body, radius, radius);

    QLinearGradient shell(0, body.top(), 0, body.bottom());
    shell.setColorAt(0.0, QColor(255, 255, 255, 220));
    shell.setColorAt(0.45, QColor(221, 227, 236, 165));
    shell.setColorAt(1.0, QColor(157, 166, 180, 160));
    painter.fillPath(bodyPath, shell);

    const qreal inset = qMax<qreal>(3.0, rect.height() * 0.09);
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
            fill.setColorAt(0.0, QColor(129, 241, 185));
            fill.setColorAt(1.0, QColor(30, 194, 133));
        } else {
            fill.setColorAt(0.0, QColor(169, 248, 199));
            fill.setColorAt(1.0, QColor(63, 209, 145));
        }
        painter.fillPath(levelPath, fill);
    }

    QRectF terminal(body.right() + 2, body.center().y() - rect.height() * .16,
                    rect.height() * .105, rect.height() * .32);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(208, 215, 225, 190));
    painter.drawRoundedRect(terminal, terminal.width() * .5, terminal.width() * .5);

    painter.setPen(QPen(QColor(255, 255, 255, 160), 1.0));
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
                                   int level, int iconKind, bool connected) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF ring(center.x() - radius, center.y() - radius,
                      radius * 2.0, radius * 2.0);
    const qreal stroke = qMax<qreal>(9.0, radius * 0.105);

    QPen track(QColor(238, 242, 250, connected ? 38 : 24), stroke,
               Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setBrush(Qt::NoBrush);
    p.setPen(track);
    p.drawEllipse(ring);

    const int clamped = qBound(0, level, 100);
    if (connected && level >= 0 && clamped > 0) {
        const qreal span = -360.0 * clamped / 100.0;
        QPen progress(QColor(239, 244, 255, 222), stroke,
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
    if (connected || iconKind == 0) {
        p.setFont(displayFont(radius > 50.0 ? 25 : 18, QFont::Normal));
        p.setPen(QColor(235, 240, 250, connected ? 182 : 66));
        p.drawText(QRectF(center.x() - radius * .72, center.y() + radius + 27,
                          radius * 1.44, 45), Qt::AlignHCenter | Qt::AlignTop,
                   !connected ? QStringLiteral("—")
                              : (level >= 0 ? QString::number(clamped) + QChar('%')
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
    p.setRenderHint(QPainter::TextAntialiasing, true);

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
        p.drawText(QRectF(batteryContent.left(), batteryContent.top(), 180, 22),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("BATTERY"));
        p.setFont(displayFont(12, QFont::Normal));
        p.setPen(QColor(177, 189, 211, 155));
        p.drawText(QRectF(batteryContent.right() - 170, batteryContent.top(), 170, 22),
                   Qt::AlignRight | Qt::AlignVCenter,
                   m_charging ? QStringLiteral("正在充电") : QStringLiteral("实时同步"));
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
        p.drawText(QRectF(weatherContent.left(), weatherContent.top(), weatherContent.width(), 22),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("WEATHER"));
        p.setFont(displayFont(12, QFont::Normal));
        p.setPen(QColor(177, 189, 211, 155));
        p.drawText(QRectF(weatherContent.left(), weatherContent.top() + 30,
                          weatherContent.width(), 22),
                   Qt::AlignLeft | Qt::AlignVCenter, m_weatherLocation);
        drawWeatherIcon(p, QRectF(weatherContent.left() + 8, weatherContent.top() + 70, 102, 96),
                        m_weatherNight);
        const QString temp = m_weatherTemperature.isEmpty() ? QStringLiteral("--")
                                                               : m_weatherTemperature + QChar(0x00B0);
        p.setFont(displayFont(40, QFont::DemiBold));
        p.setPen(QColor(249, 251, 255, 238));
        p.drawText(QRectF(weatherContent.left() + 120, weatherContent.top() + 74,
                          weatherContent.width() - 120, 54),
                   Qt::AlignLeft | Qt::AlignVCenter, temp);
        p.setFont(displayFont(13, QFont::Normal));
        p.setPen(QColor(207, 216, 234, 195));
        const QString desc = m_weatherDescription.isEmpty()
                                 ? QStringLiteral("获取天气中…") : m_weatherDescription;
        p.drawText(QRectF(weatherContent.left(), weatherContent.top() + 177,
                          weatherContent.width(), 24),
                   Qt::AlignLeft | Qt::AlignVCenter, desc);
        p.setFont(displayFont(11, QFont::Normal));
        p.setPen(QColor(173, 185, 209, 165));
        const QString range = (m_weatherHigh.isEmpty() || m_weatherLow.isEmpty())
                                  ? QStringLiteral("今日  -- / --")
                                  : QStringLiteral("今日  %1° / %2°").arg(m_weatherHigh, m_weatherLow);
        p.drawText(QRectF(weatherContent.left(), weatherContent.top() + 218,
                          weatherContent.width(), 22),
                   Qt::AlignLeft | Qt::AlignVCenter, range);

        const QRectF clock = kDashboardClockRect;
        const QRectF clockContent = clock.adjusted(24, 20, -24, -14);
        p.setFont(displayFont(11, QFont::DemiBold));
        p.setPen(QColor(213, 221, 237, 205));
        p.drawText(QRectF(clockContent.left(), clockContent.top(), 180, 22),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("CLOCK"));
        drawAnalogClock(p, QRectF(clockContent.left() + 4, clockContent.top() + 26,
                                  clockContent.height() - 18, clockContent.height() - 18));
        p.setFont(displayFont(28, QFont::DemiBold));
        p.setPen(QColor(247, 250, 255, 232));
        p.drawText(QRectF(clockContent.left() + 250, clockContent.top() + 77, 155, 42),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QTime::currentTime().toString(QStringLiteral("HH:mm")));
        p.setFont(displayFont(13, QFont::Normal));
        p.setPen(QColor(179, 191, 213, 170));
        p.drawText(QRectF(clockContent.left() + 252, clockContent.top() + 127, 150, 24),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QDate::currentDate().toString(QStringLiteral("yyyy / MM / dd")));

        drawDashboardInfo(p, kDashboardInfoRect);
        p.restore();
        return;
    }

    paintGlassSurfaceFrame(p, card, radius);

    // Independent card windows intentionally paint only their own payload;
    // blur, rounded clipping and wallpaper sampling are provided by the base.
    if (m_kind == CardKind::Battery) {
        // Derive the ring row from the actual card bounds. Fixed coordinates
        // pushed the first ring too close to the left edge after scaling.
        const qreal sideInset = card.width() * .145;
        const qreal spacing = (card.width() - sideInset * 2.0) / 3.0;
        const qreal ringY = card.top() + card.height() * .405;
        const qreal ringRadius = qMin(card.height() * .195, spacing * .39);
        const qreal firstX = card.left() + sideInset;
        drawDeviceRing(p, QPointF(firstX, ringY), ringRadius,
                       m_hasBattery ? m_level : -1, 0, m_hasBattery);
        for (int slot = 0; slot < 3; ++slot) {
            const bool connected = slot < m_connectedDevices.size();
            const ConnectedDevice device = connected ? m_connectedDevices.at(slot)
                                                      : ConnectedDevice{};
            drawDeviceRing(p, QPointF(firstX + spacing * (slot + 1), ringY), ringRadius,
                           device.level, connected ? device.iconKind : -1, connected);
        }
        p.restore();
        return;
    }

    if (m_kind == CardKind::Weather) {
        const QRectF content = card.adjusted(36, 27, -28, -22);
        p.setFont(displayFont(18, QFont::DemiBold));
        p.setPen(QColor(230, 236, 248, 215));
        p.drawText(QRectF(content.left(), content.top(), content.width(), 32),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   m_weatherLocation + QStringLiteral("  ▸"));
        const QString temp = m_weatherTemperature.isEmpty() ? QStringLiteral("--°")
                                                               : m_weatherTemperature + QChar(0x00B0);
        p.setFont(displayFont(58, QFont::Normal));
        p.setPen(QColor(246, 249, 255, 238));
        p.drawText(QRectF(content.left(), content.top() + 39, content.width(), 72),
                   Qt::AlignLeft | Qt::AlignVCenter, temp);
        drawWeatherIcon(p, QRectF(content.left(), content.top() + 130, 58, 52),
                        m_weatherNight);
        p.setFont(displayFont(16, QFont::DemiBold));
        p.setPen(QColor(221, 229, 243, 215));
        p.drawText(QRectF(content.left(), content.top() + 170, content.width(), 26),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   m_weatherDescription.isEmpty() ? QStringLiteral("获取天气中…")
                                                  : m_weatherDescription);
        p.setFont(displayFont(13, QFont::Normal));
        p.setPen(QColor(194, 204, 223, 190));
        const QString range = (m_weatherHigh.isEmpty() || m_weatherLow.isEmpty())
                                  ? QStringLiteral("最高 --°   最低 --°")
                                  : QStringLiteral("最高 %1°   最低 %2°").arg(m_weatherHigh, m_weatherLow);
        p.drawText(QRectF(content.left(), content.top() + 225, content.width(), 24),
                   Qt::AlignLeft | Qt::AlignVCenter, range);
        p.restore();
        return;
    }

    if (m_kind == CardKind::Clock) {
        drawAnalogClock(p, card);
        p.restore();
        return;
    }

    if (m_kind == CardKind::Dictionary) {
        const QRectF content = card.adjusted(32, 26, -30, -24);
        p.setFont(displayFont(34, QFont::DemiBold));
        p.setPen(QColor(244, 247, 254, 232));
        p.drawText(QRectF(content.left(), content.top(), content.width(), 42),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   m_dictionaryWord.isEmpty() ? QStringLiteral("加载中…") : m_dictionaryWord);
        p.setFont(displayFont(18, QFont::Normal));
        p.setPen(QColor(218, 226, 242, 220));
        const QString phonetic = m_dictionaryPhonetic.isEmpty()
                                     ? QStringLiteral("词典数据") : m_dictionaryPhonetic;
        p.drawText(QRectF(content.left(), content.top() + 46, content.width(), 28),
                   Qt::AlignLeft | Qt::AlignVCenter, phonetic);
        p.setFont(displayFont(16, QFont::Normal));
        p.drawText(QRectF(content.left(), content.top() + 82, content.width(), 28),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   m_dictionaryPartOfSpeech.isEmpty() ? QStringLiteral("在线词典")
                                                       : m_dictionaryPartOfSpeech);
        p.drawText(QRectF(content.left(), content.top() + 118, content.width(), 42),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(p.font()).elidedText(m_dictionaryDefinition,
                                                     Qt::ElideRight, int(content.width())));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(198, 210, 238, 105));
        const qreal barWidth = qMin<qreal>(content.width() - 110, 560.0);
        for (int i = 0; i < 20; ++i)
            p.drawRoundedRect(QRectF(content.left() + i * (barWidth / 20.0),
                                     content.top() + 164, barWidth / 20.0 - 4, 20), 4, 4);
        p.setPen(QPen(QColor(215, 226, 250, 180), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(content.left(), card.bottom() - 52, 82, 32), 6, 6);
        p.setFont(displayFont(14, QFont::Normal));
        p.drawText(QRectF(content.left(), card.bottom() - 52, 82, 32),
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

    const QRectF content = card.adjusted(24, 20, -24, -20);

    // Detail card: header, large battery percentage, status and metadata.
    p.setPen(Qt::NoPen);
    p.setBrush(m_charging ? QColor(89, 235, 166) : QColor(232, 238, 247, 170));
    p.drawEllipse(QRectF(content.left(), content.top() + 3, 7, 7));
    p.setFont(displayFont(10, QFont::DemiBold));
    p.setPen(QColor(208, 217, 235, 210));
    p.drawText(QRectF(content.left() + 15, content.top() - 1, 120, 18),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("BATTERY"));
    p.setFont(displayFont(10, QFont::Normal));
    p.setPen(QColor(177, 188, 211, 135));
    p.drawText(QRectF(content.right() - 125, content.top() - 1, 125, 18),
               Qt::AlignRight | Qt::AlignVCenter,
               QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));

    drawBatteryIcon(p, QRectF(content.left(), content.top() + 43, 104, 61),
                    m_hasBattery ? m_level : -1, m_charging);

    p.setFont(displayFont(52, QFont::DemiBold));
    p.setPen(QColor(249, 251, 255));
    p.drawText(QRectF(content.left() + 125, content.top() + 29, 150, 65),
               Qt::AlignLeft | Qt::AlignVCenter,
               m_hasBattery ? QString::number(m_level) : QStringLiteral("—"));
    if (m_hasBattery) {
        p.setFont(displayFont(21, QFont::Normal));
        p.setPen(QColor(196, 205, 222, 210));
        p.drawText(QRectF(content.left() + 210, content.top() + 40, 55, 30),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("%"));
    }

    const QString state = !m_hasBattery ? QStringLiteral("POWER ADAPTER")
                                        : (m_charging ? QStringLiteral("CHARGING") : QStringLiteral("ON BATTERY"));
    p.setFont(displayFont(11, QFont::Medium));
    p.setPen(m_charging ? QColor(117, 239, 183) : QColor(220, 227, 239, 188));
    p.drawText(QRectF(content.left() + 128, content.top() + 90, 155, 22),
               Qt::AlignLeft | Qt::AlignVCenter, state);

    p.setPen(QPen(QColor(255, 255, 255, 25), 1));
    p.drawLine(content.left(), content.bottom() - 27, content.right(), content.bottom() - 27);
    p.setFont(displayFont(10, QFont::Normal));
    p.setPen(QColor(161, 173, 199, 150));
    p.drawText(QRectF(content.left(), content.bottom() - 21, content.width(), 18),
               Qt::AlignLeft | Qt::AlignVCenter,
               m_charging ? QStringLiteral("Power source  ·  Connected")
                          : QStringLiteral("Power source  ·  Internal battery"));
    p.setPen(QColor(161, 173, 199, 122));
    p.drawText(QRectF(content.left(), content.bottom() - 21, content.width(), 18),
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
    if (event->button() == Qt::LeftButton)
        m_dragOrigin = pos();
    LiquidGlassWidget::mousePressEvent(event);
}

void BatteryWidget::mouseReleaseEvent(QMouseEvent* event)
{
    LiquidGlassWidget::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton && !m_restoringPosition) {
        const QRect target = nearestGridRect(m_kind, pos(), m_uiScale, this);
        const QPoint snapped = target.isValid() ? target.topLeft() : m_dragOrigin;
        if (snapped != pos()) {
            m_restoringPosition = true;
            move(snapped);
            m_restoringPosition = false;
        }
        m_hasSavedPosition = true;
        captureDesktopBackdrop();
        saveAllConfigurations();
        hideGridPreview();
    }
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
        showGridPreviewRect(nearestGridRect(m_kind, pos(), m_uiScale, this));
}
