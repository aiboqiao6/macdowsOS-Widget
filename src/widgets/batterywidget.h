#pragma once

#include "rendering/liquidglasswidget.h"
#include "rendering/nativewindows.h"

#include <QPoint>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QDateTime>
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

class BatteryWidget final : public LiquidGlassWidget
{
public:
    enum class CardKind { Battery, Weather, Clock, Dictionary };

    explicit BatteryWidget(QWidget* parent = nullptr,
                           CardKind kind = CardKind::Battery,
                           bool primary = true,
                           const QString& instanceId = QString(),
                           int variant = -1);
    ~BatteryWidget() override;

    QString instanceId() const { return m_instanceId; }
    CardKind cardKind() const { return m_kind; }
    int cardVariant() const { return m_variant; }
    bool placeAtDesktopPoint(const QPoint& point);
    void saveConfiguration() const;
    void showSettingsDialog();

    static QList<BatteryWidget*> restoreWidgets();
    static BatteryWidget* addWidget(CardKind kind, const QPoint& desktopPoint,
                                    int variant = -1);
    // Render the same card payload used on the desktop for the widget gallery.
    // Size is logical; pixels are generated at the target screen's density.
    static QImage renderPreview(CardKind kind, int variant, const QSize& size,
                                qreal devicePixelRatio = 1.0);
    // Registers the bundled PingFang face and returns its Qt family name.
    // Keeping this on the widget core lets the app, tests and preview gallery
    // share exactly the same font registration path.
    static QString pingFangFontFamily();
    // Applies one of the global font rasterization profiles to both Qt
    // controls and custom-painted widget text. Levels are 0=off, 1=standard,
    // 2=clear (default), 3=native-pixel grayscale with full hinting.
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
    struct ConnectedDevice {
        QString name;
        int level = -1;       // -1 means Windows exposes no battery value.
        int iconKind = 4;     // 1 mouse, 2 keyboard, 3 audio, 4 generic.
    };
    struct WeatherHour {
        QString time;
        QString temperature;
        QString description;
        int code = -1;
        bool night = false;
    };
    struct WeatherDay {
        QString label;
        QString high;
        QString low;
        QString description;
        int code = -1;
        bool night = false;
    };

    void refreshBattery();
    void refreshConnectedDevices();
    static QVector<ConnectedDevice> queryConnectedDevices();
    void drawBatteryIcon(QPainter& painter, const QRectF& rect, int level, bool charging) const;
    void drawDeviceRing(QPainter& painter, const QPointF& center, qreal radius,
                         int level, int iconKind, bool connected,
                         bool showCaption = true,
                         qreal strokeMultiplier = 1.0) const;
    void setupTrayIcon();
    void toggleWidgetVisibility();
    void toggleLayout();
    void applyScale(qreal scale);
    void updateTrayVisibilityLabel();
    void updateLayoutLabel();
    void setStartupEnabled(bool enabled);
    bool isStartupEnabled() const;
    void showAboutDialog();
    QIcon createTrayIcon() const;
    void refreshWeather();
    void handleWeatherReply(QNetworkReply* reply);
    void refreshDictionary();
    void handleDictionaryReply(QNetworkReply* reply);
    void searchWeatherCity();
    void updateDashboardObjects();
    void drawAnalogClock(QPainter& painter, const QRectF& rect, int hourOffset = 0) const;
    void drawClockFace(QPainter& painter, const QRectF& rect, int style,
                       const QString& city = QString(), int hourOffset = 0) const;
    void drawDigitalClock(QPainter& painter, const QRectF& rect,
                          const QString& city = QString(), int hourOffset = 0,
                          bool transparentFace = false) const;
    QRectF drawWeatherIcon(QPainter& painter, const QRectF& rect, bool night,
                           int weatherCode = -1) const;
    void drawDashboardInfo(QPainter& painter, const QRectF& card) const;
    void arrangeGroup();
    void syncGroupSettings(qreal scale, int blurStrength,
                           qreal opacity, bool mouseThrough, bool lowPower,
                           bool liveBackdrop, qreal renderQuality, int fontSmoothing);
    void moveToGroupPosition();
    void showWidgetLibrary();
    static QRect nearestGridRect(CardKind kind, int variant,
                                 const QPoint& requestedTopLeft, qreal scale,
                                 const BatteryWidget* ignored = nullptr);
    static QRect nearbyWidgetRect(CardKind kind, int variant,
                                  const QPoint& requestedTopLeft, qreal scale,
                                  const BatteryWidget* ignored = nullptr);
    static bool overlapsVisibleWidget(const QRect& rect,
                                      const BatteryWidget* ignored = nullptr);
    static void showGridPreview(CardKind kind, int variant, const QPoint& desktopPoint);
    static void showGridPreviewRect(const QRect& rect);
    static void hideGridPreview();
    static QString kindName(CardKind kind);
    static CardKind kindFromName(const QString& value);
    static QString configurationPath();
    static void saveAllConfigurations();

    QTimer m_refreshTimer;
    QTimer m_weatherTimer;
    QTimer m_settingsTimer;
    QTimer m_trayClickTimer;
    int m_level = -1;          // Unknown until the first system power poll.
    bool m_charging = false;
    bool m_hasBattery = false;
    // Legacy composite branches stay disabled; every live instance is one
    // independent card selected by m_kind.
    bool m_overview = false;
    bool m_dashboardMode = false;
    CardKind m_kind = CardKind::Battery;
    int m_variant = 0;
    QString m_hostDeviceName;
    bool m_primary = true;
    QString m_instanceId;
    bool m_restoringPosition = false;
    bool m_hasSavedPosition = false;
    QPoint m_dragOrigin;
    NativeWindows::HiddenWindows m_dragHiddenWindows;
    NativeWindows::HiddenWindows m_libraryDragHiddenWindows;
    bool m_clickPending = false;
    qreal m_uiScale = 1.0;
    int m_deviceRefreshTick = 0;
    QVector<ConnectedDevice> m_connectedDevices;
    QFutureWatcher<QVector<ConnectedDevice>> m_deviceWatcher;
    QSystemTrayIcon* m_trayIcon = nullptr;
    QAction* m_visibilityAction = nullptr;
    QAction* m_layoutAction = nullptr;
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
    bool m_weatherLoading = true;
    QDateTime m_weatherUpdated;
    int m_weatherReplySerial = 0;
    QTimer m_dictionaryTimer;
    QString m_dictionaryWord;
    QString m_dictionaryPhonetic;
    QString m_dictionaryPartOfSpeech;
    QString m_dictionaryDefinition;
    int m_dictionaryReplySerial = 0;
    static QList<BatteryWidget*> s_instances;
};
