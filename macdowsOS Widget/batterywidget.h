#pragma once

#include "liquidglasswidget.h"

#include <QPoint>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QDateTime>
#include <QRect>
#include <QRectF>
#include <QList>

class QAction;
class QIcon;
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
                           const QString& instanceId = QString());
    ~BatteryWidget() override;

    QString instanceId() const { return m_instanceId; }
    CardKind cardKind() const { return m_kind; }
    bool placeAtDesktopPoint(const QPoint& point);
    void saveConfiguration() const;

    static QList<BatteryWidget*> restoreWidgets();
    static BatteryWidget* addWidget(CardKind kind, const QPoint& desktopPoint);

protected:
    void paintOverlay(QPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void moveEvent(QMoveEvent* event) override;

private:
    struct ConnectedDevice {
        QString name;
        int level = -1;       // -1 means Windows exposes no battery value.
        int iconKind = 4;     // 1 mouse, 2 keyboard, 3 audio, 4 generic.
    };

    void refreshBattery();
    void refreshConnectedDevices();
    void drawBatteryIcon(QPainter& painter, const QRectF& rect, int level, bool charging) const;
    void drawDeviceRing(QPainter& painter, const QPointF& center, qreal radius,
                        int level, int iconKind, bool connected) const;
    void setupTrayIcon();
    void toggleWidgetVisibility();
    void toggleLayout();
    void showSettingsDialog();
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
    void drawAnalogClock(QPainter& painter, const QRectF& rect) const;
    void drawWeatherIcon(QPainter& painter, const QRectF& rect, bool night) const;
    void drawDashboardInfo(QPainter& painter, const QRectF& card) const;
    void arrangeGroup();
    void syncGroupSettings(qreal scale, bool systemBlur, int blurStrength,
                           qreal opacity, bool mouseThrough, bool lowPower,
                           RenderBackend renderBackend);
    void moveToGroupPosition();
    void showWidgetLibrary();
    QPoint snappedTopLeft(const QPoint& requested) const;
    static QRect nearestGridRect(CardKind kind, const QPoint& requestedTopLeft,
                                 qreal scale, const BatteryWidget* ignored = nullptr);
    static void showGridPreview(CardKind kind, const QPoint& desktopPoint);
    static void showGridPreviewRect(const QRect& rect);
    static void hideGridPreview();
    static QString kindName(CardKind kind);
    static CardKind kindFromName(const QString& value);
    static QString configurationPath();
    static void saveAllConfigurations();

    QTimer m_refreshTimer;
    QTimer m_weatherTimer;
    QTimer m_settingsTimer;
    int m_level = 100;
    bool m_charging = false;
    bool m_hasBattery = true;
    // Legacy composite branches stay disabled; every live instance is one
    // independent card selected by m_kind.
    bool m_overview = false;
    bool m_dashboardMode = false;
    CardKind m_kind = CardKind::Battery;
    bool m_primary = true;
    QString m_instanceId;
    bool m_restoringPosition = false;
    bool m_hasSavedPosition = false;
    QPoint m_dragOrigin;
    qreal m_uiScale = 1.0;
    int m_deviceRefreshTick = 0;
    QVector<ConnectedDevice> m_connectedDevices;
    QSystemTrayIcon* m_trayIcon = nullptr;
    QAction* m_visibilityAction = nullptr;
    QAction* m_layoutAction = nullptr;
    QAction* m_startupAction = nullptr;
    WidgetLibraryDialog* m_library = nullptr;
    QNetworkAccessManager* m_weatherNetwork = nullptr;
    QNetworkAccessManager* m_dictionaryNetwork = nullptr;
    QString m_weatherLocation;
    QString m_weatherTemperature;
    QString m_weatherDescription;
    QString m_weatherHigh;
    QString m_weatherLow;
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
