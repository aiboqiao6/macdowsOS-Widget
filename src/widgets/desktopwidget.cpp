#include "widgets/desktopwidget.h"
#include "widgets/widgetgeometry.h"
#include "widgets/cardmetrics.h"
#include "app/appsettings.h"
#include "app/fontsettings.h"
#include "media/systemmediasession.h"
#include "ui/widgetlibrarydialog.h"
#include <QApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSystemTrayIcon>
#include <QAction>
#include <QScreen>
#include <QSysInfo>
#include <QUuid>
#include <QtMath>

using namespace CardMetrics;
using namespace WidgetGeometry;

QString DesktopWidget::pingFangFontFamily() { return AppFonts::family(); }
int DesktopWidget::globalFontSmoothing() { return AppFonts::smoothing(); }
void DesktopWidget::applyFontSmoothing(QFont& font) { AppFonts::applySmoothing(font); }
void DesktopWidget::setGlobalFontSmoothing(int level)
{
    if (AppFonts::setSmoothing(level))
        WidgetLibraryDialog::refreshFontRendering();
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
    const qreal savedRenderQuality = qBound<qreal>(0.5,
        settings.value(QStringLiteral("performance/renderScale"), 0.60).toDouble(), 1.5);
    setRenderScale(float(savedRenderQuality));
    setMouseThroughEnabled(savedMouseThrough);
    setLowPowerRefreshEnabled(savedLowPowerRefresh);
    setLiveBackdropEnabled(savedLiveBackdrop);
    if (savedFontSmoothing != globalFontSmoothing())
        setGlobalFontSmoothing(savedFontSmoothing);
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
            const auto devices = m_deviceWatcher.result();
            if (devices != m_connectedDevices) {
                m_connectedDevices = devices;
                update();
            }
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

void DesktopWidget::syncGroupSettings(qreal scale, bool mouseThrough, bool lowPower,
                                      bool liveBackdrop, qreal renderQuality, int fontSmoothing)
{
    AppSettings settings;
    settings.setValue(QStringLiteral("performance/renderScale"), renderQuality);
    settings.setValue(QStringLiteral("appearance/liveBackdrop"), liveBackdrop);
    settings.setValue(QStringLiteral("appearance/fontSmoothing"), fontSmoothing);
    settings.sync();
    setGlobalFontSmoothing(fontSmoothing);
    // 尺寸变化尽量保留自由放置位置，仅显式重新排列时布局整个网格。
    for (DesktopWidget* widget : s_instances) {
        if (!qFuzzyCompare(widget->m_widgetScale, scale))
            widget->applyScale(scale);
        widget->setMouseThroughEnabled(mouseThrough);
        widget->setLowPowerRefreshEnabled(lowPower);
        widget->setLiveBackdropEnabled(liveBackdrop);
        widget->setRenderScale(float(renderQuality));
        if (widget->m_library) {
            widget->m_library->setInterfaceScale(scale);
            widget->m_library->setLiveBackdropEnabled(liveBackdrop);
            widget->m_library->setRenderScale(float(renderQuality));
        }
    }
    saveAllConfigurations();
}
