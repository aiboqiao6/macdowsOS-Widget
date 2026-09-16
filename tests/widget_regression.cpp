#include "rendering/desktopcapture.h"
#include "rendering/liquidglasswidget.h"
#include "rendering/nativewindows.h"
#include "rendering/responsivelayout.h"
#include "ui/widgetlibrarydialog.h"
#include "widgets/batterywidget.h"

#include <QApplication>
#include <QDir>
#include <QFrame>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMouseEvent>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QPainter>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QtTest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

class ColorWindow : public QWidget {
public:
    QColor color{210, 35, 25};
    ColorWindow() {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    }
    void paintEvent(QPaintEvent*) override { QPainter(this).fillRect(rect(), color); }
};

class TestGlass : public LiquidGlassWidget {
public:
    QColor overlay;
    TestGlass() {
        setDesktopLayerEnabled(false);
        setFixedSize(200, 140);
        setWindowFlag(Qt::WindowStaysOnTopHint);
        setNoiseAmount(0);
        setRefractionPower(0);
    }
    bool dragging() const { return windowDragging(); }
    bool excluded() const { return captureExclusionAvailable(); }
    void captureMode(bool active) { setSystemCaptureMode(active); }
    void paintOverlay(QPainter& painter) override {
        if (overlay.isValid())
            painter.fillRect(rect().adjusted(30, 30, -30, -30), overlay);
    }
};

static void mouse(QWidget& widget, QEvent::Type type, QPoint local, QPoint global,
                  Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent event(type, QPointF(local), QPointF(global), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

static QColor centerPixel(QOpenGLWidget& widget)
{
    const QImage image = widget.grabFramebuffer();
    return image.isNull() ? QColor() : image.pixelColor(image.rect().center());
}

class FakeReply : public QNetworkReply {
public:
    explicit FakeReply(QByteArray payload, QObject* parent) : QNetworkReply(parent), body(std::move(payload)) {
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        setFinished(true);
    }
    void abort() override {}
    qint64 bytesAvailable() const override { return body.size() - offset + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char* data, qint64 maxSize) override {
        const qint64 count = qMin(maxSize, qint64(body.size()) - offset);
        if (count <= 0) return -1;
        memcpy(data, body.constData() + offset, size_t(count));
        offset += count;
        return count;
    }
private:
    QByteArray body;
    qint64 offset = 0;
};

class WidgetRegression : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("macdowsOS-regression");
        QCoreApplication::setApplicationName("widget-regression");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(!BatteryWidget::pingFangFontFamily().isEmpty());
        // Regression tests never contact the live weather/dictionary services.
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", 9));
        QDir().mkpath("artifacts");
    }

    void responsiveLayoutUsesFontMetricsAndDeviceScale() {
        QImage image(QSize(480, 220), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        QFont font(BatteryWidget::pingFangFontFamily());
        font.setPointSize(72);
        painter.setFont(font);
        painter.setPen(Qt::white);

        // Deliberately make the alignment box much shorter than the font's
        // line height. The shared point/baseline renderer must preserve the
        // complete glyph instead of clipping it to this rectangle.
        const QRectF shortBox(30, 95, 400, 18);
        ResponsiveLayout::drawSingleLine(
            painter, shortBox, Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("19° 时间"));
        painter.end();

        QRect visible;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (qAlpha(image.pixel(x, y)) == 0)
                    continue;
                visible = visible.isNull() ? QRect(x, y, 1, 1)
                                           : visible.united(QRect(x, y, 1, 1));
            }
        }
        QVERIFY(!visible.isNull());
        QVERIFY(visible.height() > shortBox.height());

        QImage narrowImage(QSize(180, 140), QImage::Format_ARGB32_Premultiplied);
        narrowImage.fill(Qt::transparent);
        QPainter narrowPainter(&narrowImage);
        narrowPainter.setFont(font);
        narrowPainter.setPen(Qt::white);
        const QRectF narrowBox(20, 40, 90, 50);
        ResponsiveLayout::drawSingleLine(
            narrowPainter, narrowBox, Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("04:13"));
        narrowPainter.end();
        QRect narrowVisible;
        for (int y = 0; y < narrowImage.height(); ++y) {
            for (int x = 0; x < narrowImage.width(); ++x) {
                if (qAlpha(narrowImage.pixel(x, y)) == 0)
                    continue;
                narrowVisible = narrowVisible.isNull()
                    ? QRect(x, y, 1, 1)
                    : narrowVisible.united(QRect(x, y, 1, 1));
            }
        }
        QVERIFY(!narrowVisible.isNull());
        QVERIFY(narrowVisible.left() >= qFloor(narrowBox.left()) - 1);
        QVERIFY(narrowVisible.right() <= qCeil(narrowBox.right()) + 1);

        const ResponsiveLayout::Metrics smallMetrics(QRectF(0, 0, 120, 80));
        const ResponsiveLayout::Metrics largeMetrics(QRectF(0, 0, 360, 240));
        QCOMPARE(largeMetrics.size(.1), smallMetrics.size(.1) * 3.0);

        QImage strokeCanvas(QSize(100, 100), QImage::Format_ARGB32_Premultiplied);
        QPainter strokePainter(&strokeCanvas);
        const qreal normalHairline = smallMetrics.stroke(strokePainter, 0.0);
        strokePainter.scale(2.0, 2.0);
        const qreal scaledHairline = smallMetrics.stroke(strokePainter, 0.0);
        QCOMPARE(normalHairline, 1.0);
        QCOMPARE(scaledHairline * 2.0, 1.0);
    }

    void everyWeatherAssetIsPureWhiteAndHasVisibleContent() {
        QDir weather(QStringLiteral(":/weather"));
        const QStringList files = weather.entryList({QStringLiteral("*.png")},
                                                    QDir::Files, QDir::Name);
        QCOMPARE(files.size(), 47);
        for (const QString& file : files) {
            const QImage image(weather.filePath(file));
            QVERIFY2(!image.isNull(), qPrintable(file));
            bool hasVisiblePixel = false;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const QColor pixel = image.pixelColor(x, y);
                    if (pixel.alpha() == 0)
                        continue;
                    hasVisiblePixel = true;
                    QVERIFY2(pixel.red() == 255 && pixel.green() == 255
                                 && pixel.blue() == 255,
                             qPrintable(file));
                }
            }
            QVERIFY2(hasVisiblePixel, qPrintable(file));
        }
    }

    void batteryVariantsKeepGridShapeAndRender() {
        QSettings().setValue("appearance/scale", 0.4);
        QSettings().setValue("performance/renderBackend", 2);

        BatteryWidget compact(nullptr, BatteryWidget::CardKind::Battery, false,
                              QStringLiteral("battery-1x1"), 0);
        BatteryWidget wide(nullptr, BatteryWidget::CardKind::Battery, false,
                           QStringLiteral("battery-1x2"), 1);
        BatteryWidget list(nullptr, BatteryWidget::CardKind::Battery, false,
                           QStringLiteral("battery-2x2"), 2);
        QCOMPARE(compact.width(), compact.height());
        QCOMPARE(wide.height(), compact.height());
        QCOMPARE(list.width(), wide.width());
        QCOMPARE(list.width(), list.height());

        for (BatteryWidget* widget : {&compact, &wide, &list}) {
            widget->setDesktopLayerEnabled(false);
            widget->setDesktopCaptureEnabled(false);
            widget->setAnimationEnabled(false);
            QImage background(widget->size(), QImage::Format_RGB32);
            background.fill(QColor(53, 67, 84));
            widget->setBackgroundImage(background);
            widget->m_hasBattery = true;
            widget->m_level = 95;
            widget->m_connectedDevices = {
                {QStringLiteral("Mouse @LOFREE"), 39, 1}
            };
            widget->show();
            QVERIFY(QTest::qWaitForWindowExposed(widget));
            widget->update();
            QTest::qWait(40);
        }

        const QImage compactImage = compact.grabFramebuffer();
        const QImage wideImage = wide.grabFramebuffer();
        const QImage listImage = list.grabFramebuffer();
        QVERIFY(!compactImage.isNull());
        QVERIFY(!wideImage.isNull());
        QVERIFY(!listImage.isNull());
        QVERIFY(compactImage.save("artifacts/battery-1x1.png"));
        QVERIFY(wideImage.save("artifacts/battery-1x2.png"));
        QVERIFY(listImage.save("artifacts/battery-2x2.png"));
    }

    void weatherVariantsUseReferenceShapesAndChinaWeatherPayload() {
        QSettings().setValue("appearance/scale", 0.4);
        QSettings().setValue("performance/renderBackend", 2);

        BatteryWidget compact(nullptr, BatteryWidget::CardKind::Weather, false,
                              QStringLiteral("weather-1x1"), 0);
        BatteryWidget hourly(nullptr, BatteryWidget::CardKind::Weather, false,
                             QStringLiteral("weather-1x2"), 1);
        BatteryWidget forecast(nullptr, BatteryWidget::CardKind::Weather, false,
                               QStringLiteral("weather-2x2"), 2);
        QCOMPARE(compact.width(), compact.height());
        QCOMPARE(hourly.height(), compact.height());
        QCOMPARE(forecast.width(), hourly.width());
        QCOMPARE(forecast.width(), forecast.height());

        const QByteArray payload =
            "var cityDZ ={\"weatherinfo\":{\"city\":\"天心\",\"weather\":\"多云\","
            "\"weathercode\":\"d01\",\"weathercoden\":\"n01\"}};"
            "var dataSK ={\"cityname\":\"天心\",\"temp\":\"30.1\","
            "\"weather\":\"多云\",\"weathercode\":\"d01\"};"
            "var fc ={\"f\":[{\"fa\":\"01\",\"fc\":\"32\",\"fd\":\"21\","
            "\"fi\":\"9/15\",\"fj\":\"今天\"},{\"fa\":\"07\",\"fc\":\"31\","
            "\"fd\":\"23\",\"fi\":\"9/16\",\"fj\":\"星期三\"}]};";
        auto* reply = new FakeReply(payload, &forecast);
        reply->setProperty("weatherSerial", forecast.m_weatherReplySerial);
        forecast.handleWeatherReply(reply);
        QCOMPARE(forecast.m_weatherLocation, QStringLiteral("天心"));
        QCOMPARE(forecast.m_weatherTemperature, QStringLiteral("30.1"));
        QCOMPARE(forecast.m_weatherDescription, QStringLiteral("多云"));
        QCOMPARE(forecast.m_weatherHigh, QStringLiteral("32"));
        QCOMPARE(forecast.m_weatherLow, QStringLiteral("21"));
        QCOMPARE(forecast.m_weatherDays.size(), 2);
        QCOMPARE(forecast.m_weatherDays.at(1).code, 7);
        QCOMPARE(forecast.m_weatherDays.at(1).label, QStringLiteral("周三"));

        const QVector<BatteryWidget::WeatherHour> hours = {
            {QStringLiteral("11时"), QStringLiteral("30"), QStringLiteral("多云"), 1},
            {QStringLiteral("12时"), QStringLiteral("30"), QStringLiteral("多云"), 1},
            {QStringLiteral("13时"), QStringLiteral("31"), QStringLiteral("多云"), 1},
            {QStringLiteral("14时"), QStringLiteral("31"), QStringLiteral("多云"), 1},
            {QStringLiteral("15时"), QStringLiteral("31"), QStringLiteral("多云"), 1},
            {QStringLiteral("16时"), QStringLiteral("31"), QStringLiteral("多云"), 1}
        };
        const QVector<BatteryWidget::WeatherDay> days = {
            {QStringLiteral("今天"), QStringLiteral("32"), QStringLiteral("21"), QStringLiteral("多云"), 1},
            {QStringLiteral("周三"), QStringLiteral("31"), QStringLiteral("23"), QStringLiteral("小雨"), 7},
            {QStringLiteral("周四"), QStringLiteral("29"), QStringLiteral("20"), QStringLiteral("小雨"), 7},
            {QStringLiteral("周五"), QStringLiteral("28"), QStringLiteral("21"), QStringLiteral("多云"), 1},
            {QStringLiteral("周六"), QStringLiteral("30"), QStringLiteral("22"), QStringLiteral("晴"), 0},
            {QStringLiteral("周日"), QStringLiteral("29"), QStringLiteral("21"), QStringLiteral("多云"), 1}
        };
        for (BatteryWidget* widget : {&compact, &hourly, &forecast}) {
            widget->setDesktopLayerEnabled(false);
            widget->setDesktopCaptureEnabled(false);
            widget->setAnimationEnabled(false);
            widget->m_weatherLocation = QStringLiteral("天心区");
            widget->m_weatherTemperature = QStringLiteral("29");
            widget->m_weatherDescription = QStringLiteral("多云");
            widget->m_weatherHigh = QStringLiteral("31");
            widget->m_weatherLow = QStringLiteral("24");
            widget->m_weatherHours = hours;
            widget->m_weatherDays = days;
            widget->m_weatherCode = 1;
            QImage background(widget->size(), QImage::Format_RGB32);
            background.fill(QColor(37, 59, 80));
            widget->setBackgroundImage(background);
            widget->show();
            QVERIFY(QTest::qWaitForWindowExposed(widget));
            widget->update();
            QTest::qWait(40);
        }
        QVERIFY(compact.grabFramebuffer().save("artifacts/weather-1x1.png"));
        QVERIFY(hourly.grabFramebuffer().save("artifacts/weather-1x2.png"));
        QVERIFY(forecast.grabFramebuffer().save("artifacts/weather-2x2.png"));

        struct WeatherState {
            const char* name;
            QString location;
            QString temperature;
            QString description;
            QString high;
            QString low;
            int code;
            bool night;
            bool hasForecast;
        };
        const QVector<WeatherState> states = {
            {"clear-night", QStringLiteral("长沙"), QStringLiteral("26.7"),
             QStringLiteral("晴"), QStringLiteral("33"), QStringLiteral("23"),
             0, true, true},
            {"negative-snow", QStringLiteral("呼伦贝尔市海拉尔区"), QStringLiteral("-12.8"),
             QStringLiteral("小雪"), QStringLiteral("-8.5"), QStringLiteral("-19.2"),
             14, false, true},
            {"long-condition", QStringLiteral("乌鲁木齐市天山区"), QStringLiteral("38.6"),
             QStringLiteral("雷阵雨伴有冰雹"), QStringLiteral("41.2"), QStringLiteral("26.4"),
             5, false, true},
            {"missing-data", QStringLiteral("定位中"), QString(), QString(),
             QString(), QString(), -1, false, false}
        };
        const auto renderState = [&](BatteryWidget& widget,
                                     const WeatherState& state,
                                     const char* variant) {
            widget.m_weatherLocation = state.location;
            widget.m_weatherTemperature = state.temperature;
            widget.m_weatherDescription = state.description;
            widget.m_weatherHigh = state.high;
            widget.m_weatherLow = state.low;
            widget.m_weatherCode = state.code;
            widget.m_weatherNight = state.night;
            widget.m_weatherHours = state.hasForecast ? hours
                                                       : QVector<BatteryWidget::WeatherHour>{
                                                           {QString(), QString(), QString(), -1},
                                                           {QStringLiteral("稍后"), QString(), QString(), -1}
                                                       };
            widget.m_weatherDays = state.hasForecast ? days
                                                      : QVector<BatteryWidget::WeatherDay>{
                                                           {QStringLiteral("今天"), QString(), QString(), QString(), -1},
                                                           {QStringLiteral("周三"), QString(), QString(), QString(), -1}
                                                       };
            widget.update();
            QTest::qWait(25);
            const QString path = QStringLiteral("artifacts/weather-state-%1-%2.png")
                                     .arg(QString::fromLatin1(state.name),
                                          QString::fromLatin1(variant));
            const QImage rendered = widget.grabFramebuffer();
            QVERIFY2(!rendered.isNull(), qPrintable(path));
            QVERIFY2(rendered.save(path), qPrintable(path));
        };
        for (const WeatherState& state : states) {
            renderState(compact, state, "1x1");
            renderState(hourly, state, "1x2");
            renderState(forecast, state, "2x2");
        }
    }

    void clockAndDictionaryUseResponsiveTextLayout() {
        QSettings().setValue("appearance/scale", 0.4);
        QSettings().setValue("performance/renderBackend", 2);

        BatteryWidget analog(nullptr, BatteryWidget::CardKind::Clock, false,
                             QStringLiteral("clock-responsive-analog"), 0);
        BatteryWidget digital(nullptr, BatteryWidget::CardKind::Clock, false,
                              QStringLiteral("clock-responsive-digital"), 2);
        BatteryWidget dark(nullptr, BatteryWidget::CardKind::Clock, false,
                           QStringLiteral("clock-responsive-dark"), 1);
        BatteryWidget city(nullptr, BatteryWidget::CardKind::Clock, false,
                           QStringLiteral("clock-responsive-city"), 4);
        BatteryWidget wideDigital(nullptr, BatteryWidget::CardKind::Clock, false,
                                  QStringLiteral("clock-responsive-wide"), 8);
        QVERIFY2(wideDigital.width() > wideDigital.height(),
                 "The 2x1 digital clock must be horizontal");
        BatteryWidget dictionary(nullptr, BatteryWidget::CardKind::Dictionary, false,
                                 QStringLiteral("dictionary-responsive-wide"), 1);
        dictionary.m_dictionaryWord = QStringLiteral("serendipity");
        dictionary.m_dictionaryPhonetic = QStringLiteral("/ˌserənˈdipədē/");
        dictionary.m_dictionaryPartOfSpeech = QStringLiteral("noun");
        dictionary.m_dictionaryDefinition = QStringLiteral("the chance occurrence of a happy discovery");

        for (BatteryWidget* widget : {&analog, &digital, &dark, &city,
                                      &wideDigital, &dictionary}) {
            widget->setDesktopLayerEnabled(false);
            widget->setDesktopCaptureEnabled(false);
            widget->setAnimationEnabled(false);
            QImage background(widget->size(), QImage::Format_RGB32);
            background.fill(QColor(37, 59, 80));
            widget->setBackgroundImage(background);
            widget->show();
            QVERIFY(QTest::qWaitForWindowExposed(widget));
            widget->update();
            QTest::qWait(40);
        }

        QVERIFY(analog.grabFramebuffer().save("artifacts/clock-analog.png"));
        QVERIFY(digital.grabFramebuffer().save("artifacts/clock-digital.png"));
        QVERIFY(dark.grabFramebuffer().save("artifacts/clock-dark.png"));
        QVERIFY(city.grabFramebuffer().save("artifacts/clock-city.png"));
        QVERIFY(wideDigital.grabFramebuffer().save("artifacts/clock-wide.png"));
        QVERIFY(dictionary.grabFramebuffer().save("artifacts/dictionary-wide.png"));
    }

    void hiddenCardDoesNotBlockGridDrop() {
        QSettings().setValue("appearance/scale", 0.4);
        QScreen* screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QRect area = screen->availableGeometry();
        const qreal scale = 0.4;
        const QRect first = BatteryWidget::nearestGridRect(
            BatteryWidget::CardKind::Battery, 0, area.topLeft(), scale);
        QVERIFY(first.isValid());

        BatteryWidget hidden(nullptr, BatteryWidget::CardKind::Battery, false,
                             QStringLiteral("hidden-grid-card"), 0);
        hidden.setGeometry(first);
        hidden.hide();
        const QRect target = BatteryWidget::nearestGridRect(
            BatteryWidget::CardKind::Battery, 0, first.topLeft(), scale);
        QCOMPARE(target, first);
    }

    void draggingCardHidesApplicationWindowsButKeepsWidgets() {
        QSettings().setValue("appearance/scale", 0.4);
        QSettings().setValue("performance/renderBackend", 2);

        BatteryWidget dragged(nullptr, BatteryWidget::CardKind::Weather, false,
                              QStringLiteral("drag-isolation-active"), 0);
        BatteryWidget visiblePeer(nullptr, BatteryWidget::CardKind::Clock, false,
                                  QStringLiteral("drag-isolation-visible"), 0);
        BatteryWidget hiddenPeer(nullptr, BatteryWidget::CardKind::Dictionary, false,
                                 QStringLiteral("drag-isolation-hidden"), 0);
        const QPoint origin = QGuiApplication::primaryScreen()
                                  ->availableGeometry().topLeft() + QPoint(80, 80);
        dragged.move(origin);
        visiblePeer.move(origin + QPoint(dragged.width() + 40, 0));
        QWidget applicationWindow;
        applicationWindow.setWindowTitle(QStringLiteral("drag isolation application"));
        applicationWindow.setGeometry(
            QRect(origin + QPoint(40, 160), QSize(260, 150)));
        dragged.show();
        visiblePeer.show();
        hiddenPeer.hide();
        applicationWindow.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dragged));
        QVERIFY(QTest::qWaitForWindowExposed(&visiblePeer));
        QVERIFY(QTest::qWaitForWindowExposed(&applicationWindow));

        mouse(dragged, QEvent::MouseButtonPress, {30, 30},
              origin + QPoint(30, 30), Qt::LeftButton, Qt::LeftButton);
        mouse(dragged, QEvent::MouseMove, {70, 30},
              origin + QPoint(70, 30), Qt::NoButton, Qt::LeftButton);
        QVERIFY(dragged.isVisible());
        QVERIFY(visiblePeer.isVisible());
        QVERIFY(!hiddenPeer.isVisible());
#ifdef Q_OS_WIN
        QTRY_VERIFY_WITH_TIMEOUT(
            !IsWindowVisible(reinterpret_cast<HWND>(applicationWindow.winId())),
            1000);
        QVERIFY(!IsIconic(reinterpret_cast<HWND>(applicationWindow.winId())));
#endif

        mouse(dragged, QEvent::MouseButtonRelease, {70, 30},
              dragged.pos() + QPoint(70, 30), Qt::LeftButton, Qt::NoButton);
        QVERIFY(visiblePeer.isVisible());
        QVERIFY(!hiddenPeer.isVisible());
#ifdef Q_OS_WIN
        QTRY_VERIFY_WITH_TIMEOUT(
            IsWindowVisible(reinterpret_cast<HWND>(applicationWindow.winId())),
            1000);
#endif

        // Cancellation and focus-loss paths use the same restoration hook.
        const QPoint secondOrigin = dragged.pos();
        mouse(dragged, QEvent::MouseButtonPress, {30, 30},
              secondOrigin + QPoint(30, 30), Qt::LeftButton, Qt::LeftButton);
        mouse(dragged, QEvent::MouseMove, {70, 30},
              secondOrigin + QPoint(70, 30), Qt::NoButton, Qt::LeftButton);
        QVERIFY(visiblePeer.isVisible());
#ifdef Q_OS_WIN
        QTRY_VERIFY_WITH_TIMEOUT(
            !IsWindowVisible(reinterpret_cast<HWND>(applicationWindow.winId())),
            1000);
        QVERIFY(!IsIconic(reinterpret_cast<HWND>(applicationWindow.winId())));
#endif
        QEvent ungrab(QEvent::UngrabMouse);
        QApplication::sendEvent(&dragged, &ungrab);
        QVERIFY(visiblePeer.isVisible());
        QVERIFY(!hiddenPeer.isVisible());
#ifdef Q_OS_WIN
        QTRY_VERIFY_WITH_TIMEOUT(
            IsWindowVisible(reinterpret_cast<HWND>(applicationWindow.winId())),
            1000);
#endif
    }

    void liveBackdropAndStableCapture() {
#ifndef Q_OS_WIN
        QSKIP("Requires Windows compositor");
#endif
        const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
        ColorWindow background;
        background.setGeometry(QRect(area.center() - QPoint(320, 230), QSize(640, 460)));
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));
        TestGlass glass;
        glass.move(background.pos() + QPoint(180, 120));
        glass.show();
        glass.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        QVERIFY(glass.excluded());
        QTRY_VERIFY_WITH_TIMEOUT(centerPixel(glass).red() > 170, 6000);
        // A changing application behind the glass must update without moving
        // the widget or changing the wallpaper.
        background.color = QColor(20, 60, 215);
        background.update();
        QElapsedTimer latency;
        latency.start();
        QTRY_VERIFY_WITH_TIMEOUT(centerPixel(glass).blue() > 170, 3500);
        qInfo() << "Live backdrop change observed in" << latency.elapsed() << "ms";
        glass.grabFramebuffer().save("artifacts/live-blue.png");
        for (int frame = 0; frame < 12; ++frame) {
            QTest::qWait(35);
            const QColor color = centerPixel(glass);
            QVERIFY2(color.blue() > 170 && color.red() < 70, "self-feedback or wallpaper flash");
#ifdef Q_OS_WIN
            DWORD affinity = 0;
            QVERIFY(GetWindowDisplayAffinity(reinterpret_cast<HWND>(glass.winId()), &affinity));
            QCOMPARE(affinity, DWORD(0x0));
#endif
        }
        const QPoint original = glass.pos();
        mouse(glass, QEvent::MouseButtonPress, {50, 50}, original + QPoint(50, 50), Qt::LeftButton, Qt::LeftButton);
        QVERIFY(!glass.dragging());
        for (int i = 1; i <= 20; ++i) {
            const QPoint global = original + QPoint(50 + i * 3, 50);
            mouse(glass, QEvent::MouseMove, {50, 50}, global, Qt::NoButton, Qt::LeftButton);
            QTest::qWait(16);
            QVERIFY(centerPixel(glass).blue() > 170);
        }
        QVERIFY(glass.dragging());
        mouse(glass, QEvent::MouseButtonRelease, {50, 50}, glass.pos() + QPoint(50, 50), Qt::LeftButton, Qt::NoButton);
        QVERIFY(!glass.dragging());
        // A hidden scene must not keep a timer repainting it.
        glass.hide();
        QTest::qWait(100);
        const quint64 revision = glass.frameRevision();
        QTest::qWait(150);
        QCOMPARE(glass.frameRevision(), revision);
    }

    void systemScreenshotModeIncludesGlassWithoutFeedback() {
#ifndef Q_OS_WIN
        QSKIP("Requires Windows display affinity and screen capture");
#endif
        const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
        ColorWindow background;
        background.setGeometry(QRect(area.center() - QPoint(320, 230), QSize(640, 460)));
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));

        TestGlass glass;
        glass.overlay = QColor(25, 220, 50);
        glass.move(background.pos() + QPoint(180, 120));
        glass.show();
        glass.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        QTRY_VERIFY_WITH_TIMEOUT(centerPixel(glass).green() > 160, 6000);

        glass.captureMode(true);
        QTest::qWait(50);
        DWORD affinity = 0;
        QVERIFY(GetWindowDisplayAffinity(reinterpret_cast<HWND>(glass.winId()), &affinity));
        QCOMPARE(affinity, DWORD(0x0));
        const QImage screenshot = glass.screen()->grabWindow(
            0, glass.x(), glass.y(), glass.width(), glass.height()).toImage();
        QVERIFY(!screenshot.isNull());
        QVERIFY2(screenshot.pixelColor(screenshot.rect().center()).green() > 150,
                 "system screenshot omitted the glass card");
        QVERIFY(screenshot.save("artifacts/system-screenshot-visible.png"));

        // The live sampler is frozen while affinity is off, so a changing
        // lower window cannot feed this card back into its own material.
        background.color = QColor(20, 60, 215);
        background.update();
        QTest::qWait(300);
        QVERIFY(centerPixel(glass).green() > 160);

        glass.captureMode(false);
        QVERIFY(GetWindowDisplayAffinity(reinterpret_cast<HWND>(glass.winId()), &affinity));
        QCOMPARE(affinity, DWORD(0x0));
    }

    void lowerWidgetIncludedWithoutFeedback() {
        ColorWindow background;
        background.color = QColor(20, 30, 180);
        background.setGeometry(QRect(QGuiApplication::primaryScreen()->availableGeometry().center()
                                     - QPoint(300, 220), QSize(600, 440)));
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));
        TestGlass lower;
        lower.overlay = QColor(25, 220, 50);
        lower.move(background.pos() + QPoint(150, 100));
        lower.show();
        lower.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&lower));
        TestGlass upper;
        upper.move(lower.pos());
        upper.show();
        upper.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&upper));
        QTRY_VERIFY_WITH_TIMEOUT(centerPixel(upper).green() > 160, 3000);
        upper.grabFramebuffer().save("artifacts/lower-widget.png");
        for (int i = 0; i < 8; ++i) {
            QTest::qWait(50);
            QVERIFY(centerPixel(upper).green() > 160);
        }
        lower.hide();
        QTRY_VERIFY_WITH_TIMEOUT(centerPixel(upper).blue() > 140, 1500);
    }

    void dragThresholdAndCancellation() {
        LiquidGlassWidget glass;
        glass.setDesktopCaptureEnabled(false);
        glass.move(QGuiApplication::primaryScreen()->availableGeometry().topLeft() + QPoint(60, 60));
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        const QPoint original = glass.pos();
        mouse(glass, QEvent::MouseButtonPress, {40, 40}, original + QPoint(40, 40), Qt::LeftButton, Qt::LeftButton);
        mouse(glass, QEvent::MouseMove, {41, 40}, original + QPoint(41, 40), Qt::NoButton, Qt::LeftButton);
        QCOMPARE(glass.pos(), original);
        mouse(glass, QEvent::MouseMove, {70, 40}, original + QPoint(70, 40), Qt::NoButton, Qt::LeftButton);
#ifdef Q_OS_WIN
        QVERIFY(GetWindowLongPtrW(reinterpret_cast<HWND>(glass.winId()), GWL_EXSTYLE) & WS_EX_TOPMOST);
#endif
        QEvent ungrab(QEvent::UngrabMouse);
        QApplication::sendEvent(&glass, &ungrab);
#ifdef Q_OS_WIN
        QVERIFY(!(GetWindowLongPtrW(reinterpret_cast<HWND>(glass.winId()), GWL_EXSTYLE) & WS_EX_TOPMOST));
#endif
    }

    void cloakedWindowDoesNotEraseLowerCard() {
#ifdef Q_OS_WIN
        ColorWindow background;
        background.color = QColor(20, 30, 180);
        background.setGeometry(QRect(QGuiApplication::primaryScreen()->availableGeometry().center()
                                     - QPoint(300, 220), QSize(600, 440)));
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));
        TestGlass lower;
        lower.overlay = QColor(25, 220, 50);
        lower.move(background.pos() + QPoint(150, 100));
        lower.show();
        lower.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&lower));
        ColorWindow cloaked;
        cloaked.setGeometry(lower.geometry().adjusted(-20, -20, 20, 20));
        cloaked.show();
        QVERIFY(QTest::qWaitForWindowExposed(&cloaked));
        using SetAttribute = HRESULT (WINAPI *)(HWND, DWORD, const void*, DWORD);
        const auto setAttribute = reinterpret_cast<SetAttribute>(
            GetProcAddress(GetModuleHandleW(L"dwmapi.dll"), "DwmSetWindowAttribute"));
        QVERIFY(setAttribute);
        const BOOL cloak = TRUE;
        QVERIFY(SUCCEEDED(setAttribute(reinterpret_cast<HWND>(cloaked.winId()), 13, &cloak, sizeof(cloak))));
        TestGlass upper;
        upper.move(lower.pos());
        upper.show();
        upper.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&upper));
        QTRY_VERIFY_WITH_TIMEOUT(centerPixel(upper).green() > 160, 1000);
#endif
    }

    void samplingStaticCardMustNotRepaintIt() {
        TestGlass lower;
        lower.setDesktopCaptureEnabled(false);
        lower.overlay = QColor(25, 220, 50);
        lower.move(QGuiApplication::primaryScreen()->availableGeometry().center());
        lower.show();
        lower.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&lower));
        QTest::qWait(100);
        const auto revision = lower.frameRevision();
        for (int i = 0; i < 5; ++i) {
            const auto image = LiquidGlassWidget::captureDesktopComposite(lower.screen(), lower.geometry());
            QVERIFY(!image.isNull());
            QVERIFY(image.pixelColor(image.rect().center()).green() > 160);
        }
        QCOMPARE(lower.frameRevision(), revision);
        const quint64 reads = lower.snapshotReadbacks();
        QElapsedTimer cached;
        cached.start();
        for (int i = 0; i < 2000; ++i)
            QVERIFY(!lower.surfaceSnapshot().isNull());
        QCOMPARE(lower.snapshotReadbacks(), reads);
        qInfo() << "2000 cached snapshots:" << cached.nsecsElapsed() / 1000 << "us; additional GPU readbacks: 0";
    }

    void everyCardPairComposites_data() {
        QTest::addColumn<int>("lowerKind");
        QTest::addColumn<int>("upperKind");
        const char* names[] = {"battery", "weather", "clock", "dictionary"};
        for (int lower = 0; lower < 4; ++lower)
            for (int upper = 0; upper < 4; ++upper)
                QTest::newRow(qPrintable(QString("%1-behind-%2").arg(names[lower], names[upper]))) << lower << upper;
    }

    void everyCardPairComposites() {
        QFETCH(int, lowerKind);
        QFETCH(int, upperKind);
        QSettings().setValue("appearance/scale", 0.4);
        QSettings().setValue("appearance/opacity", 1.0);
        ColorWindow background;
        background.color = QColor(14, 22, 32);
        background.setGeometry(QRect(QGuiApplication::primaryScreen()->availableGeometry().center()
                                     - QPoint(250, 170), QSize(500, 340)));
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));
        BatteryWidget lower(nullptr, static_cast<BatteryWidget::CardKind>(lowerKind), false);
        BatteryWidget upper(nullptr, static_cast<BatteryWidget::CardKind>(upperKind), false);
        for (BatteryWidget* widget : {&lower, &upper}) {
            widget->setDesktopLayerEnabled(false);
            widget->setDesktopCaptureEnabled(false);
            widget->setWindowFlag(Qt::WindowStaysOnTopHint);
            widget->setAnimationEnabled(false);
            widget->move(background.pos() + QPoint(80, 70));
            QImage source(200, 140, QImage::Format_RGB32);
            source.fill(QColor(42, 60, 90));
            widget->setBackgroundImage(source);
            widget->show();
            widget->raise();
            QVERIFY(QTest::qWaitForWindowExposed(widget));
        }
        QTest::qWait(70);
        const QImage snapshot = lower.surfaceSnapshot();
        QVERIFY(!snapshot.isNull());
        // Verify actual painted battery/weather/clock/dictionary content,
        // including cards that have not repainted since their first frame.
        for (int frame = 0; frame < 6; ++frame) {
            const QImage actual = LiquidGlassWidget::captureDesktopComposite(lower.screen(), lower.geometry(), &upper);
            QVERIFY(!actual.isNull());
            QImage expected(actual.size(), QImage::Format_RGB32);
            expected.fill(background.color);
            QPainter painter(&expected);
            painter.drawImage(expected.rect(), lower.surfaceSnapshot());
            painter.end();
            qint64 error = 0;
            int samples = 0;
            for (int y = 8; y < actual.height() - 8; y += 4) {
                for (int x = 8; x < actual.width() - 8; x += 4) {
                    const QColor a = actual.pixelColor(x, y), b = expected.pixelColor(x, y);
                    error += qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue());
                    ++samples;
                }
            }
            QVERIFY2(error / qreal(qMax(1, samples) * 3) < 3.0,
                     qPrintable(QString("Missing/distorted lower card: mean channel error %1").arg(error / qreal(samples * 3))));
            QTest::qWait(10);
        }
    }

    void movingLowerCardUpdatesWithoutDesktopCapture() {
        ColorWindow background;
        background.color = QColor(20, 30, 180);
        background.setGeometry(QRect(QGuiApplication::primaryScreen()->availableGeometry().center()
                                     - QPoint(300, 220), QSize(600, 440)));
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));
        TestGlass lower;
        lower.overlay = QColor(25, 220, 50);
        lower.move(background.pos() + QPoint(150, 100));
        lower.show();
        lower.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&lower));
        TestGlass upper;
        upper.setLowPowerRefreshEnabled(true); // 250 ms external capture cadence
        upper.move(lower.pos());
        upper.show();
        upper.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&upper));
        QTRY_VERIFY_WITH_TIMEOUT(centerPixel(upper).green() > 160, 1500);
        QElapsedTimer latency;
        latency.start();
        lower.overlay = QColor(220, 25, 40);
        lower.update();
        while (latency.elapsed() < 150 && centerPixel(upper).red() < 160)
            QTest::qWait(5);
        QVERIFY2(centerPixel(upper).red() > 160, "Widget-to-widget update waited for the slow desktop capture");
        qInfo() << "Lower card update with 250 ms desktop cadence:" << latency.elapsed() << "ms";
        lower.move(lower.pos() + QPoint(240, 0));
        QTRY_VERIFY_WITH_TIMEOUT(centerPixel(upper).blue() > 140, 150);
        upper.setLowPowerRefreshEnabled(false);
    }

    void foregroundNeverBecomesBackdrop_data() {
        QTest::addColumn<bool>("topmost");
        QTest::addColumn<bool>("translucent");
        QTest::newRow("normal") << false << false;
        QTest::newRow("topmost") << true << false;
        QTest::newRow("translucent-topmost") << true << true;
    }

    void foregroundNeverBecomesBackdrop() {
        QFETCH(bool, topmost);
        QFETCH(bool, translucent);
        ColorWindow background;
        background.setWindowFlag(Qt::WindowStaysOnTopHint, topmost);
        background.color = QColor(20, 45, 215);
        background.setGeometry(QRect(QGuiApplication::primaryScreen()->availableGeometry().center()
                                     - QPoint(300, 220), QSize(600, 440)));
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));
        TestGlass glass;
        glass.setWindowFlag(Qt::WindowStaysOnTopHint, topmost);
        glass.setRefractionPower(1.32f);
        glass.move(background.pos() + QPoint(160, 120));
        glass.show();
        glass.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        QTRY_VERIFY_WITH_TIMEOUT(centerPixel(glass).blue() > 170, 1500);
        ColorWindow foreground;
        foreground.setWindowFlag(Qt::WindowStaysOnTopHint, topmost);
        if (translucent)
            foreground.setWindowOpacity(0.65);
        foreground.setGeometry(QRect(glass.pos() + QPoint(90, -30), QSize(160, 210)));
        foreground.show();
        foreground.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&foreground));
        for (int i = 0; i < 8; ++i) {
            foreground.move(glass.pos() + QPoint(35 + i * 10, -30));
            QTest::qWait(25);
            const auto image = LiquidGlassWidget::captureDesktopComposite(glass.screen(), glass.geometry(), &glass);
            QVERIFY(!image.isNull());
            // Inspect the entire shader input, including the covered half:
            // refraction can bring those pixels back into the exposed edge.
            for (int y = 8; y < image.height() - 8; y += 10)
                for (int x = 8; x < image.width() - 8; x += 10) {
                    const auto pixel = image.pixelColor(x, y);
                    QVERIFY2(pixel.blue() > 170 && pixel.red() < 65,
                             "Foreground contaminated the cached/refraction backdrop");
                }
            QVERIFY(centerPixel(glass).red() < 80);
        }
        foreground.hide();
        background.color = QColor(25, 180, 50);
        background.update();
        QTRY_VERIFY_WITH_TIMEOUT(centerPixel(glass).green() > 140, 1500);
    }

    void foregroundDoesNotEraseLowerGlass() {
        ColorWindow background;
        background.setGeometry(QRect(QGuiApplication::primaryScreen()->availableGeometry().center()
                                     - QPoint(300, 220), QSize(600, 440)));
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));
        TestGlass lower, upper;
        lower.overlay = QColor(25, 220, 50);
        lower.move(background.pos() + QPoint(150, 100));
        upper.move(lower.pos());
        lower.show(); lower.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&lower));
        upper.show(); upper.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&upper));
        QTRY_VERIFY_WITH_TIMEOUT(centerPixel(upper).green() > 160, 1500);
        ColorWindow foreground;
        foreground.setGeometry(QRect(upper.pos() + QPoint(90, -10), QSize(150, 160)));
        foreground.show(); foreground.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&foreground));
        QTest::qWait(80);
        const auto image = LiquidGlassWidget::captureDesktopComposite(upper.screen(), upper.geometry(), &upper);
        QVERIFY(!image.isNull());
        QVERIFY(image.pixelColor(image.rect().center()).green() > 160);
    }

    void coldCaptureUsesOnlyValidExternalPixels() {
        ColorWindow background;
        background.color = QColor(20, 45, 215);
        background.setGeometry(QRect(QGuiApplication::primaryScreen()->availableGeometry().center()
                                     - QPoint(300, 220), QSize(600, 440)));
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));
        TestGlass glass;
        glass.setDesktopCaptureEnabled(false);
        glass.move(background.pos() + QPoint(160, 120));
        glass.show(); glass.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        ColorWindow foreground;
        foreground.setGeometry(QRect(glass.pos() + QPoint(95, -30), QSize(160, 210)));
        foreground.show(); foreground.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&foreground));
        QTest::qWait(60);
        // System screenshots remain allowed to include the card. The
        // composited capture helper therefore scrubs the target rectangle
        // from the internal backdrop frame while preserving the blue desktop
        // visible on the uncovered side and rejecting the red foreground.
        const QImage image = LiquidGlassWidget::captureDesktopComposite(
            glass.screen(), glass.geometry(), &glass);
        QVERIFY(!image.isNull());
        QVERIFY(image.pixelColor(image.width() / 5, image.height() / 2).blue() > 170);
        QVERIFY(image.pixelColor(image.width() * 4 / 5, image.height() / 2)
                    != foreground.color);
    }

    void captureRejectsChangedLowerStack() {
        ColorWindow background;
        background.setGeometry(QRect(QGuiApplication::primaryScreen()->availableGeometry().center()
                                     - QPoint(300, 220), QSize(600, 440)));
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));
        TestGlass glass;
        glass.move(background.pos() + QPoint(160, 120));
        glass.show(); glass.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        auto frame = DesktopCapture::grab(glass.geometry(), glass.devicePixelRatioF(), glass.winId());
        QVERIFY(!frame.image.isNull());
        bool delivered = false;
        DesktopCapture::request(glass.geometry(), glass.devicePixelRatioF(), glass.winId(), &glass,
            [&delivered](DesktopCapture::Frame result) {
                QVERIFY2(result.image.isNull(), "Queued frame from a former lower window was accepted");
                delivered = true;
            });
        background.raise();
        QVERIFY(!DesktopCapture::validate(frame));
        QTRY_VERIFY_WITH_TIMEOUT(delivered, 1500);
    }

    void libraryOwnsNoDesktopWindowAndCancelsDrags() {
#ifdef Q_OS_WIN
        QSettings().setValue("appearance/scale", 0.4);
        BatteryWidget owner(nullptr, BatteryWidget::CardKind::Clock, false);
        owner.move(QGuiApplication::primaryScreen()->availableGeometry().center() - QPoint(150, 100));
        owner.show();
        QVERIFY(QTest::qWaitForWindowExposed(&owner));
        for (int i = 0; i < 6; ++i) {
            const QPoint point = owner.pos();
            mouse(owner, QEvent::MouseButtonPress, {30, 30}, point + QPoint(30, 30), Qt::LeftButton, Qt::LeftButton);
            mouse(owner, QEvent::MouseMove, {60, 30}, point + QPoint(60, 30), Qt::NoButton, Qt::LeftButton);
            QVERIFY(NativeWindows::isTopmost(owner.winId()));
            owner.showWidgetLibrary();
            QVERIFY(QTest::qWaitForWindowExposed(owner.m_library));
            QVERIFY(!NativeWindows::isTopmost(owner.winId()));
            QCOMPARE(GetWindow(reinterpret_cast<HWND>(owner.m_library->winId()), GW_OWNER), HWND(nullptr));
            BatteryWidget added(nullptr, BatteryWidget::CardKind::Weather, false);
            added.move(owner.pos() + QPoint(200, 0));
            added.show();
            QVERIFY(QTest::qWaitForWindowExposed(&added));
            QVERIFY(!NativeWindows::isTopmost(added.winId()));
            // Dragging while the gallery is open must also remain below it.
            const QPoint second = owner.pos();
            mouse(owner, QEvent::MouseButtonPress, {30, 30}, second + QPoint(30, 30), Qt::LeftButton, Qt::LeftButton);
            mouse(owner, QEvent::MouseMove, {60, 30}, second + QPoint(60, 30), Qt::NoButton, Qt::LeftButton);
            const auto stack = NativeWindows::snapshot();
            QVERIFY(NativeWindows::indexOf(stack, owner.m_library->winId()) < NativeWindows::indexOf(stack, owner.winId()));
            mouse(owner, QEvent::MouseButtonRelease, {60, 30}, owner.pos() + QPoint(60, 30), Qt::LeftButton, Qt::NoButton);
            QVERIFY(!NativeWindows::isTopmost(owner.winId()));
            owner.m_library->hide();
            QTest::qWait(30);
        }
#endif
    }

    void desktopCardStaysAboveWallpaperHost() {
#ifdef Q_OS_WIN
        LiquidGlassWidget glass;
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        for (HWND above = GetWindow(reinterpret_cast<HWND>(glass.winId()), GW_HWNDPREV);
             above; above = GetWindow(above, GW_HWNDPREV)) {
            if (!IsWindowVisible(above))
                continue;
            wchar_t name[128]{};
            GetClassNameW(above, name, 128);
            QVERIFY2(wcscmp(name, L"Progman") != 0 && wcscmp(name, L"WorkerW") != 0,
                     "Desktop host is above the widget");
        }
#endif
    }

    void libraryCancelDoesNotDrop() {
        WidgetLibraryDialog library;
        library.prepareBackdrop();
        QScreen* libraryScreen = QGuiApplication::screenAt(QCursor::pos());
        if (!libraryScreen)
            libraryScreen = QGuiApplication::primaryScreen();
        QVERIFY(libraryScreen);
        // The first native show must happen below the work area. Showing at
        // the destination and moving off-screen from showEvent causes a
        // one-frame full-panel flash before the opening animation.
        QVERIFY(library.geometry().top()
                > libraryScreen->availableGeometry().bottom());
        library.show();
        QVERIFY(QTest::qWaitForWindowExposed(&library));
        QTest::qWait(350);
        library.grab().save("artifacts/gallery.png");
        QSignalSpy drops(&library, &WidgetLibraryDialog::widgetDropped);
        QFrame* tile = library.findChild<QFrame*>("widgetTile");
        QVERIFY(tile);
        const QPoint outside = library.geometry().topLeft() - QPoint(20, 20);
        mouse(*tile, QEvent::MouseButtonPress, {30, 30}, tile->mapToGlobal(QPoint(30, 30)), Qt::LeftButton, Qt::LeftButton);
        mouse(*tile, QEvent::MouseMove, {-60, -60}, outside, Qt::NoButton, Qt::LeftButton);
        QTest::keyClick(tile, Qt::Key_Escape);
        mouse(*tile, QEvent::MouseButtonRelease, {-60, -60}, outside, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(drops.count(), 0);
        mouse(*tile, QEvent::MouseButtonPress, {30, 30}, tile->mapToGlobal(QPoint(30, 30)), Qt::LeftButton, Qt::LeftButton);
        mouse(*tile, QEvent::MouseMove, {-60, -60}, outside, Qt::NoButton, Qt::LeftButton);
        mouse(*tile, QEvent::MouseButtonRelease, {-60, -60}, outside, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(drops.count(), 1);
        library.resize(600, 440);
        QTest::qWait(50);
        auto* scroll = library.findChild<QScrollArea*>();
        QVERIFY(scroll);
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        library.grab().save("artifacts/gallery-compact.png");
        library.hide();
        QTest::qWait(100);
        const quint64 revision = library.frameRevision();
        QTest::qWait(120);
        QCOMPARE(library.frameRevision(), revision);
    }

    void hiddenConfigurationRoundTrip() {
        QSettings().setValue("appearance/scale", 0.4);
        const QPoint point = QGuiApplication::primaryScreen()->availableGeometry().center();
        auto* widget = BatteryWidget::addWidget(BatteryWidget::CardKind::Clock, point);
        QVERIFY(widget);
        const QPoint saved = widget->pos();
        widget->hide();
        widget->saveConfiguration();
        delete widget;
        const auto restored = BatteryWidget::restoreWidgets();
        QCOMPARE(restored.size(), 1);
        QCOMPARE(restored.first()->pos(), saved);
        QVERIFY(!restored.first()->isVisible());
        restored.first()->toggleWidgetVisibility();
        QVERIFY(restored.first()->isVisible());
        QCOMPARE(restored.first()->pos(), saved);
        restored.first()->toggleWidgetVisibility();
        QVERIFY(!restored.first()->isVisible());
        QCOMPARE(restored.first()->pos(), saved);
        qDeleteAll(restored);
    }

    void opacityAndContextRecreation() {
        TestGlass glass;
        glass.setDesktopCaptureEnabled(false);
        QImage background(200, 140, QImage::Format_RGB32);
        background.fill(QColor(160, 80, 30));
        glass.setBackgroundImage(background);
        glass.setGlassOpacity(0.5);
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        QTRY_VERIFY(glass.isValid());
        QColor pixel = centerPixel(glass);
        QVERIFY2(qAbs(pixel.alpha() - 128) <= 3, qPrintable(QString::number(pixel.alpha())));
        glass.setRenderBackend(QtGlassFlowScene::CpuBackend);
        QTest::qWait(30);
        QVERIFY(centerPixel(glass).alpha() > 110);
        glass.setRenderBackend(QtGlassFlowScene::GpuBackend);
        QTest::qWait(30);
        QVERIFY(qAbs(centerPixel(glass).alpha() - 128) <= 3);
        glass.setMouseThroughEnabled(true);
        glass.setMouseThroughEnabled(false);
        QTest::qWait(50);
        QVERIFY(qAbs(centerPixel(glass).alpha() - 128) <= 3);
        glass.setGlassOpacity(1.0);
        // Reparenting to another native window can destroy the GL context.
        QWidget parent;
        parent.resize(300, 220);
        parent.show();
        glass.setParent(&parent);
        glass.move(20, 20);
        glass.show();
        QTest::qWait(100);
        QVERIFY(glass.isValid());
        QVERIFY(centerPixel(glass).red() > 130);
        glass.setParent(nullptr); // stack child must outlive its temporary parent
    }

    void coveredWidgetDoesNotCapture() {
        LiquidGlassWidget glass;
        glass.setAnimationEnabled(true);
        glass.move(QGuiApplication::primaryScreen()->availableGeometry().center() - QPoint(150, 100));
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        ColorWindow cover;
        cover.setGeometry(glass.geometry().adjusted(-40, -40, 40, 40));
        cover.show();
        cover.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&cover));
        QTest::qWait(180);
        const quint64 revision = glass.frameRevision();
        for (int i = 0; i < 5; ++i) {
            cover.color = QColor(20 + i * 20, 80, 180);
            cover.update();
            QTest::qWait(60);
        }
        QCOMPARE(glass.frameRevision(), revision);
    }

    void malformedNetworkRepliesAndDictionaryButton() {
        BatteryWidget dictionary(nullptr, BatteryWidget::CardKind::Dictionary, false);
        auto* empty = new FakeReply("[]", &dictionary);
        empty->setProperty("dictionarySerial", dictionary.m_dictionaryReplySerial);
        empty->setProperty("dictionaryWord", dictionary.m_dictionaryWord);
        dictionary.handleDictionaryReply(empty);
        QVERIFY(!dictionary.m_dictionaryDefinition.isEmpty());
        QVERIFY(!dictionary.m_dictionaryDefinition.contains(QStringLiteral("读取在线")));
        const int serial = dictionary.m_dictionaryReplySerial;
        const QPoint button(qRound(50 * dictionary.m_uiScale),
                            dictionary.height() - qRound(36 * dictionary.m_uiScale));
        mouse(dictionary, QEvent::MouseButtonPress, button, dictionary.pos() + button, Qt::LeftButton, Qt::LeftButton);
        mouse(dictionary, QEvent::MouseButtonRelease, button, dictionary.pos() + button, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(dictionary.m_dictionaryReplySerial, serial + 1);
        BatteryWidget weather(nullptr, BatteryWidget::CardKind::Weather, false);
        weather.m_weatherTemperature = "24";
        auto* invalid = new FakeReply("{\"current_condition\":[{}]}", &weather);
        invalid->setProperty("weatherSerial", weather.m_weatherReplySerial);
        weather.handleWeatherReply(invalid);
        QCOMPARE(weather.m_weatherTemperature, QString("24"));
        QVERIFY(weather.m_weatherDescription.contains(QStringLiteral("更新失败")));
        QVERIFY(!weather.m_weatherLoading);
    }
};

QTEST_MAIN(WidgetRegression)
#include "widget_regression.moc"
