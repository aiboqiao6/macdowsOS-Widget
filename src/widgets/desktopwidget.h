#pragma once

#include "rendering/liquidglasswidget.h"
#include "rendering/nativewindows.h"
#include "platform/powerdevices.h"

#include <QPoint>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QRect>
#include <QRectF>
#include <QList>
#include <QFutureWatcher>
#include <QPointer>

class QAction;
class QIcon;
class QFont;
class QSystemTrayIcon;
class QNetworkAccessManager;
class QNetworkReply;
class QPainter;
class QMoveEvent;
class QContextMenuEvent;
class WidgetLibraryDialog;

// 独立桌面卡片：本类协调生命周期、放置和持久化，绘制、数据与设置分文件实现。
class DesktopWidget final : public LiquidGlassWidget
{
public:
    // 图库拖动编码使用枚举值，新增类型必须追加，避免改变已有编码。
    enum class CardKind { Battery, Weather, Clock, Dictionary, Music };

    explicit DesktopWidget(QWidget* parent = nullptr,
                           CardKind kind = CardKind::Battery,
                           bool ownsTray = true,
                           const QString& instanceId = QString(),
                           int variant = -1, bool previewOnly = false);
    ~DesktopWidget() override;

    QString instanceId() const { return m_instanceId; }
    CardKind cardKind() const { return m_cardKind; }
    int cardVariant() const { return m_cardVariant; }
    bool placeAtDesktopPoint(const QPoint& point);
    void saveConfiguration() const;
    void showSettingsDialog();

    static QList<DesktopWidget*> restoreWidgets();
    static DesktopWidget* addWidget(CardKind kind, const QPoint& desktopPoint,
                                    int variant = -1);
    // 图库复用桌面内容绘制；尺寸为逻辑像素，图像按目标屏幕密度生成。
    static QImage renderPreview(CardKind kind, int variant, const QSize& size,
                                qreal devicePixelRatio = 1.0);
    // 向导、设置和桌面共用尺寸计算，返回逻辑像素，屏幕 DPR 由 Qt 处理。
    static QSize cardSize(CardKind kind, int variant, qreal scale, const QRect& workArea);
    // 统一注册内置苹方字体，应用、测试与图库共用同一字体族。
    static QString pingFangFontFamily();
    // 统一字体模式：0 关闭，1 标准灰阶，2 清晰灰阶，3 原生像素灰阶，4 ClearType（默认）。
    static void setGlobalFontSmoothing(int level);
    static int globalFontSmoothing();
    static void applyFontSmoothing(QFont& font);
    static void shutdown();

protected:
    void paintOverlay(QPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void windowDragStarted() override;
    void windowDragFinished() override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool wallpaperOnlyWhenIdle() const override { return !liveBackdropEnabled(); }
    bool directBackdropDuringDrag() const override { return true; }

private:
    friend class WidgetRegression;
    using ConnectedDevice = PowerDevices::Device;
    struct WeatherHour {
        QString time;
        QString temperature;
        int code = -1;
        bool night = false;
    };
    struct WeatherDay {
        QString label;
        QString high;
        QString low;
        int code = -1;
        bool night = false;
    };

    void paintBatteryCard(QPainter& painter, const QRectF& card) const;
    void paintWeatherCard(QPainter& painter, const QRectF& card) const;
    void paintClockCard(QPainter& painter, const QRectF& card) const;
    void paintDictionaryCard(QPainter& painter, const QRectF& card) const;
    void refreshBattery();
    void refreshConnectedDevices();
    void drawBatteryIcon(QPainter& painter, const QRectF& rect, int level, bool charging) const;
    void drawDeviceRing(QPainter& painter, const QPointF& center, qreal radius,
                         int level, int iconKind, bool connected,
                         bool showCaption = true,
                         qreal strokeMultiplier = 1.0) const;
    void setupTrayIcon();
    void toggleWidgetVisibility();
    void rearrangeWidgets();
    void applyScale(qreal scale);
    void updateTrayVisibilityLabel();
    void updateRearrangeLabel();
    void setStartupEnabled(bool enabled);
    bool isStartupEnabled() const;
    void showAboutDialog();
    QIcon createTrayIcon() const;
    void refreshWeather();
    void handleWeatherReply(QNetworkReply* reply);
    void refreshDictionary();
    void handleDictionaryReply(QNetworkReply* reply);
    void searchWeatherCity();
    void updateCardGeometry();
    void drawAnalogClock(QPainter& painter, const QRectF& rect, int hourOffset = 0) const;
    void drawClockFace(QPainter& painter, const QRectF& rect, int style,
                       const QString& city = QString(), int hourOffset = 0) const;
    void drawDigitalClock(QPainter& painter, const QRectF& rect,
                          const QString& city = QString(), int hourOffset = 0,
                          bool transparentFace = false) const;
    QRectF drawWeatherIcon(QPainter& painter, const QRectF& rect, bool night,
                           int weatherCode = -1) const;
    void arrangeGroup();
    void syncGroupSettings(qreal scale, bool mouseThrough, bool lowPower,
                           bool liveBackdrop, qreal renderQuality, int fontSmoothing);
    void moveToGroupPosition();
    void showWidgetLibrary();
    static QRect nearestGridRect(CardKind kind, int variant,
                                 const QPoint& requestedTopLeft, qreal scale,
                                 const DesktopWidget* ignored = nullptr);
    static QRect nearbyWidgetRect(CardKind kind, int variant,
                                  const QPoint& requestedTopLeft, qreal scale,
                                  const DesktopWidget* ignored = nullptr);
    static bool overlapsVisibleWidget(const QRect& rect,
                                      const DesktopWidget* ignored = nullptr);
    static void showGridPreview(CardKind kind, int variant, const QPoint& desktopPoint);
    static void showGridPreviewRect(const QRect& rect);
    static void hideGridPreview();
    static QString kindName(CardKind kind);
    static CardKind kindFromName(const QString& value);
    static QString configurationPath();
    static void saveAllConfigurations();

    QTimer m_batteryTimer;
    QTimer m_weatherTimer;
    QTimer m_settingsTimer;
    QTimer m_trayClickTimer;
    int m_batteryLevel = -1;          // 首次系统电量读取前为未知状态。
    bool m_charging = false;
    bool m_hasBattery = false;
    CardKind m_cardKind = CardKind::Battery;
    int m_cardVariant = 0;
    QString m_hostDeviceName;
    bool m_ownsTray = true;
    QString m_instanceId;
    bool m_restoringPosition = false;
    bool m_hasSavedPosition = false;
    QPoint m_dragOrigin;
    NativeWindows::HiddenWindows m_dragHiddenWindows;
    NativeWindows::HiddenWindows m_libraryDragHiddenWindows;
    bool m_clickPending = false;
    qreal m_widgetScale = 1.0;
    int m_devicePollCount = 0;
    QVector<ConnectedDevice> m_connectedDevices;
    QFutureWatcher<QVector<ConnectedDevice>> m_deviceWatcher;
    QSystemTrayIcon* m_trayIcon = nullptr;
    QAction* m_visibilityAction = nullptr;
    QAction* m_rearrangeAction = nullptr;
    QAction* m_startupAction = nullptr;
    WidgetLibraryDialog* m_library = nullptr;
    QNetworkAccessManager* m_weatherNetwork = nullptr;
    QNetworkAccessManager* m_dictionaryNetwork = nullptr;
    QPointer<QNetworkReply> m_weatherReply;
    QPointer<QNetworkReply> m_dictionaryReply;
    QString m_weatherLocation;
    QString m_weatherCityId;
    QString m_weatherTemperature;
    QString m_weatherDescription;
    QString m_weatherHigh;
    QString m_weatherLow;
    QVector<WeatherHour> m_weatherHours;
    QVector<WeatherDay> m_weatherDays;
    int m_weatherCode = -1;
    bool m_weatherNight = false;
    int m_weatherReplySerial = 0;
    QTimer m_dictionaryTimer;
    QString m_dictionaryWord;
    QString m_dictionaryPhonetic;
    QString m_dictionaryPartOfSpeech;
    QString m_dictionaryDefinition;
    int m_dictionaryReplySerial = 0;
    static QList<DesktopWidget*> s_instances;
};
