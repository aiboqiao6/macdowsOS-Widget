#include "rendering/desktopcapture.h"
#include "rendering/liquidglasswidget.h"
#include "rendering/nativewindows.h"
#include "rendering/responsivelayout.h"
#include "ui/widgetlibrarydialog.h"
#include "ui/firstrunwizard.h"
#include "app/appsettings.h"
#include "ui/liquidglassmenu.h"
#include "widgets/batterywidget.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QDialog>
#include <QLineEdit>
#include <QFrame>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLabel>
#include <QMouseEvent>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QPainter>
#include <QPushButton>
#include <QProcess>
#include <QScreen>
#include <QSettings>
#include <QSlider>
#include <QSignalSpy>
#include <QSharedMemory>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QMenu>
#include <QtTest>
#include <algorithm>
#include <atomic>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
static std::atomic<int> captureFirstChanceExceptions{0};
static quintptr captureModuleStart = 0;
static quintptr captureModuleEnd = 0;
static LONG CALLBACK recordCaptureExceptions(EXCEPTION_POINTERS* exception) {
    if (exception->ExceptionRecord->ExceptionCode == 0xe06d7363) {
        void* frames[32]{};
        const USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
        for (USHORT index = 0; index < count; ++index) {
            const quintptr address = reinterpret_cast<quintptr>(frames[index]);
            if (address >= captureModuleStart && address < captureModuleEnd) {
                ++captureFirstChanceExceptions;
                break;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

class ColorWindow : public QWidget {
public:
    QColor color{210, 35, 25};
    QColor centerStripe;
    ColorWindow() {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), color);
        if (centerStripe.isValid())
            painter.fillRect(QRect(width() / 3, 0, width() / 3, height()), centerStripe);
    }
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

class DisplayCadenceSource : public QOpenGLWidget {
public:
    bool animate = false;
    bool patterned = false;
    int frame = 0;
    std::function<void()> didPaint;
    DisplayCadenceSource() {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        QSurfaceFormat surface = format();
        surface.setSwapInterval(0);
        setFormat(surface);
        connect(this, &QOpenGLWidget::frameSwapped, this, [this]() {
            if (animate) update();
        });
    }
    void paintGL() override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor::fromHsv((++frame * 23) % 360, 220, 210));
        if (patterned) {
            painter.fillRect(rect(), QColor(220, 30, 25));
            painter.fillRect(QRect(width() / 2, 0, width() / 2, height()), QColor(20, 50, 230));
        }
        if (didPaint) didPaint();
    }
};

class ToggleLiveGlass : public TestGlass {
protected:
    bool wallpaperOnlyWhenIdle() const override { return !liveBackdropEnabled(); }
};

class DesktopDragGlass : public ToggleLiveGlass {
protected:
    bool directBackdropDuringDrag() const override { return true; }
};

class DesktopLayerProbe : public LiquidGlassWidget {
public:
    int layerRequests = 0;
    DesktopLayerProbe() {
        setDesktopCaptureEnabled(false);
        setGlassRenderingEnabled(false);
        setFixedSize(120, 80);
    }
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override {
#ifdef Q_OS_WIN
        if (type == "windows_generic_MSG" && message) {
            auto* native = static_cast<MSG*>(message);
            if (native->message == WM_WINDOWPOSCHANGING) {
                const auto* position = reinterpret_cast<WINDOWPOS*>(native->lParam);
                if (position && !(position->flags & SWP_NOZORDER))
                    ++layerRequests;
            }
        }
#endif
        return LiquidGlassWidget::nativeEvent(type, message, result);
    }
};

class TextQualityGlass : public TestGlass {
public:
    int quality = 0;
    void paintOverlay(QPainter& painter) override {
        painter.scale(.85, .85);
        QFont font(BatteryWidget::pingFangFontFamily(), 52, QFont::Normal);
        BatteryWidget::applyFontSmoothing(font);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.setRenderHint(QPainter::TextAntialiasing, quality > 0);
        ResponsiveLayout::drawSingleLine(painter, QRectF(12, 12, 530, 94),
            Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("长沙 28.8°"));
        font.setPointSize(19);
        painter.setFont(font);
        ResponsiveLayout::drawSingleLine(painter, QRectF(12, 110, 530, 45),
            Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("11时  14时  17时  32°"));
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

class PendingReply : public QNetworkReply {
public:
    explicit PendingReply(QObject* parent) : QNetworkReply(parent) {
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    void abort() override {
        setError(OperationCanceledError, QStringLiteral("Cancelled"));
        setFinished(true);
        emit finished();
    }
protected:
    qint64 readData(char*, qint64) override { return -1; }
};

class PendingNetwork : public QNetworkAccessManager {
public:
    using QNetworkAccessManager::QNetworkAccessManager;
    QPointer<QNetworkReply> pending;
protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest&, QIODevice*) override {
        pending = new PendingReply(this);
        return pending;
    }
};

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

    void init() {
        // Each test changes persisted appearance/interaction settings. Avoid
        // leaking click-through or live mode into the next independent case.
        QSettings().clear();
        QFile::remove(AppSettings::filePath());
    }

    void settingsJsonMigratesAndPreservesLayout() {
        {
            AppSettings defaults;
            QCOMPARE(defaults.value("appearance/scale").toDouble(), 1.0);
            QCOMPARE(defaults.value("performance/renderScale").toDouble(), 0.60);
            QCOMPARE(defaults.value("appearance/blurStrength").toInt(), 50);
            QCOMPARE(defaults.value("appearance/opacity").toDouble(), 1.0);
        }
        QVERIFY(!QFile::exists(AppSettings::filePath()));

        const QJsonArray oldWidgets{QJsonObject{{"id", "existing-card"},
                                                {"type", "clock"}, {"x", 50}, {"y", 60}}};
        QDir().mkpath(QFileInfo(AppSettings::filePath()).absolutePath());
        QFile oldFile(AppSettings::filePath());
        QVERIFY(oldFile.open(QIODevice::WriteOnly));
        oldFile.write(QJsonDocument(QJsonObject{
                          {"version", 4}, {"scale", 0.85}, {"widgets", oldWidgets},
                          {"settings", QJsonObject{{"appearance", QJsonObject{
                              {"systemBlur", true}, {"blurStrength", 65}}},
                              {"performance", QJsonObject{{"renderBackend", 2}}}}}}).toJson());
        oldFile.close();
        QSettings().setValue("appearance/scale", 0.85);
        QSettings().setValue("weather/location", "长沙");
        {
            AppSettings settings;
            QCOMPARE(settings.value("appearance/scale").toDouble(), 0.85);
            QCOMPARE(settings.value("weather/location").toString(), QStringLiteral("长沙"));
            settings.setValue("appearance/opacity", 0.65);
            QVERIFY(settings.sync());
        }
        QFile migrated(AppSettings::filePath());
        QVERIFY(migrated.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(migrated.readAll()).object();
        migrated.close();
        QCOMPARE(root.value("widgets").toArray(), oldWidgets);
        const QJsonObject appearance = root.value("settings").toObject()
                                           .value("appearance").toObject();
        QCOMPARE(appearance.value("scale").toDouble(), 0.85);
        QCOMPARE(appearance.value("opacity").toDouble(), 0.65);
        QCOMPARE(appearance.value("blurStrength").toInt(), 65);
        QVERIFY(!appearance.contains("systemBlur"));
        QVERIFY(!root.value("settings").toObject().value("performance").toObject()
                     .contains("renderBackend"));
        QCOMPARE(root.value("settings").toObject().value("weather").toObject()
                     .value("location").toString(), QStringLiteral("长沙"));
        QVERIFY(root.value("settings").toObject().contains("interaction"));

        BatteryWidget::saveAllConfigurations();
        QFile layoutSaved(AppSettings::filePath());
        QVERIFY(layoutSaved.open(QIODevice::ReadOnly));
        root = QJsonDocument::fromJson(layoutSaved.readAll()).object();
        QCOMPARE(root.value("settings").toObject().value("appearance").toObject()
                     .value("opacity").toDouble(), 0.65);
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

    void cardsPreserveOriginalSizeAndTransparentCorners() {
        for (auto kind : {BatteryWidget::CardKind::Battery, BatteryWidget::CardKind::Weather,
                          BatteryWidget::CardKind::Clock, BatteryWidget::CardKind::Dictionary}) {
            AppSettings().setValue("appearance/scale", 1.0);
            BatteryWidget widget(nullptr, kind, false, QStringLiteral("corner-probe"), 0);
            QCOMPARE(widget.size(), QSize(328, 328));
            QCOMPARE(widget.m_uiScale, 1.0);
            QCOMPARE(AppSettings().value("appearance/scale").toDouble(), 1.0);
            widget.setDesktopLayerEnabled(false);
            widget.setDesktopCaptureEnabled(false);
            widget.setAnimationEnabled(false);
            QImage background(328, 328, QImage::Format_RGB32);
            background.fill(QColor(53, 67, 84));
            widget.setBackgroundImage(background);
            widget.show();
            QVERIFY(QTest::qWaitForWindowExposed(&widget));
            const QImage frame = widget.grabFramebuffer().convertToFormat(QImage::Format_ARGB32);
            QVERIFY(!frame.isNull());
            for (int y = 0; y < 5; ++y) {
                for (int x = 0; x < 5; ++x) {
                    QCOMPARE(qAlpha(frame.pixel(x, y)), 0);
                    QCOMPARE(qAlpha(frame.pixel(frame.width() - 1 - x, y)), 0);
                    QCOMPARE(qAlpha(frame.pixel(x, frame.height() - 1 - y)), 0);
                    QCOMPARE(qAlpha(frame.pixel(frame.width() - 1 - x, frame.height() - 1 - y)), 0);
                }
            }
            QVERIFY(frame.save(QStringLiteral("artifacts/original-card-%1.png").arg(int(kind))));
            widget.hide();
        }
    }

    void batteryVariantsKeepGridShapeAndRender() {
        AppSettings().setValue("appearance/scale", 0.4);

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
        AppSettings().setValue("appearance/scale", 0.4);

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
        AppSettings().setValue("appearance/scale", 0.4);

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
        AppSettings().setValue("appearance/scale", 0.4);
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

    void freePlacementOnlySnapsBesideVisibleCards() {
        AppSettings().setValue("appearance/scale", 0.4);
        QScreen* screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QRect area = screen->availableGeometry();
        const QPoint freePosition = area.topLeft() + QPoint(85, 95);
        BatteryWidget card(nullptr, BatteryWidget::CardKind::Clock, false,
                           QStringLiteral("free-placement"), 0);
        QVERIFY(card.placeAtDesktopPoint(freePosition + QPoint(card.width() / 2,
                                                               card.height() / 2)));
        QCOMPARE(card.pos(), freePosition);
        QVERIFY(!BatteryWidget::nearbyWidgetRect(BatteryWidget::CardKind::Clock, 0,
                                                  freePosition + QPoint(45, 37), 0.4).isValid());

        card.show();
        QVERIFY(QTest::qWaitForWindowExposed(&card));
        const QPoint beside(card.geometry().right() + 9, card.y());
        const QRect snapped = BatteryWidget::nearbyWidgetRect(
            BatteryWidget::CardKind::Clock, 0, beside, 0.4);
        QVERIFY(snapped.isValid());
        QCOMPARE(snapped.top(), card.y());
        QVERIFY(snapped.left() > card.geometry().right());
        const QPoint alongEdge(beside.x(), card.y() + 30);
        const QRect edgeSnap = BatteryWidget::nearbyWidgetRect(
            BatteryWidget::CardKind::Clock, 0, alongEdge, 0.4);
        QVERIFY(edgeSnap.isValid());
        QCOMPARE(edgeSnap, snapped);
        QCOMPARE(BatteryWidget::nearbyWidgetRect(BatteryWidget::CardKind::Clock, 0,
                     QPoint(beside.x(), card.y() + 55), 0.4), snapped);
        QCOMPARE(BatteryWidget::nearbyWidgetRect(BatteryWidget::CardKind::Clock, 0,
                     beside + QPoint(40, 0), 0.4), snapped);
        QCOMPARE(BatteryWidget::nearbyWidgetRect(BatteryWidget::CardKind::Clock, 0,
                     beside + QPoint(60, 0), 0.4), snapped);
        QVERIFY(!BatteryWidget::nearbyWidgetRect(BatteryWidget::CardKind::Clock, 0,
                     beside + QPoint(130, 0), 0.4).isValid());
        QVERIFY(!BatteryWidget::nearbyWidgetRect(BatteryWidget::CardKind::Clock, 0,
                  area.center(), 0.4).isValid());

        BatteryWidget added(nullptr, BatteryWidget::CardKind::Clock, false,
                            QStringLiteral("overlap-drop"), 0);
        QVERIFY(added.placeAtDesktopPoint(card.geometry().center()));
        QVERIFY(!added.geometry().intersects(card.geometry()));

        BatteryWidget dragged(nullptr, BatteryWidget::CardKind::Clock, false,
                              QStringLiteral("overlap-drag"), 0);
        dragged.move(area.center());
        dragged.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dragged));
        dragged.m_dragOrigin = dragged.pos();
        dragged.move(card.pos());
        dragged.windowDragFinished();
        QVERIFY(!dragged.geometry().intersects(card.geometry()));

        BatteryWidget restored(nullptr, BatteryWidget::CardKind::Clock, false,
                               QStringLiteral("overlap-show"), 0);
        restored.move(card.pos());
        restored.show();
        QVERIFY(QTest::qWaitForWindowExposed(&restored));
        QVERIFY(!restored.geometry().intersects(card.geometry()));
        QVERIFY(!restored.geometry().intersects(dragged.geometry()));
    }

    void allFourCornerSlotsTriggerWithoutSliding() {
        AppSettings().setValue("appearance/scale", 0.4);
        QScreen* screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        BatteryWidget anchor(nullptr, BatteryWidget::CardKind::Clock, false,
                             QStringLiteral("corner-anchor"), 0);
        anchor.move(screen->availableGeometry().center()
                    - QPoint(anchor.width() / 2, anchor.height() / 2));
        anchor.show();
        QVERIFY(QTest::qWaitForWindowExposed(&anchor));

        const QRect bounds = anchor.geometry();
        const QPoint probes[] = {
            {bounds.left() - anchor.width() - 9, bounds.top() - anchor.height() - 9},
            {bounds.right() + 10, bounds.top() - anchor.height() - 9},
            {bounds.left() - anchor.width() - 9, bounds.bottom() + 10},
            {bounds.right() + 10, bounds.bottom() + 10}
        };
        const QPoint farther[] = {{-25, -25}, {25, -25}, {-25, 25}, {25, 25}};
        for (int index = 0; index < 4; ++index) {
            const QRect slot = BatteryWidget::nearbyWidgetRect(
                BatteryWidget::CardKind::Clock, 0, probes[index], 0.4);
            QVERIFY2(slot.isValid(), "Every corner needs a placement slot");
            QVERIFY(!slot.intersects(bounds));
            QCOMPARE(BatteryWidget::nearbyWidgetRect(BatteryWidget::CardKind::Clock, 0,
                         probes[index] + farther[index], 0.4), slot);
            if (index % 2 == 0)
                QVERIFY(slot.right() < bounds.left());
            else
                QVERIFY(slot.left() > bounds.right());
            if (index < 2)
                QVERIFY(slot.bottom() < bounds.top());
            else
                QVERIFY(slot.top() > bounds.bottom());
        }
    }

    void firstRunWizardPersistsSelections() {
        FirstRunWizard wizard;
        auto* scale = wizard.findChild<QSlider*>();
        const auto boxes = wizard.findChildren<QComboBox*>();
        auto* live = wizard.findChild<QCheckBox*>();
        auto* welcomeNext = wizard.findChild<QPushButton*>(QStringLiteral("continueButton"));
        auto* scaleNext = wizard.findChild<QPushButton*>(QStringLiteral("primaryButton"));
        auto* start = wizard.findChild<QPushButton*>(QStringLiteral("startButton"));
        auto* welcome = wizard.findChild<QLabel*>(QStringLiteral("welcomeTitle"));
        QVERIFY(scale && boxes.size() == 1 && live && welcomeNext && scaleNext && start);
        QVERIFY(welcome && welcome->text().contains(QStringLiteral("欢迎使用")));
        QVERIFY(!QFile::exists(AppSettings::filePath()));
        QCOMPARE(wizard.renderScale(), 0.6f);
        QCOMPARE(wizard.glassOpacity(), 1.0);
        QVERIFY(wizard.liveBackdropEnabled());
        QVERIFY(wizard.desktopCaptureEnabled());
        QCOMPARE(scale->value(), 100);
        QCOMPARE(boxes.at(0)->currentData().toDouble(), 0.60);
        QVERIFY(live->isChecked());
        wizard.show();
        QVERIFY(QTest::qWaitForWindowExposed(&wizard));
        QTest::qWait(450);
        QVERIFY(wizard.grab().save(QStringLiteral("artifacts/first-run-welcome.png")));
        QVERIFY(!QFile::exists(AppSettings::filePath()));
        const QPoint fixedPosition = wizard.pos();
        QTest::mousePress(&wizard, Qt::LeftButton, Qt::NoModifier, QPoint(8, 8));
        QTest::mouseMove(&wizard, QPoint(90, 80));
        QTest::mouseRelease(&wizard, Qt::LeftButton, Qt::NoModifier, QPoint(90, 80));
        QCOMPARE(wizard.pos(), fixedPosition);
        welcomeNext->click();
        QTest::qWait(380);
        scale->setValue(85);
        QVERIFY(wizard.grab().save(QStringLiteral("artifacts/first-run-scale.png")));
        scaleNext->click();
        QTest::qWait(380);
        boxes.at(0)->setCurrentIndex(3);
        live->setChecked(true);
        QVERIFY(wizard.grab().save(QStringLiteral("artifacts/first-run-quality.png")));
        QCOMPARE(scale->value(), 85);
        start->click();
        QCOMPARE(AppSettings().value("appearance/scale").toDouble(), 0.85);
        QCOMPARE(AppSettings().value("performance/renderScale").toDouble(), 1.25);
        QVERIFY(!AppSettings().contains("performance/renderBackend"));
        QVERIFY(AppSettings().value("appearance/liveBackdrop").toBool());
        QVERIFY(!AppSettings().contains("onboarding/completed"));
        QVERIFY(QFile::exists(AppSettings::filePath()));
    }

    void firstRunWizardPagesScaleLive() {
        FirstRunWizard wizard;
        wizard.show();
        QVERIFY(QTest::qWaitForWindowExposed(&wizard));
        QTest::qWait(450);
        const auto pages = wizard.findChildren<QWidget*>(QStringLiteral("wizardPage"));
        QCOMPARE(pages.size(), 3);
        auto* welcomeNext = wizard.findChild<QPushButton*>(QStringLiteral("continueButton"));
        auto* scaleNext = wizard.findChild<QPushButton*>(QStringLiteral("primaryButton"));
        QVERIFY(welcomeNext && scaleNext);
        welcomeNext->click();
        QTest::qWait(90);
        QVERIFY(pages.at(1)->x() > 0 && pages.at(1)->x() < wizard.width());
        QTest::qWait(320);
        QCOMPARE(pages.at(1)->x(), 0);
        auto* slider = wizard.findChild<QSlider*>();
        auto* scaleValue = wizard.findChild<QLabel*>(QStringLiteral("scaleValue"));
        QVERIFY(slider && scaleValue);
        QVERIFY(!wizard.findChild<QFrame*>(QStringLiteral("previewPanel")));
        QVERIFY(!wizard.findChild<QFrame*>(QStringLiteral("previewCard")));
        for (int percent : {40, 85, 100, 150, 200, 100}) {
            slider->setValue(percent);
            QApplication::processEvents();
            QCOMPARE(scaleValue->text(), QStringLiteral("%1%").arg(percent));
            const QSize available = wizard.screen()->availableGeometry().size() - QSize(32, 32);
            QCOMPARE(wizard.size(), QSize(qRound(780 * percent / 100.0),
                                          qRound(540 * percent / 100.0)).boundedTo(available));
            QCOMPARE(scaleNext->height(), qRound(48 * percent / 100.0));
            QVERIFY(scaleValue->styleSheet().contains(
                QStringLiteral("font-size:%1px").arg(qRound(20 * percent / 100.0))));
            QVERIFY(!QFile::exists(AppSettings::filePath()));
            wizard.grab().save(QStringLiteral("artifacts/wizard-scale-%1.png").arg(percent));
        }
        wizard.grab().save("artifacts/wizard-scale-no-preview.png");
        scaleNext->click();
        QTest::qWait(90);
        QVERIFY(pages.at(2)->x() > 0 && pages.at(2)->x() < wizard.width());
        QTest::qWait(320);
        QCOMPARE(pages.at(2)->x(), 0);
        QCOMPARE(wizard.renderScale(), 0.6f);
        QCOMPARE(wizard.font().family(), BatteryWidget::pingFangFontFamily());
        wizard.hide();
    }

    void wizardSliderStaysRoundAtFractionalScales() {
        FirstRunWizard wizard;
        wizard.setDesktopCaptureEnabled(false);
        wizard.setLiveBackdropEnabled(false, false);
        wizard.show();
        QVERIFY(QTest::qWaitForWindowExposed(&wizard));
        QTest::qWait(450);
        const auto pages = wizard.findChildren<QWidget*>("wizardPage");
        wizard.findChild<QPushButton*>("continueButton")->click();
        QTRY_VERIFY(pages[1]->isVisible() && pages[1]->pos() == QPoint());
        QTest::qWait(30);
        auto* slider = wizard.findChild<QSlider*>();
        QVERIFY(slider);
        for (int percent = 40; percent <= 200; percent += 5) {
            slider->setValue(percent);
            QApplication::processEvents();
            const QImage image = slider->grab().toImage().convertToFormat(QImage::Format_ARGB32);
            QRect whiteBounds;
            int whitePixels = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const QColor color = image.pixelColor(x, y);
                    if (color.alpha() > 235 && color.red() > 235 && color.green() > 235 && color.blue() > 235) {
                        whiteBounds |= QRect(x, y, 1, 1);
                        ++whitePixels;
                    }
                }
            }
            QVERIFY2(!whiteBounds.isEmpty(), qPrintable(QStringLiteral("No thumb at %1%").arg(percent)));
            const qreal coverage = whitePixels / qreal(whiteBounds.width() * whiteBounds.height());
            QVERIFY2(coverage > .60 && coverage < .89,
                qPrintable(QStringLiteral("Thumb is square at %1% (white coverage %2)").arg(percent).arg(coverage)));
            QVERIFY(qAbs(whiteBounds.width() - whiteBounds.height()) <= 2);
            if (percent == 105 || percent == 125 || percent == 175)
                image.save(QStringLiteral("artifacts/wizard-slider-%1.png").arg(percent));
        }
    }

    void wizardQualityPopupIsExcludedFromCapture() {
#ifndef Q_OS_WIN
        QSKIP("Checks Windows capture preflight");
#endif
#ifdef Q_OS_WIN
        ColorWindow background;
        background.setGeometry(QGuiApplication::primaryScreen()->availableGeometry());
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));
        FirstRunWizard wizard;
        wizard.show();
        wizard.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&wizard));
        QTest::qWait(450);
        const auto pages = wizard.findChildren<QWidget*>("wizardPage");
        wizard.findChild<QPushButton*>("continueButton")->click();
        QTRY_VERIFY(pages[1]->isVisible() && pages[1]->pos() == QPoint());
        QTest::qWait(30);
        wizard.findChild<QPushButton*>("primaryButton")->click();
        QTRY_VERIFY(pages[2]->isVisible() && pages[2]->pos() == QPoint());
        QTest::qWait(80);
        QTRY_VERIFY(!wizard.m_lastCompositedBackdrop.isNull());
        HMODULE captureModule = GetModuleHandleW(L"GraphicsCapture.dll");
        MODULEINFO module{};
        QVERIFY(captureModule && GetModuleInformation(GetCurrentProcess(), captureModule, &module, sizeof(module)));
        captureModuleStart = reinterpret_cast<quintptr>(module.lpBaseOfDll);
        captureModuleEnd = captureModuleStart + module.SizeOfImage;
        captureFirstChanceExceptions = 0;
        void* handler = AddVectoredExceptionHandler(1, recordCaptureExceptions);
        QVERIFY(handler);
        const auto removeHandler = qScopeGuard([&]() { RemoveVectoredExceptionHandler(handler); });
        auto* quality = wizard.findChild<QComboBox*>();
        QVERIFY(quality);
        for (int index : {0, 1, 3}) {
            quality->showPopup();
            QWidget* popup = quality->view()->window();
            QTRY_VERIFY(popup && popup->isVisible());
            wchar_t className[128]{};
            GetClassNameW(reinterpret_cast<HWND>(popup->winId()), className, 128);
            QVERIFY2(!NativeWindows::isCaptureCandidate(popup->winId()), qPrintable(QString::fromWCharArray(className)));
            const auto stack = NativeWindows::snapshot();
            const int position = NativeWindows::indexOf(stack, popup->winId());
            QVERIFY(position >= 0 && stack[position].excluded);
            const auto revision = wizard.frameRevision();
            QTest::qWait(250);
            QVERIFY(wizard.frameRevision() > revision);
            quality->hidePopup();
            quality->setCurrentIndex(index);
            QTest::qWait(150);
        }
        QCOMPARE(captureFirstChanceExceptions.load(), 0);
#endif
    }

    void firstRunWizardModalRunCompletes() {
        FirstRunWizard wizard;
        auto* welcomeNext = wizard.findChild<QPushButton*>(QStringLiteral("continueButton"));
        auto* scaleNext = wizard.findChild<QPushButton*>(QStringLiteral("primaryButton"));
        auto* start = wizard.findChild<QPushButton*>(QStringLiteral("startButton"));
        const auto pages = wizard.findChildren<QWidget*>(QStringLiteral("wizardPage"));
        QVERIFY(welcomeNext && scaleNext && start);
        QCOMPARE(pages.size(), 3);
        QTimer driver;
        driver.setInterval(50);
        int visibleTicks = 0;
        connect(&driver, &QTimer::timeout, &wizard, [&]() {
            if (!wizard.isVisible())
                return;
            if (visibleTicks < 8) {
                ++visibleTicks;
                return;
            }
            if (pages.at(2)->isVisible() && pages.at(2)->pos() == QPoint()) {
                driver.stop();
                wizard.grab().save(QStringLiteral("artifacts/first-run-modal-quality.png"));
                start->click();
            } else if (pages.at(1)->isVisible() && pages.at(1)->pos() == QPoint()) {
                wizard.grab().save(QStringLiteral("artifacts/first-run-modal-scale.png"));
                scaleNext->click();
            } else if (pages.at(0)->isVisible() && pages.at(0)->pos() == QPoint()) {
                wizard.grab().save(QStringLiteral("artifacts/first-run-modal-welcome.png"));
                welcomeNext->click();
            }
        });
        driver.start();
        QTimer::singleShot(8000, &wizard, &QWidget::close);
        QVERIFY(wizard.run());
        const AppSettings saved;
        QCOMPARE(saved.value("appearance/scale").toDouble(), 1.0);
        QCOMPARE(saved.value("performance/renderScale").toDouble(), 0.60);
        QCOMPARE(saved.value("appearance/liveBackdrop").toBool(), true);
        QCOMPARE(saved.value("appearance/blurStrength").toInt(), 50);
        QCOMPARE(saved.value("appearance/opacity").toDouble(), 1.0);
        QCOMPARE(saved.value("appearance/fontSmoothing").toInt(), 2);
        QCOMPARE(saved.value("interaction/mouseThrough").toBool(), false);
        QCOMPARE(saved.value("interaction/lowPowerRefresh").toBool(), false);
        QCOMPARE(saved.value("weather/location").toString(), QStringLiteral("北京"));
        QCOMPARE(saved.value("weather/cityId").toString(), QStringLiteral("101010100"));
        QCOMPARE(saved.value("startup/enabled").toBool(), false);
    }

    void firstRunWizardLiveBackdropAtDisplayCadence_data() {
        QTest::addColumn<bool>("capture");
        QTest::newRow("live") << true;
        QTest::newRow("static") << false;
    }

    void animatedBackdropSource() {
        const QString marker = qEnvironmentVariable("WIDGET_TEST_BACKDROP_MARKER");
        if (marker.isEmpty()) QSKIP("Helper process for live backdrop measurements");
        QSharedMemory counter(marker);
        QVERIFY(counter.attach());
        DisplayCadenceSource background;
        background.didPaint = [&]() {
            counter.lock();
            ++*static_cast<qint64*>(counter.data());
            counter.unlock();
        };
        background.setGeometry(QGuiApplication::primaryScreen()->availableGeometry());
        const bool moving = qEnvironmentVariableIntValue("WIDGET_TEST_BACKDROP_MOVE");
        const QPoint center = background.geometry().center();
        if (moving) {
            background.setGeometry(QRect(center - QPoint(450, 310), QSize(900, 620)));
            background.patterned = true;
        }
        background.animate = qEnvironmentVariableIntValue("WIDGET_TEST_BACKDROP_ANIMATE");
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));
        QFile ready(marker);
        QVERIFY(ready.open(QIODevice::WriteOnly));
        ready.write("ready");
        ready.close();
        QTimer movement;
        movement.setTimerType(Qt::PreciseTimer);
        movement.setInterval(16);
        int moves = 0;
        connect(&movement, &QTimer::timeout, &background, [&]() {
            const int phase = (++moves * 7) % 800;
            const int offset = phase < 400 ? phase - 200 : 600 - phase;
            background.move(center - QPoint(450, 310) + QPoint(offset, 0));
        });
        if (moving) movement.start();
        QEventLoop loop;
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
    }

    void firstRunWizardFollowsMovingBackground() {
#ifndef Q_OS_WIN
        QSKIP("Requires Windows Graphics Capture");
#endif
        QTemporaryDir sourceFiles;
        const QString marker = sourceFiles.filePath("ready");
        QSharedMemory counter(marker);
        QVERIFY(counter.create(sizeof(qint64)));
        *static_cast<qint64*>(counter.data()) = 0;
        QProcess source;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("WIDGET_TEST_BACKDROP_MARKER", marker);
        environment.insert("WIDGET_TEST_BACKDROP_MOVE", "1");
        environment.insert("WIDGET_TEST_BACKDROP_ANIMATE", "0");
        source.setProcessEnvironment(environment);
        source.start(QCoreApplication::applicationFilePath(), {"animatedBackdropSource", "-o", "-", "txt"});
        const auto stopSource = qScopeGuard([&]() { source.kill(); source.waitForFinished(); });
        QVERIFY(source.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(marker), 5000);
        FirstRunWizard wizard;
        QElapsedTimer clock;
        int updates = 0;
        qint64 lastUpdate = 0;
        qint64 longestGap = 0;
        qint64 previousKey = 0;
        connect(&wizard, &QOpenGLWidget::frameSwapped, &wizard, [&]() {
            if (!clock.isValid()) return;
            const qint64 key = wizard.m_lastCompositedBackdrop.cacheKey();
            if (key && key != previousKey) {
                previousKey = key;
                const qint64 now = clock.elapsed();
                longestGap = qMax(longestGap, now - lastUpdate);
                lastUpdate = now;
                ++updates;
            }
        });
        QTimer::singleShot(1200, &wizard, [&]() { clock.start(); });
        QTimer::singleShot(4200, &wizard, &QWidget::close);
        QVERIFY(!wizard.run());
        longestGap = qMax(longestGap, clock.elapsed() - lastUpdate);
        qInfo() << "Moving background:" << updates << "updates in" << clock.elapsed()
                << "ms; longest freeze" << longestGap << "ms";
        QVERIFY2(updates >= 60, "Moving lower window starved live background delivery");
        QVERIFY2(longestGap < 250, "Moving lower window froze the backdrop for over 250 ms");
    }

    void firstRunWizardLiveBackdropAtDisplayCadence() {
#ifndef Q_OS_WIN
        QSKIP("Requires Windows Graphics Capture");
#endif
        QFETCH(bool, capture);
        QTemporaryDir sourceFiles;
        const QString marker = sourceFiles.filePath("ready");
        QSharedMemory counter(marker);
        QVERIFY(counter.create(sizeof(qint64)));
        *static_cast<qint64*>(counter.data()) = 0;
        const auto sourceCount = [&]() {
            counter.lock();
            const qint64 frames = *static_cast<qint64*>(counter.data());
            counter.unlock();
            return frames;
        };
        qint64 sourceStart = 0;
        QProcess source;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("WIDGET_TEST_BACKDROP_MARKER", marker);
        environment.insert("WIDGET_TEST_BACKDROP_ANIMATE", capture ? "1" : "0");
        source.setProcessEnvironment(environment);
        source.start(QCoreApplication::applicationFilePath(), {"animatedBackdropSource", "-o", "-", "txt"});
        QVERIFY(source.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(marker), 5000);
        FirstRunWizard wizard;
        wizard.setDesktopCaptureEnabled(capture);
        int changedFrames = 0;
        int presentedFrames = 0;
        QElapsedTimer composeClock;
        qint64 composeNs = 0;
        bool measuring = false;
        qint64 lastKey = 0;
        connect(&wizard, &QOpenGLWidget::frameSwapped, &wizard, [&]() {
            if (!measuring) return;
            if (composeClock.isValid()) composeNs += composeClock.nsecsElapsed();
            ++presentedFrames;
            const qint64 key = wizard.m_lastCompositedBackdrop.cacheKey();
            if (key != lastKey) {
                ++changedFrames;
                lastKey = key;
            }
        });
        connect(&wizard, &QOpenGLWidget::aboutToCompose, &wizard, [&]() { composeClock.start(); });
        QElapsedTimer measured;
        QTimer::singleShot(1500, &wizard, [&]() {
            wizard.makeCurrent();
            qInfo() << "Renderer:" << reinterpret_cast<const char*>(
                wizard.context()->functions()->glGetString(GL_RENDERER));
            wizard.doneCurrent();
            measuring = true;
            sourceStart = sourceCount();
            measured.start();
        });
        QTimer::singleShot(4000, &wizard, &QWidget::close);
        QVERIFY(!wizard.run());
        const qint64 elapsed = measured.elapsed();
        const qint64 paintedSourceFrames = sourceCount() - sourceStart;
        source.kill();
        QVERIFY(source.waitForFinished());
        const qreal fps = presentedFrames * 1000.0 / elapsed;
        const qreal hz = wizard.screen()->refreshRate();
        const int sourceFrames = qRound(hz * elapsed / 1000.0);
        qInfo() << "Wizard:" << fps << "presented FPS on" << hz << "Hz display;"
                << changedFrames << "changed backdrops /" << sourceFrames << "expected display ticks; compose ms"
                << composeNs / qreal(qMax(1, presentedFrames)) / 1e6;
        qInfo() << "Source paints:" << paintedSourceFrames;
        qInfo() << "Fresh backdrop FPS:" << changedFrames * 1000.0 / elapsed;
        QVERIFY2(fps >= hz * 0.97, "Wizard presentation is below display cadence");
        if (capture)
            QVERIFY2(changedFrames >= qMax<qint64>(10, paintedSourceFrames / 2), "Wizard backdrop is stale");
        QVERIFY(!QFile::exists(AppSettings::filePath()));
    }

    void draggingCardHidesApplicationWindowsButKeepsWidgets() {
        AppSettings().setValue("appearance/scale", 0.4);

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
        QColor initialBackdrop;
        QElapsedTimer initialCaptureClock;
        initialCaptureClock.start();
        do {
            initialBackdrop = centerPixel(glass);
            if (initialBackdrop.red() > 170)
                break;
            QTest::qWait(50);
        } while (initialCaptureClock.elapsed() < 6000);
        QVERIFY2(initialBackdrop.red() > 170,
                 qPrintable(QStringLiteral("initial backdrop rgba(%1,%2,%3,%4)")
                                .arg(initialBackdrop.red()).arg(initialBackdrop.green())
                                .arg(initialBackdrop.blue()).arg(initialBackdrop.alpha())));
        // The old self-removal code stretched the two horizontal edge pixels
        // across the card, destroying this vertical feature into scan-line
        // bands. The internal exclusion path must reveal its real 2-D detail.
        background.centerStripe = QColor(20, 210, 45);
        background.repaint();
        QColor observed;
        QTRY_VERIFY_WITH_TIMEOUT((observed = centerPixel(glass),
                                  observed.green() > 155 && observed.red() < 120), 3500);
        glass.grabFramebuffer().save("artifacts/live-pattern.png");
        for (int frame = 0; frame < 12; ++frame) {
            QTest::qWait(35);
            const QColor color = centerPixel(glass);
            QVERIFY2(color.green() > 155 && color.red() < 120,
                     qPrintable(QStringLiteral("stable frame %1 sampled rgba(%2,%3,%4,%5)")
                                    .arg(frame).arg(color.red()).arg(color.green())
                                    .arg(color.blue()).arg(color.alpha())));
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
            const QColor color = centerPixel(glass);
            QVERIFY2(color.green() > 155 && color.red() < 120,
                     qPrintable(QStringLiteral("drag frame %1 sampled rgba(%2,%3,%4,%5)")
                                    .arg(i).arg(color.red()).arg(color.green())
                                    .arg(color.blue()).arg(color.alpha())));
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

    void draggingKeepsInteractiveRenderCadence() {
        TestGlass glass;
        // Isolate the scene scheduler from desktop-capture completions: a
        // moving static card must repaint at interaction cadence on its own.
        glass.setDesktopCaptureEnabled(false);
        glass.move(QGuiApplication::primaryScreen()->availableGeometry().center()
                   - QPoint(glass.width() / 2, glass.height() / 2));
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        QTest::qWait(80);

        const QPoint origin = glass.pos();
        mouse(glass, QEvent::MouseButtonPress, {30, 30}, origin + QPoint(30, 30),
              Qt::LeftButton, Qt::LeftButton);
        mouse(glass, QEvent::MouseMove, {60, 30}, origin + QPoint(60, 30),
              Qt::NoButton, Qt::LeftButton);
        QVERIFY(glass.dragging());
        QScreen* dragScreen = QGuiApplication::screenAt(glass.frameGeometry().center());
        qreal refreshRate = dragScreen ? dragScreen->refreshRate() : 60.0;
        if (!qIsFinite(refreshRate) || refreshRate < 24.0)
            refreshRate = 60.0;
        const int expectedInterval = qBound(
            2, qFloor(1000.0 / qMin<qreal>(refreshRate, 500.0)), 42);
        QCOMPARE(glass.refreshInterval(), expectedInterval);
        const quint64 startRevision = glass.frameRevision();
        QTest::qWait(140);
        QVERIFY2(glass.frameRevision() >= startRevision + 3,
                 qPrintable(QStringLiteral("drag produced only %1 frames in 140 ms")
                                .arg(glass.frameRevision() - startRevision)));

        mouse(glass, QEvent::MouseButtonRelease, {60, 30}, glass.pos() + QPoint(60, 30),
              Qt::LeftButton, Qt::NoButton);
        QVERIFY(!glass.dragging());
    }

    void desktopDragUsesSameWorkWithLiveOnAndOff() {
        DesktopDragGlass lower;
        DesktopDragGlass moving;
        const QPoint center = QGuiApplication::primaryScreen()->availableGeometry().center();
        lower.move(center - QPoint(110, 70));
        moving.move(center - QPoint(30, 45));
        lower.show();
        moving.show();
        moving.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&moving));
        for (const bool live : {false, true}) {
            lower.setLiveBackdropEnabled(live);
            moving.setLiveBackdropEnabled(live);
            const QPoint origin = moving.pos();
            mouse(moving, QEvent::MouseButtonPress, {30, 30}, origin + QPoint(30, 30),
                  Qt::LeftButton, Qt::LeftButton);
            mouse(moving, QEvent::MouseMove, {60, 30}, origin + QPoint(60, 30),
                  Qt::NoButton, Qt::LeftButton);
            QVERIFY(moving.dragging());
            QTest::qWait(150);
            QCOMPARE(lower.m_backdropTimer->interval(), 10000);
            QCOMPARE(moving.m_backdropTimer->interval(), moving.activeDisplayInterval());
            // A WGC request queued immediately before pointer-down may still
            // be starting. It must complete and never be replenished in drag.
            QTRY_VERIFY_WITH_TIMEOUT(!moving.m_capturesInFlight && !lower.m_capturesInFlight, 2000);
            const quint64 lowerRevision = lower.frameRevision();
            const quint64 movingRevision = moving.frameRevision();
            int steps = 0;
            QTimer movement;
            movement.setInterval(moving.activeDisplayInterval());
            connect(&movement, &QTimer::timeout, &moving, [&]() {
                mouse(moving, QEvent::MouseMove, {60, 30},
                    origin + QPoint(60 + (++steps % 24), 30), Qt::NoButton, Qt::LeftButton);
            });
            movement.start();
            QTest::qWait(700);
            movement.stop();
            qInfo() << "Drag live=" << live << "frames=" << moving.frameRevision() - movingRevision
                    << "peer frames=" << lower.frameRevision() - lowerRevision;
            QCOMPARE(lower.frameRevision(), lowerRevision);
            QVERIFY(moving.frameRevision() > movingRevision + 10);
            QVERIFY(!moving.m_capturesInFlight);
            mouse(moving, QEvent::MouseButtonRelease, {60, 30}, moving.pos() + QPoint(60, 30),
                  Qt::LeftButton, Qt::NoButton);
            QCOMPARE(lower.m_backdropTimer->interval(), live ? lower.activeDisplayInterval() : 10000);
            QTest::qWait(150);
        }
        lower.setLiveBackdropEnabled(false);
        moving.setLiveBackdropEnabled(false);
        auto removed = std::make_unique<DesktopDragGlass>();
        lower.setLiveBackdropEnabled(true);
        removed->move(center);
        removed->show();
        removed->raise();
        QVERIFY(QTest::qWaitForWindowExposed(removed.get()));
        mouse(*removed, QEvent::MouseButtonPress, {30, 30}, center + QPoint(30, 30),
              Qt::LeftButton, Qt::LeftButton);
        mouse(*removed, QEvent::MouseMove, {60, 30}, center + QPoint(60, 30),
              Qt::NoButton, Qt::LeftButton);
        QVERIFY(removed->dragging());
        QCOMPARE(lower.m_backdropTimer->interval(), 10000);
        removed.reset();
        QCOMPARE(lower.m_backdropTimer->interval(), lower.activeDisplayInterval());
        lower.setLiveBackdropEnabled(false);
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

    void repeatedOverlapDoesNotAccumulateBrightness() {
        ColorWindow background;
        background.color = QColor(18, 34, 92);
        background.setGeometry(QRect(QGuiApplication::primaryScreen()->availableGeometry().center()
                                     - QPoint(320, 230), QSize(640, 460)));
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));

        TestGlass lower;
        lower.overlay = QColor(35, 170, 95, 210);
        lower.move(background.pos() + QPoint(170, 130));
        lower.show(); lower.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&lower));

        TestGlass upper;
        upper.setRefractionPower(1.32f);
        upper.move(lower.pos());
        upper.show(); upper.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&upper));
        QTRY_VERIFY_WITH_TIMEOUT(centerPixel(upper).green() > 100, 4000);
        QTest::qWait(250);

        QVector<int> brightnesses;
        for (int pass = 0; pass < 14; ++pass) {
            upper.move(lower.pos() + QPoint(96, 0));
            upper.captureDesktopBackdrop();
            QTest::qWait(80);
            upper.move(lower.pos());
            upper.captureDesktopBackdrop();
            QTest::qWait(100);
            const QColor sample = centerPixel(upper);
            const int brightness = sample.red() + sample.green() + sample.blue();
            brightnesses.append(brightness);
        }
        const auto stableBegin = brightnesses.cbegin() + brightnesses.size() / 2;
        const auto stableEnd = brightnesses.cend();
        const auto [minimum, maximum] = std::minmax_element(stableBegin, stableEnd);
        QVERIFY2(*maximum - *minimum <= 15,
                 qPrintable(QStringLiteral("Settled overlap brightness drifted by %1")
                                .arg(*maximum - *minimum)));
    }

    void globalFontSmoothingProfiles() {
        for (int level = 0; level <= 3; ++level) {
            BatteryWidget::setGlobalFontSmoothing(level);
            QCOMPARE(BatteryWidget::globalFontSmoothing(), level);
            QCOMPARE(AppSettings().value("appearance/fontSmoothing").toInt(), level);
            QFont font(BatteryWidget::pingFangFontFamily());
            BatteryWidget::applyFontSmoothing(font);
            if (level == 0)
                QVERIFY(int(font.styleStrategy()) & int(QFont::NoAntialias));
            else
                QVERIFY(int(font.styleStrategy()) & int(QFont::PreferAntialias));
        }
        BatteryWidget::setGlobalFontSmoothing(2);
    }

    void fontSmoothingChangesRenderedCoverage() {
        const auto renderText = [](int level) {
            BatteryWidget::setGlobalFontSmoothing(level);
            QImage image(260, 100, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::TextAntialiasing, level > 0);
            QFont font(BatteryWidget::pingFangFontFamily(), 48, QFont::DemiBold);
            BatteryWidget::applyFontSmoothing(font);
            painter.setFont(font);
            painter.setPen(Qt::white);
            painter.drawText(image.rect(), Qt::AlignCenter, QStringLiteral("Aa 26"));
            painter.end();
            return image;
        };
        const QImage aliased = renderText(0);
        const QImage smoothed = renderText(3);
        QVERIFY(aliased != smoothed);
        int aliasedPartial = 0;
        int smoothedPartial = 0;
        for (int y = 0; y < aliased.height(); ++y) {
            for (int x = 0; x < aliased.width(); ++x) {
                const int a0 = qAlpha(aliased.pixel(x, y));
                const int a1 = qAlpha(smoothed.pixel(x, y));
                aliasedPartial += a0 > 0 && a0 < 255;
                smoothedPartial += a1 > 0 && a1 < 255;
            }
        }
        QVERIFY2(smoothedPartial > aliasedPartial,
                 qPrintable(QStringLiteral("partial alpha pixels: off=%1 on=%2")
                                .arg(aliasedPartial).arg(smoothedPartial)));
        BatteryWidget::setGlobalFontSmoothing(2);
    }

    void fontSmoothingRefreshesOpenGalleryPreviews() {
        BatteryWidget::setGlobalFontSmoothing(0);
        WidgetLibraryDialog library;
        QFrame* tile = nullptr;
        for (QFrame* candidate : library.findChildren<QFrame*>(QStringLiteral("widgetTile"))) {
            if (candidate->property("tileKind").toInt() == 2
                && candidate->property("tileVariant").toInt() == 8) {
                tile = candidate;
                break;
            }
        }
        QVERIFY(tile);
        const QImage aliased = tile->grab().toImage();
        BatteryWidget::setGlobalFontSmoothing(3);
        const QImage smoothed = tile->grab().toImage();
        QVERIFY(!aliased.isNull());
        QVERIFY2(aliased != smoothed,
                 "The open gallery kept the preview rendered with the previous font setting");
        BatteryWidget::setGlobalFontSmoothing(2);
    }

    void clearTextMatchesNativeRasterAtFractionalScale() {
        // A partial-alpha count alone rewards blur. Compare actual glyph
        // pixels with Qt's native-size rasterization, including small CJK.
        const QString text = QStringLiteral("添加小组件 时钟 28.4 Aa");
        for (int level : {2, 3}) {
            BatteryWidget::setGlobalFontSmoothing(level);
            for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
                for (qreal uiScale : {.7, 1.0}) {
                    for (int size : {12, 16, 28}) {
                        QImage actual(1100, 160, QImage::Format_ARGB32_Premultiplied);
                        actual.setDevicePixelRatio(dpr);
                        actual.fill(Qt::transparent);
                        QFont font(BatteryWidget::pingFangFontFamily());
                        font.setPixelSize(size);
                        BatteryWidget::applyFontSmoothing(font);
                        const QPointF baseline(16.25, 60.25);
                        QPainter painter(&actual);
                        painter.scale(uiScale, uiScale);
                        painter.setFont(font);
                        painter.setPen(Qt::white);
                        QVERIFY(ResponsiveLayout::drawHighQualityText(painter, baseline, text));
                        painter.end();

                        QImage expected(actual.size(), actual.format());
                        expected.fill(Qt::transparent);
                        font.setPixelSize(qRound(size * dpr * uiScale));
                        font.setStyleStrategy(static_cast<QFont::StyleStrategy>(
                            font.styleStrategy() | QFont::NoSubpixelAntialias));
                        QPainter reference(&expected);
                        reference.setFont(font);
                        reference.setPen(Qt::white);
                        reference.setRenderHint(QPainter::TextAntialiasing);
                        reference.drawText(QPointF(qRound(baseline.x() * dpr * uiScale),
                                                  qRound(baseline.y() * dpr * uiScale)), text);
                        reference.end();
                        expected.setDevicePixelRatio(dpr);
                        QCOMPARE(actual, expected);
                    }
                }
            }
        }
        BatteryWidget::setGlobalFontSmoothing(2);
    }

    void galleryPreviewsUseNativePixels() {
        BatteryWidget::setGlobalFontSmoothing(3);
        const QSize box(204, 128);
        for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
            const QImage image = BatteryWidget::renderPreview(
                BatteryWidget::CardKind::Dictionary, 0, box, dpr);
            QVERIFY(!image.isNull());
            QCOMPARE(image.devicePixelRatio(), dpr);
            QCOMPARE(image.size(), QSize(qRound(128 * dpr), qRound(128 * dpr)));
        }
        WidgetLibraryDialog library;
        library.selectCategory(1);
        // Layout/stacking changes can reorder QObject's child list.
        auto* tile = library.m_tiles.first();
        QVERIFY(tile);
        const qreal dpr = tile->devicePixelRatioF();
        const qreal galleryScale = tile->width() / 220.0;
        const qreal previewDpr = dpr * galleryScale;
        const QImage reference = BatteryWidget::renderPreview(
            BatteryWidget::CardKind::Battery, 0, box, previewDpr);
        QImage rendered(tile->size() * dpr, reference.format());
        rendered.setDevicePixelRatio(dpr);
        rendered.fill(Qt::transparent);
        QPainter tilePainter(&rendered);
        tile->render(&tilePainter, QPoint(), QRegion(), QWidget::DrawChildren);
        tilePainter.end();
        const QPoint origin = (QPointF(110, 66) * previewDpr
            - QPointF(reference.width() * .5, reference.height() * .5)).toPoint();
        QImage crop = rendered.copy(QRect(origin, reference.size()));
        crop.setDevicePixelRatio(previewDpr);
        qInfo() << "Preview native DPI" << dpr << "origin" << origin;
        crop.save(QStringLiteral("artifacts/preview-native-tile.png"));
        reference.save(QStringLiteral("artifacts/preview-native-reference.png"));
        // The battery payload can update between two captures. Compare the
        // static antialiased rim to detect any filtering of the preview image.
        const QRect rim(0, 0, reference.width(), qRound(3 * dpr));
        QCOMPARE(crop.copy(rim), reference.copy(rim));
        for (auto* nav : library.m_navButtons) {
            const QPixmap icon = nav->icon().pixmap(QSize(27, 27), 2.0);
            QCOMPARE(icon.size(), QSize(54, 54));
            QCOMPARE(icon.devicePixelRatio(), 2.0);
        }
        BatteryWidget::setGlobalFontSmoothing(2);
    }

    void galleryDictionaryTextAppearance() {
        BatteryWidget::setGlobalFontSmoothing(3);
        WidgetLibraryDialog library;
        library.prepareBackdrop();
        library.selectCategory(4);
        library.show();
        QVERIFY(QTest::qWaitForWindowExposed(&library));
        QTest::qWait(400);
        QVERIFY(library.grab().save(QStringLiteral("artifacts/gallery-dictionary-native.png")));
        QCOMPARE(qApp->font().family(), BatteryWidget::pingFangFontFamily());
        for (QWidget* control : library.findChildren<QWidget*>())
            QCOMPARE(control->font().family(), BatteryWidget::pingFangFontFamily());
        library.hide();
        BatteryWidget::setGlobalFontSmoothing(2);
    }

    void fontSmoothingReachesGalleryControls() {
        BatteryWidget::setGlobalFontSmoothing(0);
        WidgetLibraryDialog library;
        QFont menuFont(BatteryWidget::pingFangFontFamily());
        menuFont.setPixelSize(16);
        BatteryWidget::applyFontSmoothing(menuFont);
        LiquidGlassMenu menu(menuFont);
        auto* nav = library.findChild<QPushButton*>(QStringLiteral("navSelected"));
        auto* search = library.findChild<QLineEdit*>();
        QVERIFY(nav);
        QVERIFY(search);
        for (int level = 0; level <= 3; ++level) {
            BatteryWidget::setGlobalFontSmoothing(level);
            for (QWidget* control : {static_cast<QWidget*>(nav),
                                     static_cast<QWidget*>(search),
                                     static_cast<QWidget*>(&menu)}) {
                const auto strategy = control->font().styleStrategy();
                if (level == 0)
                    QVERIFY(int(strategy) & int(QFont::NoAntialias));
                else
                    QVERIFY(int(strategy) & int(QFont::PreferAntialias));
                QCOMPARE(bool(strategy & QFont::NoSubpixelAntialias),
                         level == 1 || level == 3);
            }
        }
        WidgetLibraryDialog newlyOpened;
        for (QWidget* control : newlyOpened.findChildren<QWidget*>()) {
            if (!qobject_cast<QLabel*>(control)
                && !qobject_cast<QPushButton*>(control)
                && !qobject_cast<QLineEdit*>(control))
                continue;
            const auto strategy = control->font().styleStrategy();
            QVERIFY(int(strategy) & int(QFont::PreferAntialias));
            QVERIFY(int(strategy) & int(QFont::NoSubpixelAntialias));
        }
        BatteryWidget::setGlobalFontSmoothing(2);
    }

    void enablingLiveBackdropRefreshesStationaryCard() {
#ifndef Q_OS_WIN
        QSKIP("Requires Windows compositor");
#endif
        const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
        ColorWindow background;
        background.color = QColor(210, 35, 25);
        background.setGeometry(QRect(area.center() - QPoint(320, 230), QSize(640, 460)));
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));

        ToggleLiveGlass glass;
        glass.setLiveBackdropEnabled(false);
        QCOMPARE(glass.m_backdropTimer->interval(), 10000);
        glass.move(background.pos() + QPoint(180, 120));
        glass.show();
        glass.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));

        glass.setLiveBackdropEnabled(true);
        QCOMPARE(glass.m_backdropTimer->interval(), glass.activeDisplayInterval());
        QVERIFY(glass.m_backdropTimer->isActive());
        QColor observed;
        QTRY_VERIFY_WITH_TIMEOUT((observed = centerPixel(glass), observed.red() > 170), 3000);
        background.color = QColor(20, 205, 45);
        background.repaint();
        QTRY_VERIFY_WITH_TIMEOUT((observed = centerPixel(glass),
                                  observed.green() > 155 && observed.red() < 120), 3000);
        // A legitimate solid frame must not be replaced with the previous
        // coloured frame by a capture-hole heuristic.
        for (const QColor color : {QColor(Qt::black), QColor(Qt::white), QColor(20, 40, 210)}) {
            background.color = color;
            background.repaint();
            QTRY_VERIFY_WITH_TIMEOUT((observed = centerPixel(glass),
                qAbs(observed.red() - color.red()) < 60
                && qAbs(observed.green() - color.green()) < 60
                && qAbs(observed.blue() - color.blue()) < 60), 3000);
        }
    }

    void stationaryLiveRenderingKeepsCadenceAndStopsWhenDisabled() {
        ToggleLiveGlass glass;
        glass.setDesktopCaptureEnabled(false);
        glass.setAnimationEnabled(false);
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        glass.setLiveBackdropEnabled(true);
        QCOMPARE(glass.refreshInterval(), glass.activeDisplayInterval());
        QTest::qWait(100);
        quint64 revision = glass.frameRevision();
        QTest::qWait(250);
        QVERIFY2(glass.frameRevision() >= revision + 5,
                 "Stationary live mode stopped rendering without pointer input");

        glass.setLiveBackdropEnabled(false);
        QTest::qWait(100);
        revision = glass.frameRevision();
        QTest::qWait(150);
        QCOMPARE(glass.frameRevision(), revision);
        glass.setLiveBackdropEnabled(true);
        glass.hide();
        QTest::qWait(80);
        revision = glass.frameRevision();
        QTest::qWait(150);
        QCOMPARE(glass.frameRevision(), revision);
        glass.show();
        QTRY_VERIFY_WITH_TIMEOUT(glass.frameRevision() >= revision + 3, 1500);
        glass.setMouseThroughEnabled(true);
        glass.setLowPowerRefreshEnabled(true);
        QCOMPARE(glass.refreshInterval(), 250);
        glass.setLiveBackdropEnabled(false);
    }

    void desktopWidgetFollowsLiveWallpaperHost() {
#ifndef Q_OS_WIN
        QSKIP("Requires a native Windows wallpaper host");
#else
        // Model a wallpaper application's WorkerW host, not an ordinary
        // foreground window. This previously took the static-file fallback.
        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"WorkerW";
        type.lpfnWndProc = [](HWND window, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (message == WM_NCCREATE) {
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
                    reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
            }
            if (message == WM_PAINT) {
                PAINTSTRUCT paint{};
                HDC dc = BeginPaint(window, &paint);
                const auto* color = reinterpret_cast<COLORREF*>(GetWindowLongPtrW(window, GWLP_USERDATA));
                HBRUSH brush = CreateSolidBrush(color ? *color : RGB(0, 0, 0));
                RECT bounds{};
                GetClientRect(window, &bounds);
                FillRect(dc, &bounds, brush);
                DeleteObject(brush);
                EndPaint(window, &paint);
                return 0;
            }
            return DefWindowProcW(window, message, wParam, lParam);
        };
        QVERIFY(RegisterClassW(&type));
        const auto unregister = qScopeGuard([&]() { UnregisterClassW(type.lpszClassName, type.hInstance); });
        COLORREF color = RGB(220, 30, 20);
        QScreen* screen = QGuiApplication::primaryScreen();
        const QPoint center = screen->availableGeometry().center();
        const qreal dpr = screen->devicePixelRatio();
        HWND host = CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName, L"Live wallpaper regression",
            WS_POPUP, qRound((center.x() - 350) * dpr), qRound((center.y() - 300) * dpr),
            qRound(700 * dpr), qRound(600 * dpr), nullptr, nullptr, type.hInstance, &color);
        QVERIFY(host);
        const auto destroy = qScopeGuard([&]() { DestroyWindow(host); NativeWindows::invalidate(); });
        ShowWindow(host, SW_SHOWNOACTIVATE);
        SetWindowPos(host, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        AppSettings().setValue("appearance/scale", 0.4);
        BatteryWidget widget(nullptr, BatteryWidget::CardKind::Battery, false,
                             QStringLiteral("live-wallpaper-probe"), 0);
        // Isolate this capture from the user's already-open desktop cards.
        widget.setDesktopLayerEnabled(false);
        widget.setWindowFlag(Qt::WindowStaysOnTopHint);
        widget.move(center - QPoint(widget.width() / 2, widget.height() / 2));
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));
        const auto backdropColor = [&]() {
            const QImage& frame = widget.m_lastCompositedBackdrop;
            return frame.isNull() ? QColor() : frame.pixelColor(frame.width() / 2, frame.height() / 2);
        };
        QTRY_VERIFY_WITH_TIMEOUT(backdropColor().red() > 180 && backdropColor().blue() < 80, 4000);
        color = RGB(20, 30, 220);
        InvalidateRect(host, nullptr, FALSE);
        QTRY_VERIFY_WITH_TIMEOUT(backdropColor().blue() > 180 && backdropColor().red() < 80, 1500);
        widget.setLiveBackdropEnabled(false);
        QTest::qWait(100);
        const QImage frozen = widget.m_lastCompositedBackdrop;
        color = RGB(20, 220, 30);
        InvalidateRect(host, nullptr, FALSE);
        QTest::qWait(200);
        QCOMPARE(widget.m_lastCompositedBackdrop, frozen);
        widget.setLiveBackdropEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(backdropColor().green() > 180 && backdropColor().blue() < 80, 1500);
#endif
    }

    void stationaryLiveBackdropFollowsAnimation() {
#ifndef Q_OS_WIN
        QSKIP("Requires Windows Graphics Capture");
#endif
        ColorWindow background;
        const QPoint center = QGuiApplication::primaryScreen()->availableGeometry().center();
        background.setGeometry(QRect(center - QPoint(320, 230), QSize(640, 460)));
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));
        ToggleLiveGlass glass;
        glass.move(center - QPoint(100, 70));
        glass.setLiveBackdropEnabled(true);
        glass.show();
        glass.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        QTest::qWait(500);
        int sourceFrames = 0;
        int changedFrames = 0;
        qint64 lastKey = glass.m_lastCompositedBackdrop.cacheKey();
        QTimer animation;
        animation.setInterval(glass.activeDisplayInterval());
        animation.setTimerType(Qt::PreciseTimer);
        connect(&animation, &QTimer::timeout, &background, [&]() {
            background.color = QColor::fromHsv((++sourceFrames * 23) % 360, 220, 210);
            background.update();
        });
        connect(&glass, &QtGlassFlowScene::frameRendered, &glass, [&]() {
            const qint64 key = glass.m_lastCompositedBackdrop.cacheKey();
            if (key != lastKey) {
                ++changedFrames;
                lastKey = key;
            }
        });
        animation.start();
        QTest::qWait(2000);
        animation.stop();
        qInfo() << "Stationary backdrop:" << changedFrames << "changes /"
                << sourceFrames << "source frames in 2 seconds";
        QVERIFY(changedFrames >= qMax(20, sourceFrames / 2));
        glass.setLiveBackdropEnabled(false);
    }

    void captureSurvivesResizeRecreationAndCancelledReceivers() {
#ifndef Q_OS_WIN
        QSKIP("Requires Windows Graphics Capture");
#endif
        ColorWindow background;
        const QPoint center = QGuiApplication::primaryScreen()->availableGeometry().center();
        background.setGeometry(QRect(center - QPoint(320, 230), QSize(640, 460)));
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));
        for (int cycle = 0; cycle < 12; ++cycle) {
            auto glass = std::make_unique<ToggleLiveGlass>();
            glass->move(center - QPoint(100, 70));
            glass->setLiveBackdropEnabled(true);
            glass->show();
            glass->raise();
            QVERIFY(QTest::qWaitForWindowExposed(glass.get()));
            background.resize(cycle % 2 ? QSize(620, 440) : QSize(680, 500));
            background.color = cycle % 2 ? QColor(20, 205, 45) : QColor(210, 35, 25);
            background.repaint();
            QTRY_VERIFY_WITH_TIMEOUT(!glass->m_lastCompositedBackdrop.isNull()
                && (cycle % 2
                    ? glass->m_lastCompositedBackdrop.pixelColor(glass->m_lastCompositedBackdrop.rect().center()).green() > 150
                    : glass->m_lastCompositedBackdrop.pixelColor(glass->m_lastCompositedBackdrop.rect().center()).red() > 170), 3000);
            for (int toggle = 0; toggle < 6; ++toggle) {
                glass->setLiveBackdropEnabled(false);
                glass->setRenderScale(toggle % 2 ? 1.0f : .5f);
                glass->setLiveBackdropEnabled(true);
            }
            // Keep a request alive while its UI receiver is destroyed.
            DesktopCapture::request(glass->frameGeometry(), 1, glass->winId(), glass.get(),
                                    [](DesktopCapture::Frame) {});
            glass.reset();
            QTest::qWait(20);
        }
        AppSettings().setValue(QStringLiteral("appearance/liveBackdrop"), false);
    }

    void liveCaptureSoak() {
#ifdef Q_OS_WIN
        const int duration = qMax(3000, qEnvironmentVariableIntValue("WIDGET_STRESS_MS"));
        ColorWindow background;
        const QPoint center = QGuiApplication::primaryScreen()->availableGeometry().center();
        background.setGeometry(QRect(center - QPoint(320, 230), QSize(640, 460)));
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));
        ToggleLiveGlass glass;
        glass.move(center - QPoint(100, 70));
        glass.setLiveBackdropEnabled(true);
        glass.show();
        glass.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        int sourceFrames = 0;
        QTimer animation;
        animation.setInterval(16);
        animation.setTimerType(Qt::PreciseTimer);
        connect(&animation, &QTimer::timeout, &background, [&]() {
            background.color = QColor::fromHsv((++sourceFrames * 13) % 360, 230, 210);
            background.update();
            if (sourceFrames % 60 == 0)
                background.resize(sourceFrames % 120 ? QSize(620, 440) : QSize(680, 500));
        });
        animation.start();
        QTest::qWait(3000); // Warm driver, shaders and the capture pool.
        PROCESS_MEMORY_COUNTERS_EX initial{};
        initial.cb = sizeof(initial);
        QVERIFY(GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&initial), sizeof(initial)));
        DWORD initialHandles = 0;
        QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &initialHandles));
        const DWORD initialGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        QElapsedTimer elapsed;
        elapsed.start();
        int changes = 0;
        QImage previous = glass.m_lastCompositedBackdrop;
        while (elapsed.elapsed() < duration) {
            const quint64 revision = glass.frameRevision();
            QTest::qWait(250);
            QVERIFY(glass.frameRevision() > revision);
            if (glass.m_lastCompositedBackdrop != previous) {
                ++changes;
                previous = glass.m_lastCompositedBackdrop;
            }
        }
        animation.stop();
        PROCESS_MEMORY_COUNTERS_EX finalMemory{};
        finalMemory.cb = sizeof(finalMemory);
        QVERIFY(GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&finalMemory), sizeof(finalMemory)));
        DWORD finalHandles = 0;
        QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &finalHandles));
        const DWORD finalGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        qInfo() << "Soak ms:" << elapsed.elapsed() << "source frames:" << sourceFrames
                << "changed samples:" << changes
                << "private bytes:" << initial.PrivateUsage << finalMemory.PrivateUsage
                << "handles:" << initialHandles << finalHandles
                << "GDI:" << initialGdi << finalGdi;
        QVERIFY(changes >= duration / 1000);
        QVERIFY(finalMemory.PrivateUsage <= initial.PrivateUsage + 64 * 1024 * 1024);
        QVERIFY(finalHandles <= initialHandles + 64);
        QVERIFY(finalGdi <= initialGdi + 8);
        glass.setLiveBackdropEnabled(false);
#else
        QSKIP("Requires Windows Graphics Capture");
#endif
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
        AppSettings().setValue("appearance/scale", 0.4);
        AppSettings().setValue("appearance/opacity", 1.0);
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
        const QColor overlapping = centerPixel(upper);
        lower.move(lower.pos() + QPoint(240, 0));
        QColor uncovered;
        QTRY_VERIFY_WITH_TIMEOUT((uncovered = centerPixel(upper),
            qAbs(uncovered.red() - overlapping.red())
            + qAbs(uncovered.green() - overlapping.green())
            + qAbs(uncovered.blue() - overlapping.blue()) > 80), 250);
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
        // Showing a non-topmost tool window does not guarantee that Windows
        // raises it above the app which launched this test process. Establish
        // the fixture order explicitly before placing the glass above it.
        background.raise();
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
        AppSettings().setValue("appearance/scale", 0.4);
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

    void libraryHonorsGlobalLiveBackdropSetting() {
        AppSettings().setValue(QStringLiteral("appearance/liveBackdrop"), false);
        AppSettings().setValue(QStringLiteral("appearance/blurStrength"), 72);
        AppSettings().setValue(QStringLiteral("appearance/opacity"), 0.80);
        WidgetLibraryDialog staticLibrary;
        QVERIFY(!staticLibrary.liveBackdropEnabled());
        QCOMPARE(staticLibrary.materialBlurStrength(), 72);
        QCOMPARE(staticLibrary.glassOpacity(), 0.80);
        staticLibrary.prepareBackdrop();
        staticLibrary.show();
        QVERIFY(QTest::qWaitForWindowExposed(&staticLibrary));
        QTest::qWait(400);
        QVERIFY(!staticLibrary.m_liveBackdropTimer.isActive());
        staticLibrary.hide();

        AppSettings().setValue(QStringLiteral("appearance/liveBackdrop"), true);
        WidgetLibraryDialog liveLibrary;
        QVERIFY(liveLibrary.liveBackdropEnabled());
        liveLibrary.prepareBackdrop();
        liveLibrary.show();
        QVERIFY(QTest::qWaitForWindowExposed(&liveLibrary));
        QTRY_VERIFY_WITH_TIMEOUT(liveLibrary.m_liveBackdropTimer.isActive(), 1000);
        QCOMPARE(liveLibrary.m_liveBackdropTimer.interval(),
                 qMax(1, liveLibrary.activeDisplayInterval() / 2));
        liveLibrary.hide();
        AppSettings().setValue(QStringLiteral("appearance/liveBackdrop"), false);
    }

    void libraryLiveBackdropFollowsAnimatedWindow() {
#ifndef Q_OS_WIN
        QSKIP("Requires Windows Graphics Capture");
#endif
        AppSettings().setValue(QStringLiteral("appearance/liveBackdrop"), true);
        AppSettings().setValue(QStringLiteral("performance/renderScale"), 0.5);
        ColorWindow background;
        background.setGeometry(QGuiApplication::primaryScreen()->availableGeometry());
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));

        WidgetLibraryDialog library;
        library.prepareBackdrop();
        library.show();
        library.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&library));
        QTRY_VERIFY_WITH_TIMEOUT(library.m_liveBackdropTimer.isActive(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(!library.m_backdropCanvas.isNull(), 2000);

        const int interval = library.activeDisplayInterval();
        background.color = QColor(20, 90, 180);
        background.update();
        const quint64 warmupSamples = library.m_liveBackdropSamples;
        QTRY_VERIFY_WITH_TIMEOUT(library.m_liveBackdropSamples > warmupSamples, 1000);

        const quint64 initialFrames = library.m_liveBackdropFrames;
        const quint64 initialSamples = library.m_liveBackdropSamples;
        int sourceFrames = 0;
        QTimer sourceTimer;
        sourceTimer.setTimerType(Qt::PreciseTimer);
        sourceTimer.setInterval(interval);
        connect(&sourceTimer, &QTimer::timeout, &background, [&]() {
            background.color = QColor::fromHsv((sourceFrames * 23) % 360, 220, 210);
            ++sourceFrames;
            background.update();
        });
        QElapsedTimer sampleClock;
        sampleClock.start();
        sourceTimer.start();
        QTest::qWait(1000);
        sourceTimer.stop();
        QTest::qWait(interval * 2);
        const qint64 sampleMs = sampleClock.elapsed();
        const int changedFrames = int(library.m_liveBackdropFrames - initialFrames);
        const int capturedSamples = int(library.m_liveBackdropSamples - initialSamples);
        // The producer timer is the observable workload here. A GUI test
        // process may not receive every nominal display tick, so compare
        // capture cadence with the source frames it actually emitted.
        const int expectedSourceSamples = qMax(1, sourceFrames);
        qInfo() << "Library animated backdrop frames:" << changedFrames
                << "changed," << capturedSamples << "samples /" << sourceFrames
                << "source frames at" << interval << "ms cadence in"
                << sampleMs << "ms";
        QVERIFY2(capturedSamples >= expectedSourceSamples * 4 / 5,
                 qPrintable(QStringLiteral("library sampled only %1/%2 emitted source frames")
                                .arg(capturedSamples).arg(expectedSourceSamples)));
        // DWM/WGC may coalesce adjacent compositor frames while the source
        // window is animating. Require a sustained stream of visibly changed
        // captures without assuming one unique readback for every source tick.
        QVERIFY2(changedFrames >= qMax(10, sourceFrames / 3),
                 qPrintable(QStringLiteral("library received only %1/%2 animated source frames")
                                .arg(changedFrames).arg(sourceFrames)));
        library.hide();
        AppSettings().setValue(QStringLiteral("appearance/liveBackdrop"), false);
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

    void stationaryCardsRejectNativeRaisesAndRecoverTheirLayer() {
#ifdef Q_OS_WIN
        ColorWindow application;
        application.setWindowFlag(Qt::WindowStaysOnTopHint, false);
        application.setGeometry(80, 80, 320, 240);
        application.show();
        QVERIFY(QTest::qWaitForWindowExposed(&application));
        application.raise();
        DesktopLayerProbe first, second;
        first.move(100, 100);
        second.move(240, 100);
        first.show();
        second.show();
        QVERIFY(QTest::qWaitForWindowExposed(&first));
        QVERIFY(QTest::qWaitForWindowExposed(&second));
        const auto belowApp = [&]() {
            const auto stack = NativeWindows::snapshot();
            const int appIndex = NativeWindows::indexOf(stack, application.winId());
            return appIndex >= 0 && NativeWindows::indexOf(stack, first.winId()) > appIndex
                && NativeWindows::indexOf(stack, second.winId()) > appIndex
                && !NativeWindows::isTopmost(first.winId())
                && !NativeWindows::isTopmost(second.winId());
        };
        QTRY_VERIFY_WITH_TIMEOUT(belowApp(), 1500);
        const HWND window = reinterpret_cast<HWND>(first.winId());
        constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
        for (HWND after : {HWND_TOP, HWND_TOPMOST}) {
            QVERIFY(SetWindowPos(window, after, 0, 0, 0, 0, flags));
            // No event-loop grace period: the proposed raise is redirected
            // before Windows can display even one foreground frame.
            QVERIFY(belowApp());
        }
        first.raise();
        QVERIFY(belowApp());
        // Some native callers bypass WINDOWPOSCHANGING. The topology watcher
        // must still repair those changes with desktop capture disabled.
        QVERIFY(SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, flags | SWP_NOSENDCHANGING));
        QTRY_VERIFY_WITH_TIMEOUT(belowApp(), 1500);
        QVERIFY(SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, flags | SWP_NOSENDCHANGING));
        QTRY_VERIFY_WITH_TIMEOUT(belowApp(), 1500);
        first.hide();
        first.show();
        QTRY_VERIFY_WITH_TIMEOUT(belowApp(), 1500);
        QTest::qWait(150);
        const int settledRequests = first.layerRequests + second.layerRequests;
        QTest::qWait(250);
        QVERIFY2(first.layerRequests + second.layerRequests <= settledRequests + 2,
                 "Idle cards repeatedly reorder one another");

        const QPoint origin = first.pos();
        mouse(first, QEvent::MouseButtonPress, {10, 10}, origin + QPoint(10, 10), Qt::LeftButton, Qt::LeftButton);
        mouse(first, QEvent::MouseMove, {50, 10}, origin + QPoint(50, 10), Qt::NoButton, Qt::LeftButton);
        QVERIFY(NativeWindows::isTopmost(first.winId()));
        mouse(first, QEvent::MouseButtonRelease, {50, 10}, first.pos() + QPoint(50, 10), Qt::LeftButton, Qt::NoButton);
        QVERIFY(belowApp());
#else
        QSKIP("Requires native Windows Z-order");
#endif
    }

    void glassMenuAppearanceAndInput() {
        AppSettings().setValue("appearance/liveBackdrop", false);
        AppSettings().setValue("performance/renderScale", 1.5);
        BatteryWidget::setGlobalFontSmoothing(3);
        QFont font(BatteryWidget::pingFangFontFamily());
        font.setPixelSize(16);
        BatteryWidget::applyFontSmoothing(font);
        ColorWindow backdrop;
        backdrop.color = QColor(36, 75, 125);
        backdrop.centerStripe = QColor(210, 143, 66);
        const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
        backdrop.setGeometry(QRect(available.center() - QPoint(220, 160), QSize(440, 320)));
        backdrop.show();
        backdrop.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&backdrop));
        QTest::qWait(100);
        LiquidGlassMenu menu(font, &backdrop);
        QAction* add = menu.addGlassAction(QStringLiteral("添加小组件…"), LiquidGlassMenu::Icon::Add);
        menu.addSeparator();
        QAction* search = menu.addGlassAction(QStringLiteral("搜索城市…"), LiquidGlassMenu::Icon::Search);
        menu.addSeparator();
        QAction* remove = menu.addGlassAction(QStringLiteral("删除此组件"), LiquidGlassMenu::Icon::Remove);
        remove->setEnabled(false);
        QSignalSpy triggered(&menu, &QMenu::triggered);
        menu.popup(backdrop.pos() + QPoint(90, 90));
        QVERIFY(QTest::qWaitForWindowExposed(&menu));
        auto* surface = menu.findChild<QtGlassFlowScene*>();
        QVERIFY(surface);
        QTRY_VERIFY(surface->frameRevision() > 0);
        QCOMPARE(surface->renderScale(), 1.5f);
        QVERIFY(NativeWindows::isGlassWindow(menu.winId()));
        QTest::keyClick(&menu, Qt::Key_Down);
        QCOMPARE(menu.activeAction(), add);
        QTest::keyClick(&menu, Qt::Key_Down);
        QCOMPARE(menu.activeAction(), search);
        QTest::qWait(50);
        QImage frame = surface->grabFramebuffer();
        QVERIFY(!frame.isNull());
        QVERIFY(frame.pixelColor(0, 0).alpha() < 10);
        QVERIFY(frame.pixelColor(frame.width() / 2, frame.height() / 2).alpha() > 245);
        QVERIFY(frame.save(QStringLiteral("artifacts/glass-menu.png")));
        QVERIFY(menu.grab().save(QStringLiteral("artifacts/glass-menu-window.png")));
        QTest::qWait(80);
        QScreen* output = menu.screen();
        QVERIFY(output->grabWindow(0, menu.x() - output->geometry().x(),
            menu.y() - output->geometry().y(), menu.width(), menu.height())
            .save(QStringLiteral("artifacts/glass-menu-screen.png")));
        const quint64 idleRevision = surface->frameRevision();
        QTest::qWait(120);
        QCOMPARE(surface->frameRevision(), idleRevision);
        QTest::mouseClick(&menu, Qt::LeftButton, Qt::NoModifier, menu.actionGeometry(remove).center());
        QCOMPARE(triggered.count(), 0);
        QVERIFY(menu.isVisible());
        menu.setActiveAction(search);
        QTest::keyClick(&menu, Qt::Key_Return);
        QCOMPARE(triggered.count(), 1);
        QCOMPARE(qvariant_cast<QAction*>(triggered.at(0).at(0)), search);
        QVERIFY(!menu.isVisible());
        QVERIFY(!NativeWindows::isGlassWindow(menu.winId()));
        const quint64 revision = surface->frameRevision();
        QTest::qWait(80);
        QCOMPARE(surface->frameRevision(), revision);

        menu.popup(available.bottomRight() - QPoint(1, 1));
        QVERIFY(QTest::qWaitForWindowExposed(&menu));
        QVERIFY2(available.contains(menu.geometry()), qPrintable(QStringLiteral("screen %1,%2 %3x%4 menu %5,%6 %7x%8")
            .arg(available.x()).arg(available.y()).arg(available.width()).arg(available.height())
            .arg(menu.x()).arg(menu.y()).arg(menu.width()).arg(menu.height())));
        QTest::keyClick(&menu, Qt::Key_Escape);
        QVERIFY(!menu.isVisible());
        QCOMPARE(triggered.count(), 1);

        menu.popup(backdrop.pos() + QPoint(90, 90));
        QVERIFY(QTest::qWaitForWindowExposed(&menu));
        QTest::mouseClick(&menu, Qt::LeftButton, Qt::NoModifier, menu.actionGeometry(add).center());
        QCOMPARE(triggered.count(), 2);
        QCOMPARE(qvariant_cast<QAction*>(triggered.at(1).at(0)), add);
        QVERIFY(!menu.isVisible());
        menu.popup(backdrop.pos() + QPoint(90, 90));
        QVERIFY(QTest::qWaitForWindowExposed(&menu));
        QTest::mouseClick(&menu, Qt::LeftButton, Qt::NoModifier, QPoint(-15, -15));
        QVERIFY(!menu.isVisible());
        QCOMPARE(triggered.count(), 2);
    }

    void glassMenuLiveBackdropAndLifetime() {
        AppSettings().setValue("performance/renderScale", 1.5);
        QFont font(BatteryWidget::pingFangFontFamily());
        font.setPixelSize(14);
        ColorWindow backdrop;
        const QPoint origin = QGuiApplication::primaryScreen()->availableGeometry().center() - QPoint(200, 150);
        backdrop.setGeometry(QRect(origin, QSize(400, 300)));
        backdrop.show();
        backdrop.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&backdrop));
        for (bool live : {false, true}) {
            AppSettings().setValue("appearance/liveBackdrop", live);
            backdrop.color = QColor(200, 30, 20);
            backdrop.raise();
            backdrop.activateWindow();
            backdrop.repaint();
            QTest::qWait(200);
            LiquidGlassMenu menu(font, &backdrop);
            menu.addGlassAction(QStringLiteral("添加小组件…"), LiquidGlassMenu::Icon::Add);
            menu.addGlassAction(QStringLiteral("删除此组件"), LiquidGlassMenu::Icon::Remove);
            menu.popup(origin + QPoint(80, 80));
            QVERIFY(QTest::qWaitForWindowExposed(&menu));
            auto* surface = menu.findChild<QtGlassFlowScene*>();
            QVERIFY(surface);
            QTRY_VERIFY(!surface->m_bgImage.isNull());
            QTRY_VERIFY_WITH_TIMEOUT(surface->m_bgImage.pixelColor(surface->m_bgImage.rect().center()).red() > 170, 2000);
            const QImage initial = surface->m_bgImage;
            backdrop.color = QColor(20, 30, 210);
            backdrop.repaint();
            if (live) {
                QTRY_VERIFY_WITH_TIMEOUT(surface->m_bgImage.pixelColor(surface->m_bgImage.rect().center()).blue() > 180, 2500);
                QTest::qWait(200);
                QVERIFY(surface->m_bgImage.pixelColor(surface->m_bgImage.rect().center()).blue() > 180);
            } else {
                QTest::qWait(180);
                QCOMPARE(surface->m_bgImage, initial);
            }
            menu.close();
            QVERIFY(!NativeWindows::isGlassWindow(menu.winId()));
        }
        // Destroy while a worker may still be sampling. Its QObject context
        // and generation must keep callbacks away from a closed/reused menu.
        for (int i = 0; i < 4; ++i) {
            auto* menu = new LiquidGlassMenu(font, &backdrop);
            menu->addGlassAction(QStringLiteral("添加小组件…"), LiquidGlassMenu::Icon::Add);
            menu->popup(origin + QPoint(80, 80));
            QTest::qWait(25);
            const WId id = menu->winId();
            delete menu;
            QVERIFY(!NativeWindows::isGlassWindow(id));
        }
        QTest::qWait(150);
    }

    void contextMenuAddsWidgetsAndCancellationDoesNotOpenDialogs() {
        AppSettings().setValue("appearance/scale", .4);
        for (auto kind : {BatteryWidget::CardKind::Clock, BatteryWidget::CardKind::Weather}) {
            BatteryWidget card(nullptr, kind, false, QStringLiteral("context-menu-test"), 0);
            card.setDesktopCaptureEnabled(false);
            card.show();
            QVERIFY(QTest::qWaitForWindowExposed(&card));
            bool unexpectedDialog = false;
            QTimer dialogGuard;
            dialogGuard.setInterval(20);
            connect(&dialogGuard, &QTimer::timeout, &card, [&]() {
                for (QDialog* dialog : card.findChildren<QDialog*>())
                    if (dialog->isVisible()) {
                        unexpectedDialog = true;
                        dialog->reject();
                    }
            });
            dialogGuard.start();
            for (bool selectAdd : {false, true}) {
                bool foundAdd = false;
                QTimer selection;
                selection.setSingleShot(false);
                connect(&selection, &QTimer::timeout, &card, [&]() {
                    for (QMenu* menu : card.findChildren<QMenu*>()) {
                        if (!menu || !menu->isVisible()) continue;
                        selection.stop();
                        QAction* add = nullptr;
                        for (QAction* action : menu->actions())
                            if (action->text() == QStringLiteral("添加小组件…"))
                                add = action;
                        foundAdd = add != nullptr;
                        if (selectAdd && add) {
                            menu->setActiveAction(add);
                            QTest::keyClick(menu, Qt::Key_Return);
                        } else {
                            menu->close();
                        }
                        break;
                    }
                });
                selection.start(50);
                QContextMenuEvent event(QContextMenuEvent::Mouse, QPoint(20, 20),
                                        card.mapToGlobal(QPoint(20, 20)));
                QApplication::sendEvent(&card, &event);
                QVERIFY(foundAdd);
                QVERIFY(!unexpectedDialog);
                QCOMPARE(bool(card.m_library && card.m_library->isVisible()), selectAdd);
                QVERIFY(!NativeWindows::isTopmost(card.winId()));
            }
            card.m_library->hide();
        }
        BatteryWidget host(nullptr, BatteryWidget::CardKind::Clock, false,
                           QStringLiteral("menu-library-host"), 0);
        BatteryWidget guest(nullptr, BatteryWidget::CardKind::Weather, false,
                            QStringLiteral("menu-library-guest"), 0);
        host.m_primary = true;
        guest.showWidgetLibrary();
        QVERIFY(host.m_library && host.m_library->isVisible());
        QVERIFY(!guest.m_library);
        auto* shared = host.m_library;
        guest.showWidgetLibrary();
        QCOMPARE(host.m_library, shared);
        host.m_library->hide();
    }

    void libraryCancelDoesNotDrop() {
        BatteryWidget::setGlobalFontSmoothing(3);
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
        BatteryWidget::setGlobalFontSmoothing(2);
    }

    void libraryFollowsAppearanceScale() {
        AppSettings().setValue("appearance/scale", 0.7);
        WidgetLibraryDialog library;
        auto* tile = library.findChild<QFrame*>("widgetTile");
        auto* search = library.findChild<QLineEdit*>();
        auto* footer = library.findChild<QFrame*>("footer");
        QVERIFY(tile && search && footer);
        QCOMPARE(tile->size(), QSize(231, 185));
        QCOMPARE(search->height(), 38);
        QCOMPARE(footer->height(), 74);
        QVERIFY(search->styleSheet().contains("border-radius:19px"));
        QVERIFY(library.styleSheet().contains("border-radius:19px"));
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        QCOMPARE(library.width(), qMin(1386, screen->availableGeometry().width() - 32));
        QCOMPARE(library.height(), qMin(840, screen->availableGeometry().height() - 32));
        library.prepareBackdrop();
        library.show();
        QVERIFY(QTest::qWaitForWindowExposed(&library));
        QTest::qWait(350);
        const QImage gallery = library.grab().toImage().convertToFormat(QImage::Format_ARGB32);
        gallery.save("artifacts/gallery-small-scale.png");
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                QCOMPARE(qAlpha(gallery.pixel(x, y)), 0);
                QCOMPARE(qAlpha(gallery.pixel(gallery.width() - 1 - x, y)), 0);
                QCOMPARE(qAlpha(gallery.pixel(x, gallery.height() - 1 - y)), 0);
                QCOMPARE(qAlpha(gallery.pixel(gallery.width() - 1 - x, gallery.height() - 1 - y)), 0);
            }
        }
        library.hide();

        library.setInterfaceScale(1.5);
        QCOMPARE(tile->size(), QSize(495, 396));
        QCOMPARE(search->height(), 81);
        QCOMPARE(footer->height(), 158);
        QCOMPARE(library.m_uiScale, 1.5);
        library.setInterfaceScale(0.4);
        QCOMPARE(tile->size(), QSize(132, 106));
        QCOMPARE(search->height(), 22);
        QCOMPARE(footer->height(), 42);
        QVERIFY(search->styleSheet().contains("border-radius:11px"));

        BatteryWidget owner(nullptr, BatteryWidget::CardKind::Clock, false,
                            QStringLiteral("scale-library-owner"), 0);
        owner.m_primary = true;
        owner.showWidgetLibrary();
        QVERIFY(owner.m_library);
        auto* shared = owner.m_library;
        QCOMPARE(shared->m_uiScale, 0.7);
        owner.applyScale(1.25);
        QCOMPARE(owner.m_library, shared);
        QCOMPARE(shared->m_uiScale, 1.25);
        owner.syncGroupSettings(1.25, 72, 0.80, false, false, false,
                                1.25, 2);
        QCOMPARE(shared->materialBlurStrength(), 72);
        QCOMPARE(shared->glassOpacity(), 0.80);
        QCOMPARE(shared->renderScale(), 1.25f);
        QCOMPARE(shared->findChild<QFrame*>("widgetTile")->size(), QSize(413, 330));
        QVERIFY2(shared->styleSheet().contains("border-radius:34px"),
                 qPrintable(shared->styleSheet()));
        QVERIFY2(shared->findChild<QLineEdit*>()->styleSheet().contains("border-radius:34px"),
                 qPrintable(shared->findChild<QLineEdit*>()->styleSheet()));
        QTest::qWait(350);
        QCOMPARE(shared->width(), qMin(2475, screen->availableGeometry().width() - 32));
        auto* scroll = shared->findChild<QScrollArea*>();
        QVERIFY(scroll);
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        shared->grab().save("artifacts/gallery-scaled.png");
        shared->hide();
    }

    void hiddenConfigurationRoundTrip() {
        AppSettings().setValue("appearance/scale", 0.4);
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

    void partiallyOffscreenSavedCardIsRestoredWithinScreen() {
        AppSettings().setValue("appearance/scale", 0.4);
        QScreen* screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QRect area = screen->availableGeometry();
        const QJsonArray widgets{QJsonObject{
            {"id", "partially-offscreen"}, {"type", "clock"}, {"variant", 0},
            {"x", area.right() - 10}, {"y", area.bottom() - 10},
            {"visible", false}}};
        QFile file(AppSettings::filePath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        root.insert("widgets", widgets);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(root).toJson());
        file.close();

        const auto restored = BatteryWidget::restoreWidgets();
        QCOMPARE(restored.size(), 1);
        QVERIFY(area.contains(restored.first()->geometry()));
        QVERIFY(!restored.first()->isVisible());
        qDeleteAll(restored);
    }

    void opacityAndContextRecreation() {
        TestGlass glass;
        glass.setDesktopCaptureEnabled(false);
        glass.setRenderScale(1.5f);
        QCOMPARE(glass.renderScale(), 1.5f);
        glass.setRenderScale(2.0f);
        QCOMPARE(glass.renderScale(), 1.5f);
        glass.setRenderScale(1.25f);
        QImage background(200, 140, QImage::Format_RGB32);
        background.fill(QColor(160, 80, 30));
        glass.setBackgroundImage(background);
        glass.setGlassOpacity(0.5);
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        QTRY_VERIFY(glass.isValid());
        QColor pixel = centerPixel(glass);
        QVERIFY2(qAbs(pixel.alpha() - 128) <= 3, qPrintable(QString::number(pixel.alpha())));
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

    void gpuRoundedEdgeHasAnalyticAntialiasing() {
        TestGlass glass;
        glass.setDesktopCaptureEnabled(false);
        glass.setRenderScale(1.5f);
        QImage background(400, 280, QImage::Format_RGB32);
        background.fill(QColor(180, 90, 35));
        glass.setBackgroundImage(background);
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        QTRY_VERIFY_WITH_TIMEOUT(glass.frameRevision() > 0, 1000);
        const QImage frame = glass.grabFramebuffer().convertToFormat(QImage::Format_ARGB32);
        QVERIFY(!frame.isNull());
        int partialAlpha = 0;
        for (int y = 0; y < frame.height(); ++y) {
            for (int x = 0; x < frame.width(); ++x) {
                const int alpha = qAlpha(frame.pixel(x, y));
                partialAlpha += alpha > 0 && alpha < 255;
            }
        }
        QVERIFY2(partialAlpha > 0, "rounded GPU silhouette contains no antialiased coverage pixels");
    }

    void materialBlurStrengthSoftensBackdrop() {
        TestGlass glass;
        glass.setFixedSize(384, 256);
        glass.setDesktopCaptureEnabled(false);
        glass.setGlassOpacity(1);
        QImage source(384, 256, QImage::Format_RGB32);
        source.fill(Qt::black);
        QPainter sourcePainter(&source);
        sourcePainter.fillRect(QRect(192, 0, 192, 256), Qt::white);
        sourcePainter.end();
        glass.setBackgroundImage(source);
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        for (float quality : {.5f, 1.f, 1.5f}) {
            glass.setRenderScale(quality);
            int widths[3] = {};
            int index = 0;
            for (int strength : {0, 50, 100}) {
                glass.setMaterialBlurStrength(strength);
                glass.setRefractionPower(0); // Measure blur independently of lens distortion.
                QTest::qWait(60);
                const QImage frame = glass.grabFramebuffer();
                QVERIFY(frame.save(QStringLiteral("artifacts/material-blur-%1-%2.png")
                                       .arg(quality).arg(strength)));
                const int y = frame.height() / 2;
                for (int x = frame.width() / 4; x < frame.width() * 3 / 4; ++x) {
                    const int red = qRed(frame.pixel(x, y));
                    widths[index] += red > 30 && red < 220;
                }
                ++index;
            }
            qInfo() << "Material blur quality / transition widths" << quality
                    << widths[0] << widths[1] << widths[2];
            QVERIFY2(widths[1] > widths[0] + 8, "Default material has no measurable blur");
            QVERIFY2(widths[2] > widths[1] + 8, "Blur strength does not broaden the transition");
        }
    }

    void gpuHighQualityBlurHasNoSamplingLattice() {
        TestGlass glass;
        glass.setFixedSize(384, 256);
        glass.setDesktopCaptureEnabled(false);
        glass.setBlurRadius(8);
        glass.setBlurIterations(1);
        QImage source(384, 256, QImage::Format_RGB32);
        source.fill(QColor(20, 30, 50));
        QPainter painter(&source);
        painter.fillRect(QRect(180, 116, 24, 24), Qt::white);
        painter.end();
        glass.setBackgroundImage(source);
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        for (const float quality : {.5f, .75f, 1.f, 1.25f, 1.5f}) {
            glass.setRenderScale(quality);
            glass.setBackgroundImage(source);
            QTest::qWait(60);
            const QImage frame = glass.grabFramebuffer();
            QVERIFY(frame.save(QStringLiteral("artifacts/gpu-blur-%1.png").arg(quality)));
            if (quality < 1.25f)
                continue;
            const int cy = frame.height() / 2;
            const int cx = frame.width() / 2;
            int largestRise = 0;
            for (int offset = 2; offset < qRound(75 * glass.devicePixelRatioF()); ++offset) {
                const int before = qRed(frame.pixel(cx + offset - 1, cy));
                const int after = qRed(frame.pixel(cx + offset, cy));
                largestRise = qMax(largestRise, after - before);
            }
            qInfo() << "Blur quality" << quality << "largest secondary peak rise" << largestRise;
            QVERIFY2(largestRise <= 1, "Highlight has repeated sampling peaks/grid lines");
        }
    }

    void supersampledWallpaperUsesSmoothInterpolation() {
        TestGlass glass;
        QScreen *screen = QGuiApplication::primaryScreen();
        glass.m_wallpaperScreenName = screen->name();
        glass.m_wallpaperCanvas = QImage(screen->size(), QImage::Format_RGB32);
        for (int y = 0; y < glass.m_wallpaperCanvas.height(); ++y) {
            auto *row = reinterpret_cast<QRgb *>(glass.m_wallpaperCanvas.scanLine(y));
            for (int x = 0; x < glass.m_wallpaperCanvas.width(); ++x)
                row[x] = x % 2 ? qRgb(255, 255, 255) : qRgb(0, 0, 0);
        }
        glass.m_wallpaperRefreshClock.start();
        const QImage crop = glass.wallpaperBackdrop(
            QRect(screen->geometry().topLeft(), QSize(16, 16)), 2.25);
        int intermediate = 0;
        for (int y = 0; y < crop.height(); ++y)
            for (int x = 0; x < crop.width(); ++x) {
                const int red = qRed(crop.pixel(x, y));
                intermediate += red > 0 && red < 255;
            }
        QVERIFY2(intermediate > crop.width() * crop.height() / 2,
                 "Wallpaper enlargement produced nearest-neighbour pixel blocks");
    }

    void actualWeatherHighestQuality() {
        AppSettings().setValue("appearance/scale", .7);
        AppSettings().setValue("performance/renderScale", 1.5);
        BatteryWidget card(nullptr, BatteryWidget::CardKind::Weather, false,
                           QStringLiteral("weather-quality"), 1);
        card.setDesktopLayerEnabled(false);
        card.setDesktopCaptureEnabled(false);
        card.setAnimationEnabled(false);
        card.m_weatherLocation = QStringLiteral("长沙");
        card.m_weatherTemperature = QStringLiteral("28.4");
        card.m_weatherDescription = QStringLiteral("多云");
        card.m_weatherHigh = QStringLiteral("32");
        card.m_weatherLow = QStringLiteral("23");
        card.m_weatherCode = 1;
        card.m_weatherNight = true;
        for (int i = 0; i < 6; ++i)
            card.m_weatherHours.append({QString::number(11 + i * 3) + QStringLiteral("时"),
                QString::number(31 - i), QStringLiteral("多云"), 1});
        QImage backdrop(card.size() * card.devicePixelRatioF(), QImage::Format_RGB32);
        backdrop.fill(QColor(24, 37, 67));
        QPainter source(&backdrop);
        source.fillRect(QRect(90, backdrop.height() - 45, 24, 24), QColor(255, 226, 153));
        source.fillRect(QRect(backdrop.width() / 2, backdrop.height() - 80, 18, 60), Qt::white);
        source.end();
        card.setBackgroundImage(backdrop);
        card.show();
        QVERIFY(QTest::qWaitForWindowExposed(&card));
        int edgePixels[2] = {};
        for (int index = 0; index < 2; ++index) {
            const int level = index ? 3 : 0;
            BatteryWidget::setGlobalFontSmoothing(level);
            card.update();
            QTest::qWait(70);
            const QImage frame = card.grabFramebuffer();
            QVERIFY(frame.save(QStringLiteral("artifacts/actual-weather-aa-%1.png")
                               .arg(level)));
            // The temperature occupies this normalized region of the actual
            // wide card. The backdrop is dark here; intermediate red values
            // measure coverage at digit edges, not the bright glyph interior.
            for (int y = qRound(frame.height() * .24); y < qRound(frame.height() * .45); ++y)
                for (int x = qRound(frame.width() * .035); x < qRound(frame.width() * .37); ++x) {
                    const int red = qRed(frame.pixel(x, y));
                    edgePixels[index] += red > 100 && red < 220;
                }
        }
        qInfo() << "Actual weather digit edge coverage"
                << edgePixels[0] << edgePixels[1];
        QVERIFY2(edgePixels[1] > edgePixels[0] + 100,
                 "Highest AA did not smooth the actual 28.4 temperature");
        qInfo() << "Weather device scale" << card.devicePixelRatioF()
                << "quality" << card.renderScale();
        BatteryWidget::setGlobalFontSmoothing(2);
    }

    void highestTextAntialiasingWorksInOpenGL() {
        TextQualityGlass glass;
        glass.setFixedSize(480, 160);
        glass.setDesktopCaptureEnabled(false);
        glass.setGlassRenderingEnabled(false);
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        int partial[2] = {};
        for (int index = 0; index < 2; ++index) {
            const int level = index ? 3 : 0;
            glass.quality = level;
            BatteryWidget::setGlobalFontSmoothing(level);
            glass.update();
            QTest::qWait(70);
            const QImage frame = glass.grabFramebuffer();
            QVERIFY(frame.save(QStringLiteral("artifacts/text-aa-%1.png").arg(level)));
            for (int y = 0; y < frame.height(); ++y)
                for (int x = 0; x < frame.width(); ++x) {
                    const int alpha = qAlpha(frame.pixel(x, y));
                    partial[index] += alpha > 0 && alpha < 255;
                }
        }
        qInfo() << "Native GL text partial coverage:" << partial[0] << partial[1];
        QVERIFY(partial[1] > partial[0] + 100);
        // Backdrop quality must not change the text's final device coverage.
        const QImage nativeText = glass.grabFramebuffer();
        glass.setRenderScale(.5f);
        QTest::qWait(60);
        QCOMPARE(glass.grabFramebuffer(), nativeText);
        BatteryWidget::setGlobalFontSmoothing(2);
    }

    void backgroundReusePreservesOrientation() {
        TestGlass glass;
        glass.setFixedSize(317, 189);
        glass.setDesktopCaptureEnabled(false);
        glass.setRenderScale(1.5f);
        QImage source(479, 287, QImage::Format_RGB32);
        for (int y = 0; y < source.height(); ++y) {
            auto *row = reinterpret_cast<QRgb *>(source.scanLine(y));
            for (int x = 0; x < source.width(); ++x)
                row[x] = qRgb(x * 255 / source.width(), y * 255 / source.height(),
                              (x + y) % 256);
        }
        glass.setBackgroundImage(source);
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        const QImage normal = glass.grabFramebuffer();
        QVERIFY(!glass.m_bgDirty);
        QVERIFY(!glass.m_blurCacheDirty);
        for (int i = 0; i < 100; ++i)
            glass.setBackgroundImage(source);
        QVERIFY2(!glass.m_bgDirty && !glass.m_blurCacheDirty,
                 "Identical image submissions invalidated upload/blur caches");
        const auto compareFrame = [&normal](const QImage &frame) {
            if (frame.size() != normal.size())
                return 256;
            int error = 0;
            for (int y = 0; y < frame.height(); ++y)
                for (int x = 0; x < frame.width(); ++x) {
                    const QRgb a = frame.pixel(x, y), b = normal.pixel(x, y);
                    error = qMax(error, qAbs(qRed(a) - qRed(b)));
                    error = qMax(error, qAbs(qGreen(a) - qGreen(b)));
                    error = qMax(error, qAbs(qBlue(a) - qBlue(b)));
                    error = qMax(error, qAbs(qAlpha(a) - qAlpha(b)));
                }
            return error;
        };
        glass.setBackgroundImageFlipped(source.flipped(Qt::Vertical));
        QVERIFY2(compareFrame(glass.grabFramebuffer()) <= 1,
                 "Direct GPU upload changed background orientation or coverage");
        glass.setBackgroundImage(source);
        QVERIFY2(compareFrame(glass.grabFramebuffer()) <= 1,
                 "Restoring the unflipped image changed the frame");
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
        // Let any capture queued just before the cover reached the compositor
        // finish before taking the suspended-rendering baseline.
        QTest::qWait(500);
        const quint64 revision = glass.frameRevision();
        for (int i = 0; i < 5; ++i) {
            cover.color = QColor(20 + i * 20, 80, 180);
            cover.update();
            QTest::qWait(60);
        }
        QCOMPARE(glass.frameRevision(), revision);
    }

    void cancellingCitySearchHandlesSynchronousFinished() {
        BatteryWidget weather(nullptr, BatteryWidget::CardKind::Weather, false);
        delete weather.m_weatherNetwork;
        auto* network = new PendingNetwork(&weather);
        weather.m_weatherNetwork = network;
        bool issued = false;
        QTimer::singleShot(0, &weather, [&]() {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            if (auto* edit = dialog->findChild<QLineEdit*>()) {
                edit->setText(QStringLiteral("海淀"));
                QMetaObject::invokeMethod(edit, "returnPressed", Qt::DirectConnection);
                issued = bool(network->pending);
            }
            dialog->reject();
        });
        weather.searchWeatherCity();
        QVERIFY(issued);
        QTRY_VERIFY(network->pending.isNull());
    }

    void sharedSettingsCanSearchWeatherFromBatteryCard() {
        BatteryWidget battery(nullptr, BatteryWidget::CardKind::Battery, false);
        bool openedSearch = false;
        QTimer::singleShot(0, &battery, [&]() {
            auto* settingsDialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!settingsDialog)
                return;
            QPushButton* searchButton = nullptr;
            for (QPushButton* button : settingsDialog->findChildren<QPushButton*>()) {
                if (button->text() == QStringLiteral("搜索区县…")) {
                    searchButton = button;
                    break;
                }
            }
            if (!searchButton) {
                settingsDialog->reject();
                return;
            }
            QTimer::singleShot(0, &battery, [&, settingsDialog]() {
                auto* searchDialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                openedSearch = searchDialog && searchDialog != settingsDialog
                    && searchDialog->windowTitle() == QStringLiteral("搜索城市或区县");
                if (searchDialog && searchDialog != settingsDialog)
                    searchDialog->reject();
                settingsDialog->reject();
            });
            searchButton->click();
        });
        battery.showSettingsDialog();
        QVERIFY(openedSearch);
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
