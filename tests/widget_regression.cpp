#include "rendering/desktopcapture.h"
#include "rendering/liquidglasswidget.h"
#include "rendering/nativewindows.h"
#include "rendering/responsivelayout.h"
#include "rendering/renderdiagnostics.h"
#include "ui/widgetlibrarydialog.h"
#include "ui/firstrunwizard.h"
#include "ui/scaleselector.h"
#include "widgets/musiccard.h"
#include "media/systemmediasession.h"
#include "rendering/cleartypetext.h"
#include "app/appsettings.h"
#include "ui/liquidglassmenu.h"
#include "widgets/desktopwidget.h"
#include "widgets/widgetgeometry.h"
#include "widgets/cardmetrics.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QDialog>
#include <QLineEdit>
#include <QFrame>
#include <QGridLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLabel>
#include <QMouseEvent>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QPainter>
#include <QOpenGLExtraFunctions>
#include <QPushButton>
#include <QProcess>
#include <QScreen>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
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
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#include <systemmediatransportcontrolsinterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
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
    int paintedFrames = 0;
    ColorWindow() {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    }
    void paintEvent(QPaintEvent*) override {
        ++paintedFrames;
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
        QFont font(DesktopWidget::pingFangFontFamily(), 52, QFont::Normal);
        DesktopWidget::applyFontSmoothing(font);
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

class ClearTypeGlass : public TestGlass {
public:
    void paintOverlay(QPainter& painter) override {
        painter.fillRect(rect(), QColor(32, 32, 32));
        QFont font(DesktopWidget::pingFangFontFamily());
        font.setPixelSize(25);
        DesktopWidget::applyFontSmoothing(font);
        painter.setFont(font);
        painter.setPen(Qt::white);
        ResponsiveLayout::drawSingleLine(painter, QRectF(10, 20, 180, 40),
            Qt::AlignCenter, QStringLiteral("音乐 Aa 123"));
        painter.fillRect(QRectF(50, 95, 80, 12), Qt::red);
    }
};

// 固定内容用于跨版本逐像素比较，避免系统捕获、网络和时钟影响测量。
class PipelineProbe : public QtGlassFlowScene {
public:
    bool withText = false;
    void paintOverlay(QPainter& painter) override {
        if (!withText)
            return;
        QFont font(DesktopWidget::pingFangFontFamily());
        font.setPixelSize(22);
        DesktopWidget::applyFontSmoothing(font);
        painter.setFont(font);
        for (int row = 0; row < 8; ++row) {
            painter.setPen(QColor(245 - row * 17, 150 + row * 10, 220, 255 - row * 18));
            painter.setOpacity(1.0 - row * .07);
            ResponsiveLayout::drawSingleLine(painter, QRectF(20, 12 + row * 32, 330, 30),
                Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("清晰音乐 Aa 123 — %1").arg(row));
        }
        painter.setOpacity(1);
        painter.fillRect(QRectF(30, 280, 200, 8), Qt::red);
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

// 材质规范的颜色响应用于隔离空间折射，零折射测试仍单独检查原始高斯采样。
static QColor expectedGlassColor(const QColor& pixel)
{
    const double luminance = pixel.red()*.2126 + pixel.green()*.7152 + pixel.blue()*.0722;
    return QColor(qBound(0, qRound(pixel.red()*.94 - luminance*.35 + 255*.129), 255),
                  qBound(0, qRound(pixel.green()*.94 - luminance*.35 + 255*.147), 255),
                  qBound(0, qRound(pixel.blue()*.94 - luminance*.35 + 255*.159), 255));
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
    void opticalGlassKeepsCenterStableAndBendsOnlyRim() {
        PipelineProbe scene;
        scene.setNoiseAmount(0);
        scene.show();
        QVERIFY(QTest::qWaitForWindowExposed(&scene));
        for (QSize size : {QSize(240, 160), QSize(672, 328), QSize(360, 440)}) {
            scene.resize(size);
            scene.m_objects.clear();
            scene.addGlassObject(QPointF(), size);
            scene.setGlassObjectCornerRadius(0, qMin(size.width(), size.height()) * .16);
            const qreal dpr = scene.devicePixelRatioF();
            QImage background(size * dpr, QImage::Format_RGB32);
            for (int y = 0; y < background.height(); ++y)
                for (int x = 0; x < background.width(); ++x)
                    background.setPixel(x, y, qRgb(30 + 200 * x / background.width(),
                        30 + 200 * y / background.height(), (int(x / dpr) / 8) % 2 ? 220 : 40));
            scene.setBackgroundImage(background);
            for (float quality : {.6f, .8f, 1.f, 1.25f, 1.5f}) {
                scene.setRenderScale(quality);
                scene.setRefractionPower(0);
                scene.setBlurRadius(0);
                const QImage clear = scene.grabFramebuffer();
                scene.setBlurRadius(LiquidGlassStyle::kBlurRadius);
                const QImage frosted = scene.grabFramebuffer();
                scene.setRefractionPower(LiquidGlassStyle::kRefractionPower);
                const QImage optical = scene.grabFramebuffer();
                const int cx = optical.width() / 2, cy = optical.height() / 2;
                const int rimX = qRound(qMin(optical.width(), optical.height()) * .02);
                const int rimY = rimX;
                // 红绿通道编码背景坐标，检测边缘向内折射和中心位置保持。
                QVERIFY(optical.pixelColor(rimX, cy).red() > expectedGlassColor(clear.pixelColor(rimX, cy)).red() + 7);
                QVERIFY(optical.pixelColor(cx, rimY).green() > expectedGlassColor(clear.pixelColor(cx, rimY)).green() + 7);
                QVERIFY(qAbs(optical.pixelColor(cx, cy).red() - expectedGlassColor(clear.pixelColor(cx, cy)).red()) <= 2);
                QVERIFY(qAbs(optical.pixelColor(cx, cy).green() - expectedGlassColor(clear.pixelColor(cx, cy)).green()) <= 2);
                qint64 centerDelta = 0, frostedError = 0;
                for (int y = cy - optical.height() / 10; y < cy + optical.height() / 10; ++y)
                    for (int x = cx - optical.width() / 10; x < cx + optical.width() / 10; ++x) {
                        const int detail = qBlue(clear.pixel(x, y));
                        centerDelta += qAbs(qBlue(optical.pixel(x, y)) - expectedGlassColor(frosted.pixelColor(x, y)).blue());
                        frostedError += qAbs(qBlue(frosted.pixel(x, y)) - detail);
                    }
                QVERIFY2(frostedError > 100, "The glass did not soften the background");
                QVERIFY2(centerDelta < frostedError * .05,
                         "Refraction spread into the flat center of the glass");
                QVERIFY(optical.save(QStringLiteral("artifacts/optical-glass-%1x%2-%3.png")
                    .arg(size.width()).arg(size.height()).arg(quality)));
            }
        }
    }

    void specializedBlurMatchesFallbackAndBoundsCache() {
        PipelineProbe scene;
        scene.resize(240, 180);
        scene.setNoiseAmount(0);
        scene.addGlassObject(QPointF(0, 0), QSizeF(240, 180));
        scene.setGlassObjectCornerRadius(0, 24);
        QImage pattern(360, 270, QImage::Format_RGB32);
        for (int y = 0; y < pattern.height(); ++y)
            for (int x = 0; x < pattern.width(); ++x)
                pattern.setPixel(x, y, qRgb((x * 13) % 256, (y * 7) % 256, (x * y) % 256));
        scene.setBackgroundImage(pattern);
        scene.show();
        QVERIFY(QTest::qWaitForWindowExposed(&scene));
        for (float quality : {.6f, .8f, 1.f, 1.25f, 1.5f}) {
            scene.setRenderScale(quality);
            for (float radius : {0.f, 2.f, 8.f, 30.f}) {
                scene.setBlurRadius(radius);
                const QImage specialized = scene.grabFramebuffer();
                const int count = int(scene.m_blurKernelSamples.size());
                QCOMPARE(bool(scene.m_blurPrograms.object(count)), count >= 16);
                QVERIFY(scene.m_blurPrograms.size() <= 4);
                scene.makeCurrent();
                scene.m_blurPrograms.remove(count);
                scene.m_uploadedKernelProgram = nullptr;
                // 模拟驱动拒绝变体编译，确认完整高斯采样的回退路径保持画面。
                scene.m_failedBlurPrograms.insert(count);
                scene.m_blurCacheDirty = true;
                scene.doneCurrent();
                const QImage fallback = scene.grabFramebuffer();
                QCOMPARE(specialized, fallback);
                QVERIFY(!scene.m_blurPrograms.object(count));
                scene.m_failedBlurPrograms.remove(count);
            }
        }
        scene.hide();
        scene.makeCurrent();
        scene.cleanupGL();
        QVERIFY(scene.m_blurPrograms.isEmpty());
        QVERIFY(scene.m_failedBlurPrograms.isEmpty());
    }

    void largePanelRestrainsRefractionAndKeepsPreviewCardsReadable() {
        PipelineProbe scene;
        scene.resize(1000, 700);
        scene.setNoiseAmount(0);
        scene.setBlurRadius(0);
        scene.addGlassObject(QPointF(), scene.size());
        scene.setGlassObjectCornerRadius(0, 26);
        QImage background(1500, 1050, QImage::Format_RGB32);
        for (int y = 0; y < background.height(); ++y)
            for (int x = 0; x < background.width(); ++x)
                background.setPixel(x, y, qRgb(30 + 200*x/background.width(),
                                               30 + 200*y/background.height(), 80));
        scene.setBackgroundImage(background);
        scene.show();
        QVERIFY(QTest::qWaitForWindowExposed(&scene));
        for (float quality : {.6f, 1.f, 1.5f}) {
            scene.setRenderScale(quality);
            scene.setRefractionPower(0);
            const QImage clear = scene.grabFramebuffer();
            scene.setRefractionPower(LiquidGlassStyle::kRefractionPower);
            const QImage glass = scene.grabFramebuffer();
            const int x = qRound(8 * scene.devicePixelRatioF());
            const int cx = glass.width()/2, cy = glass.height()/2;
            // 大面板边缘只允许小幅折射，不把背景压缩成大面积的波纹。
            QVERIFY(qAbs(glass.pixelColor(x, cy).red() - expectedGlassColor(clear.pixelColor(x, cy)).red()) <= 5);
            QVERIFY(qAbs(glass.pixelColor(cx, x).green() - expectedGlassColor(clear.pixelColor(cx, x)).green()) <= 5);
        }
        for (auto kind : {DesktopWidget::CardKind::Battery, DesktopWidget::CardKind::Weather,
                          DesktopWidget::CardKind::Clock, DesktopWidget::CardKind::Dictionary}) {
            const QImage preview = DesktopWidget::renderPreview(kind, 0, QSize(204, 128), 1.5);
            QCOMPARE(preview.pixelColor(0, 0).alpha(), 0);
            const QColor surface = preview.pixelColor(preview.width()/2, qRound(4 * 1.5));
            QVERIFY2(surface.alpha() >= 240, "The gallery preview lost its independent card surface");
        }
    }

    void renderingPipelineParityAndTiming() {
        const QString output = qEnvironmentVariable("WIDGET_PIPELINE_OUTPUT");
        if (output.isEmpty())
            QSKIP("Set WIDGET_PIPELINE_OUTPUT to capture a rendering baseline or comparison");
        QVERIFY(QDir().mkpath(output));
        PipelineProbe scene;
        scene.resize(400, 310);
        scene.setNoiseAmount(0);
        scene.setBlurRadius(8);
        scene.addGlassObject(QPointF(0, 0), QSizeF(400, 310));
        scene.setGlassObjectCornerRadius(0, 34);
        QImage background(600, 465, QImage::Format_RGB32);
        for (int y = 0; y < background.height(); ++y)
            for (int x = 0; x < background.width(); ++x)
                background.setPixel(x, y, qRgb((x * 7 + y) % 256, (y * 3) % 256, (x + y * 5) % 256));
        scene.setBackgroundImage(background);
        scene.show();
        QVERIFY(QTest::qWaitForWindowExposed(&scene));
        for (float quality : {.6f, .8f, 1.f, 1.25f, 1.5f}) {
            scene.setRenderScale(quality);
            for (int mode = 0; mode < 3; ++mode) {
                scene.withText = mode == 2;
                const QString name = QStringLiteral("pipeline-%1-%2").arg(quality).arg(mode);
                scene.makeCurrent();
                auto* gl = scene.context()->extraFunctions();
                QList<double> durations;
                // 每次完成 GPU 工作再计时；模式一强制背景变化，其他模式复用模糊缓存。
                for (int frame = 0; frame < 58; ++frame) {
                    if (mode == 1)
                        scene.m_blurCacheDirty = true;
                    QElapsedTimer timer;
                    timer.start();
                    scene.paintGL();
                    gl->glFinish();
                    if (frame >= 10)
                        durations.append(timer.nsecsElapsed() / 1e6);
                }
                scene.doneCurrent();
                std::sort(durations.begin(), durations.end());
                qInfo() << name << "DPR" << scene.devicePixelRatioF()
                        << "median_ms" << durations[durations.size() / 2]
                        << "p95_ms" << durations[qFloor(durations.size() * .95)];
                const QImage frame = scene.grabFramebuffer().convertToFormat(QImage::Format_RGBA8888);
                QVERIFY(frame.save(output + '/' + name + ".png"));
                const QString baselinePath = qEnvironmentVariable("WIDGET_PIPELINE_BASELINE");
                if (!baselinePath.isEmpty()) {
                    const QImage baseline = QImage(baselinePath + '/' + name + ".png")
                        .convertToFormat(QImage::Format_RGBA8888);
                    QCOMPARE(frame.size(), baseline.size());
                    int maxDelta = 0;
                    qint64 totalDelta = 0;
                    for (int y = 0; y < frame.height(); ++y)
                        for (int x = 0; x < frame.width() * 4; ++x) {
                            const int delta = qAbs(int(frame.constScanLine(y)[x]) - baseline.constScanLine(y)[x]);
                            maxDelta = qMax(maxDelta, delta);
                            totalDelta += delta;
                        }
                    qInfo() << name << "max_channel_delta" << maxDelta
                            << "mean_channel_delta" << totalDelta / double(frame.sizeInBytes());
                    // 保留原有字体混合顺序，所有路径均要求逐像素一致。
                    QVERIFY2(maxDelta == 0, qPrintable(name));
                }
            }
        }
    }

    void initTestCase() {
        QCoreApplication::setOrganizationName("macdowsOS-regression");
        QCoreApplication::setApplicationName("widget-regression");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(!DesktopWidget::pingFangFontFamily().isEmpty());
        // 回归测试不访问真实的天气或词典服务。
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", 9));
        QDir().mkpath("artifacts");
    }

    void init() {
        // 每个测试独立清理显示与交互设置，避免鼠标穿透或实时模式影响后续用例。
        QSettings().clear();
        QFile::remove(AppSettings::filePath());
        DesktopWidget::setGlobalFontSmoothing(4);
        QFile::remove(AppSettings::filePath());
    }

    void clearTypeUsesRgbCoverageAndRestoresGlPainter() {
        DesktopWidget::setGlobalFontSmoothing(4);
        for (qreal scale : {1.0, 1.25, 1.5, 1.75, 2.0}) {
            QFont font(DesktopWidget::pingFangFontFamily());
            font.setPixelSize(qRound(19 * scale));
            DesktopWidget::applyFontSmoothing(font);
            const QImage mask = ClearTypeText::coverage(font, QStringLiteral("清晰 Aa 123"));
            QVERIFY(!mask.isNull());
            int coloredPixels = 0;
            for (int y = 0; y < mask.height(); ++y)
                for (int x = 0; x < mask.width(); ++x) {
                    const QColor pixel = mask.pixelColor(x, y);
                    if (pixel.red() != pixel.green() || pixel.green() != pixel.blue())
                        ++coloredPixels;
                }
            QVERIFY2(coloredPixels > 20, "ClearType mask unexpectedly contains only grayscale coverage");
        }
        ClearTypeGlass glass;
        glass.setGlassRenderingEnabled(false);
        glass.setDesktopCaptureEnabled(false);
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        const QImage clear = glass.grabFramebuffer();
        const qreal dpr = glass.devicePixelRatioF();
        QCOMPARE(clear.pixelColor(qRound(60*dpr), qRound(100*dpr)), QColor(Qt::red));
        int coloredPixels = 0;
        for (int y = qRound(20*dpr); y < qRound(60*dpr); ++y)
            for (int x = 0; x < clear.width(); ++x) {
                const QColor pixel = clear.pixelColor(x,y);
                if (pixel.red() != pixel.green() || pixel.green() != pixel.blue())
                    ++coloredPixels;
            }
        QVERIFY(coloredPixels > 20);
        DesktopWidget::setGlobalFontSmoothing(3);
        const QImage gray = glass.grabFramebuffer();
        QVERIFY(clear != gray);
        QVERIFY(clear.save("artifacts/cleartype-native.png"));
        QVERIFY(gray.save("artifacts/grayscale-native.png"));
    }

    void previewDoesNotStartServicesOrCreateConfiguration() {
        for (auto kind : {DesktopWidget::CardKind::Battery, DesktopWidget::CardKind::Weather,
                          DesktopWidget::CardKind::Clock, DesktopWidget::CardKind::Dictionary}) {
            const QImage image = DesktopWidget::renderPreview(kind, 0, QSize(328,328), 1.5);
            QVERIFY(!image.isNull());
            QVERIFY(!QFile::exists(AppSettings::filePath()));
        }
    }

    void scalePreviewAnimatesWithoutBlockingControls() {
        FirstRunWizard wizard;
        wizard.setDesktopCaptureEnabled(false);
        wizard.show();
        QVERIFY(QTest::qWaitForWindowExposed(&wizard));
        QTest::qWait(450);
        wizard.findChild<QPushButton*>("continueButton")->click();
        auto* actual = wizard.findChild<QWidget*>("actualSizePreview");
        auto* slider = wizard.findChild<QSlider*>("scaleSlider");
        QVERIFY(actual && slider);
        QTRY_VERIFY(actual->isVisible());
        slider->setValue(40);
        QTRY_COMPARE(actual->width(), 131);
        slider->setValue(200);
        QTest::qWait(35);
        QVERIFY(actual->width() > 131 && actual->width() < 656);
        QTRY_COMPARE_WITH_TIMEOUT(actual->width(), 656, 1000);
        const QRect controls = wizard.frameGeometry().translated(-actual->pos());
        QVERIFY(!actual->mask().intersects(controls));
        const QSize stableSurface = wizard.size();
        QVector<qint64> intervals;
        QElapsedTimer clock;
        clock.start();
        QTimer driver;
        driver.setTimerType(Qt::PreciseTimer);
        driver.setInterval(16);
        int step = 0;
        connect(&driver, &QTimer::timeout, &wizard, [&] {
            intervals.append(clock.restart());
            slider->setValue(40 + (step++ * 7) % 161);
        });
        driver.start();
        QTest::qWait(1200);
        driver.stop();
        QCOMPARE(wizard.size(), stableSurface);
        QVERIFY(intervals.size() > 30);
        std::sort(intervals.begin(), intervals.end());
        const qint64 p95 = intervals.at(intervals.size() * 95 / 100);
        qInfo() << "连续缩放界面定时器 P95 间隔:" << p95 << "ms，DPR:" << wizard.devicePixelRatioF();
        QVERIFY2(p95 < 50, "Continuous scaling blocks the UI event loop");
        QVERIFY(!QFile::exists(AppSettings::filePath()));
    }

    void settingsAndWizardShareScaleSelector() {
        DesktopWidget widget(nullptr, DesktopWidget::CardKind::Clock, false);
        bool checked = false;
        bool effectOptionsRemoved = false;
        QTimer::singleShot(0, &widget, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto* slider = dialog->findChild<QSlider*>("scaleSlider");
            auto* selector = slider ? dynamic_cast<ScaleSelector*>(slider->parentWidget()) : nullptr;
            if (selector) {
                effectOptionsRemoved = dialog->findChildren<QSpinBox*>().isEmpty()
                    && dialog->findChildren<QSlider*>().size() == 1;
                for (const QLabel* label : dialog->findChildren<QLabel*>())
                    effectOptionsRemoved &= label->text() != QStringLiteral("模糊程度")
                        && label->text() != QStringLiteral("透明度");
                slider->setValue(125);
                checked = selector->previewSize() == DesktopWidget::cardSize(
                    DesktopWidget::CardKind::Battery, 0, 1.25, dialog->screen()->availableGeometry());
                dialog->accept();
            } else dialog->reject();
        });
        widget.showSettingsDialog();
        QVERIFY(checked);
        QVERIFY(effectOptionsRemoved);
        QCOMPARE(widget.m_widgetScale, 1.25);
        QCOMPARE(AppSettings().value("appearance/scale").toDouble(), 1.25);
        QCOMPARE(widget.glassOpacity(), 1.0f);
        QVERIFY(!AppSettings().contains("appearance/blurStrength"));
        QVERIFY(!AppSettings().contains("appearance/opacity"));
    }

    void unifiedGlassUsesSingleOpticalMaterialWithoutOverlayMasks() {
        AppSettings().setValue("appearance/liveBackdrop", false);
        DesktopWidget card(nullptr, DesktopWidget::CardKind::Weather, false,
                           QStringLiteral("unified-glass"), 1, true);
        FirstRunWizard wizard;
        WidgetLibraryDialog library;
        QFont font(DesktopWidget::pingFangFontFamily());
        LiquidGlassMenu menu(font);
        menu.addGlassAction(QStringLiteral("统一液态玻璃"), LiquidGlassMenu::Icon::Add);
        menu.popup(QGuiApplication::primaryScreen()->availableGeometry().topLeft() + QPoint(80, 80));
        QVERIFY(QTest::qWaitForWindowExposed(&menu));
        auto* menuSurface = menu.findChild<QtGlassFlowScene*>();
        QVERIFY(menuSurface);
        for (QtGlassFlowScene* surface : QList<QtGlassFlowScene*>{&card, &wizard, &library, menuSurface}) {
            QCOMPARE(surface->m_fPower, LiquidGlassStyle::kRefractionPower);
            QCOMPARE(surface->m_blurRadius, LiquidGlassStyle::kBlurRadius);
            QCOMPARE(surface->m_blurIterations, LiquidGlassStyle::kBlurIterations);
            QCOMPARE(surface->m_noiseAmount, LiquidGlassStyle::kNoiseAmount);
            QCOMPARE(surface->glassOpacity(), 1.0f);
            QCOMPARE(surface->m_objects.first().powerFactor, LiquidGlassStyle::kShapePower);
        }
        // 恒色背景只经过一次统一光学响应，重复遮罩或独立明暗渐变都会触发失败。
        const QColor backdrop(205, 155, 95);
        const QColor materialColor(169, 127, 73);
        const auto setBackdrop = [&](QtGlassFlowScene& surface) {
            QImage background(surface.size() * surface.devicePixelRatioF(), QImage::Format_RGB32);
            background.fill(backdrop);
            surface.setBackgroundImage(background);
            surface.setNoiseAmount(0);
            surface.update();
        };
        setBackdrop(*menuSurface);
        QTest::qWait(40);
        const QImage menuFrame = menuSurface->grabFramebuffer();
        const QColor menuPixel = menuFrame.pixelColor(menuFrame.width() / 2,
            qRound(4 * menuSurface->devicePixelRatioF()));
        QVERIFY(qAbs(menuPixel.red() - materialColor.red()) <= 2);
        QVERIFY(qAbs(menuPixel.green() - materialColor.green()) <= 2);
        QVERIFY(qAbs(menuPixel.blue() - materialColor.blue()) <= 2);
        QVERIFY(menuFrame.save(QStringLiteral("artifacts/unified-glass-menu.png")));
        menu.hide();
        wizard.setDesktopCaptureEnabled(false);
        wizard.show();
        QVERIFY(QTest::qWaitForWindowExposed(&wizard));
        setBackdrop(wizard);
        QTest::qWait(40);
        const QImage wizardFrame = wizard.grabFramebuffer();
        for (qreal y : {.25, .5, .75}) {
            const QColor pixel = wizardFrame.pixelColor(wizardFrame.width() / 2,
                qRound(wizardFrame.height() * y));
            QVERIFY(qAbs(pixel.red() - materialColor.red()) <= 2);
            QVERIFY(qAbs(pixel.green() - materialColor.green()) <= 2);
            QVERIFY(qAbs(pixel.blue() - materialColor.blue()) <= 2);
        }
        QVERIFY(wizardFrame.save(QStringLiteral("artifacts/unified-glass-wizard.png")));
        QVERIFY(wizard.grab().save(QStringLiteral("artifacts/unified-glass-wizard-window.png")));
    }

    void sceneDragStopsOnReleaseAndObjectHide() {
        QtGlassFlowScene scene;
        scene.resize(240, 180);
        const int lower = scene.addGlassObject(QPointF(20, 20), QSizeF(80, 60));
        const int upper = scene.addGlassObject(QPointF(20, 20), QSizeF(80, 60));
        scene.show();
        QVERIFY(QTest::qWaitForWindowExposed(&scene));
        QVERIFY(scene.m_timer);
        QVERIFY(!scene.m_timer->isActive());
        const auto send = [&](QEvent::Type type, QPoint point, Qt::MouseButton button,
                              Qt::MouseButtons buttons) {
            mouse(scene, type, point, scene.mapToGlobal(point), button, buttons);
        };
        send(QEvent::MouseButtonPress, QPoint(60, 50), Qt::LeftButton, Qt::LeftButton);
        QVERIFY(scene.m_timer->isActive());
        send(QEvent::MouseMove, QPoint(100, 80), Qt::NoButton, Qt::LeftButton);
        QCOMPARE(scene.m_objects[lower].position, QPointF(20, 20));
        QCOMPARE(scene.m_objects[upper].position, QPointF(60, 50));
        send(QEvent::MouseButtonRelease, QPoint(100, 80), Qt::LeftButton, Qt::NoButton);
        QVERIFY(!scene.m_timer->isActive());
        send(QEvent::MouseMove, QPoint(150, 100), Qt::NoButton, Qt::NoButton);
        QCOMPARE(scene.m_objects[upper].position, QPointF(60, 50));
        send(QEvent::MouseButtonPress, QPoint(100, 80), Qt::LeftButton, Qt::LeftButton);
        scene.setGlassObjectVisible(upper, false);
        QVERIFY(!scene.m_timer->isActive());
        send(QEvent::MouseMove, QPoint(150, 100), Qt::NoButton, Qt::LeftButton);
        QCOMPARE(scene.m_objects[upper].position, QPointF(60, 50));
        send(QEvent::MouseButtonRelease, QPoint(150, 100), Qt::LeftButton, Qt::NoButton);
    }

    void musicCardsRenderStatesAndRestoreConfiguration() {
        MediaSnapshot media;
        media.available = true;
        media.playing = true;
        media.canNext = media.canPrevious = media.canToggle = true;
        media.title = QStringLiteral("从系统读取的曲目 · 测试很长的曲名不会溢出");
        media.artist = QStringLiteral("艺术家");
        media.durationMs = 240000;
        media.positionMs = 75000;
        for (const QSize size : {QSize(328,328), QSize(672,328)}) {
            const auto regions = MusicCard::layout(QRectF(QPointF(), QSizeF(size)));
            QVERIFY(!regions.toggle.intersects(regions.previous));
            QVERIFY(!regions.toggle.intersects(regions.next));
            for (bool playing : {true, false}) {
                media.playing = playing;
                QImage image(size * 2, QImage::Format_ARGB32_Premultiplied);
                image.setDevicePixelRatio(2);
                image.fill(QColor(32,40,55));
                QPainter painter(&image);
                MusicCard::paint(painter, QRectF(QPointF(), QSizeF(size)), media);
                painter.end();
                QVERIFY(image.save(QStringLiteral("artifacts/music-%1-%2.png").arg(size.width()).arg(playing)));
            }
        }
        {
            DesktopWidget music(nullptr, DesktopWidget::CardKind::Music, false,
                QStringLiteral("music-restore"), 1);
            QCOMPARE(music.cardVariant(), 1);
            music.saveConfiguration();
        }
        auto restored = DesktopWidget::restoreWidgets();
        QCOMPARE(restored.size(), 1);
        QCOMPARE(restored.first()->cardKind(), DesktopWidget::CardKind::Music);
        QCOMPARE(restored.first()->instanceId(), QStringLiteral("music-restore"));
        QCOMPARE(restored.first()->cardVariant(), 1);
        qDeleteAll(restored);
    }

    void systemMediaReadsAndControlsOwnSession() {
#ifdef Q_OS_WIN
        using namespace winrt::Windows::Media;
        QWidget host;
        host.setWindowTitle(QStringLiteral("系统音乐回归测试"));
        host.resize(160,80);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        SystemMediaTransportControls controls{nullptr};
        try {
            auto interop = winrt::get_activation_factory<SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
            winrt::check_hresult(interop->GetForWindow(reinterpret_cast<HWND>(host.winId()),
                winrt::guid_of<SystemMediaTransportControls>(), winrt::put_abi(controls)));
            controls.IsEnabled(true);
            controls.IsPlayEnabled(true);
            controls.IsPauseEnabled(true);
            controls.IsPreviousEnabled(true);
            controls.IsNextEnabled(true);
            auto updater = controls.DisplayUpdater();
            updater.Type(MediaPlaybackType::Music);
            updater.MusicProperties().Title(L"macdowsOS 媒体会话回归测试");
            updater.MusicProperties().Artist(L"系统会话测试源");
            updater.Update();
            SystemMediaTransportControlsTimelineProperties timeline;
            timeline.StartTime(std::chrono::milliseconds(0));
            timeline.EndTime(std::chrono::milliseconds(240000));
            timeline.Position(std::chrono::milliseconds(75000));
            timeline.MinSeekTime(std::chrono::milliseconds(0));
            timeline.MaxSeekTime(std::chrono::milliseconds(240000));
            controls.UpdateTimelineProperties(timeline);
            controls.PlaybackStatus(MediaPlaybackStatus::Playing);
        } catch (const winrt::hresult_error&) {
            QSKIP("Windows session does not provide desktop SMTC");
        }
        const auto cleanup = qScopeGuard([&] { controls.IsEnabled(false); });
        auto* service = SystemMediaSession::instance();
        QElapsedTimer timeout;
        timeout.start();
        while (service->snapshot().title != QStringLiteral("macdowsOS 媒体会话回归测试") && timeout.elapsed() < 6000)
            QTest::qWait(100);
        if (service->snapshot().title != QStringLiteral("macdowsOS 媒体会话回归测试"))
            QSKIP("Another system media session is selected, or the shell did not publish the fixture");
        QVERIFY(service->snapshot().available);
        QVERIFY(service->snapshot().playing);
        QCOMPARE(service->snapshot().artist, QStringLiteral("系统会话测试源"));
        QCOMPARE(service->snapshot().durationMs, 240000);
        QVERIFY(service->snapshot().positionMs >= 75000 && service->snapshot().positionMs < 90000);
        // 只控制本测试创建的会话，避免影响用户已有的播放器。
        std::atomic<int> buttons{0};
        const auto token = controls.ButtonPressed([&](const auto&, const SystemMediaTransportControlsButtonPressedEventArgs& args) {
            if (args.Button() == SystemMediaTransportControlsButton::Pause) {
                ++buttons;
                controls.PlaybackStatus(MediaPlaybackStatus::Paused);
            } else if (args.Button() == SystemMediaTransportControlsButton::Next) {
                ++buttons;
                controls.DisplayUpdater().MusicProperties().Title(L"下一首测试曲目");
                controls.DisplayUpdater().Update();
            } else if (args.Button() == SystemMediaTransportControlsButton::Previous) {
                ++buttons;
                controls.DisplayUpdater().MusicProperties().Title(L"上一首测试曲目");
                controls.DisplayUpdater().Update();
            }
        });
        const auto revoke = qScopeGuard([&] { controls.ButtonPressed(token); });
        service->sendCommand(SystemMediaSession::Command::TogglePlayback);
        QTRY_VERIFY_WITH_TIMEOUT(buttons.load() > 0, 4000);
        QTRY_VERIFY_WITH_TIMEOUT(!service->snapshot().playing, 4000);
        QVERIFY(service->snapshot().canNext && service->snapshot().canPrevious);
        service->sendCommand(SystemMediaSession::Command::Next);
        QTRY_COMPARE_WITH_TIMEOUT(service->snapshot().title, QStringLiteral("下一首测试曲目"), 4000);
        service->sendCommand(SystemMediaSession::Command::Previous);
        QTRY_COMPARE_WITH_TIMEOUT(service->snapshot().title, QStringLiteral("上一首测试曲目"), 4000);
#else
        QSKIP("Requires Windows system media sessions");
#endif
    }

    void darkGlassSwitchKeepsLightPixelsAndBlurCache() {
        PipelineProbe scene;
        scene.resize(260, 180);
        scene.addGlassObject({}, QSizeF(scene.size()));
        scene.setGlassObjectCornerRadius(0, 36);
        scene.setNoiseAmount(0);
        QImage background(scene.size(), QImage::Format_RGB32);
        background.fill(QColor(180, 140, 100));
        scene.setBackgroundImage(background);
        scene.show();
        QVERIFY(QTest::qWaitForWindowExposed(&scene));
        const QImage light = scene.grabFramebuffer();
        QVERIFY(!light.isNull());
        QVERIFY(!scene.darkModeEnabled());
        QVERIFY(!scene.m_blurCacheDirty);
        const auto* cache = scene.m_blurPongFbo;
        const quint64 before = scene.frameRevision();
        scene.setDarkModeEnabled(true);
        QTRY_VERIFY(scene.frameRevision() > before);
        const QImage dark = scene.grabFramebuffer();
        QCOMPARE(scene.m_blurPongFbo, cache);
        QVERIFY(!scene.m_blurCacheDirty);
        const QColor pixel = dark.pixelColor(dark.width() / 2, dark.height() / 2);
        QVERIFY(qGray(pixel.rgb()) < qGray(light.pixelColor(light.width() / 2, light.height() / 2).rgb()) * .6);
        QVERIFY(pixel.red() > pixel.green() && pixel.green() > pixel.blue());
        QCOMPARE(pixel.alpha(), 255);
        QCOMPARE(dark.pixelColor(0, 0).alpha(), 0);
        scene.setDarkModeEnabled(false);
        QCOMPARE(scene.grabFramebuffer(), light);
        scene.setDarkModeEnabled(true);
        background.fill(QColor(20, 100, 200));
        scene.setBackgroundImage(background);
        const QColor changed = centerPixel(scene);
        QVERIFY(changed.blue() > changed.green() && changed.green() > changed.red());
        QVERIFY(changed != pixel);
    }

    void darkModePersistsAndSynchronizesExistingAndNewSurfaces() {
        DesktopWidget card(nullptr, DesktopWidget::CardKind::Clock, false, "theme-card", 0, true);
        WidgetLibraryDialog gallery;
        FirstRunWizard wizard;
        const QSize initialSize = gallery.size();
        const QSize initialTileSize = gallery.m_tiles.first()->size();
        const QImage light = DesktopWidget::renderPreview(DesktopWidget::CardKind::Battery, 0, QSize(192, 192));
        QVERIFY(!card.darkModeEnabled());
        DesktopWidget::setGlobalDarkMode(true);
        QVERIFY(AppSettings().value("appearance/darkMode").toBool());
        QVERIFY(card.darkModeEnabled());
        QVERIFY(gallery.darkModeEnabled());
        QVERIFY(wizard.darkModeEnabled());
        QCOMPARE(gallery.size(), initialSize);
        QCOMPARE(gallery.m_tiles.first()->size(), initialTileSize);
        const QImage dark = DesktopWidget::renderPreview(DesktopWidget::CardKind::Battery, 0, QSize(192, 192));
        QVERIFY(dark != light);
        const auto tile = gallery.m_tiles.first();
        const qreal dpr = tile->devicePixelRatioF();
        const QImage reference = DesktopWidget::renderPreview(DesktopWidget::CardKind::Battery, 0, tile->size(), dpr);
        QImage rendered(reference.size(), reference.format());
        rendered.setDevicePixelRatio(dpr);
        rendered.fill(Qt::transparent);
        QPainter painter(&rendered);
        tile->render(&painter, {}, {}, QWidget::DrawChildren);
        painter.end();
        QCOMPARE(rendered, reference);
        DesktopWidget later(nullptr, DesktopWidget::CardKind::Battery, false, "theme-later", 0, true);
        QVERIFY(later.darkModeEnabled());
        LiquidGlassMenu menu{QFont(DesktopWidget::pingFangFontFamily())};
        menu.addGlassAction(QStringLiteral("添加小组件…"), LiquidGlassMenu::Icon::Add);
        menu.popup(QGuiApplication::primaryScreen()->availableGeometry().center());
        QVERIFY(QTest::qWaitForWindowExposed(&menu));
        auto* surface = menu.findChild<QtGlassFlowScene*>();
        QVERIFY(surface && surface->darkModeEnabled());
        menu.hide();
        DesktopWidget::setGlobalDarkMode(false);
        QVERIFY(!AppSettings().value("appearance/darkMode").toBool());
        QVERIFY(!card.darkModeEnabled() && !gallery.darkModeEnabled() && !wizard.darkModeEnabled());
        QCOMPARE(DesktopWidget::renderPreview(DesktopWidget::CardKind::Battery, 0, QSize(192, 192)), light);
    }

    void settingsColorModeAcceptsAndCancels() {
        DesktopWidget card(nullptr, DesktopWidget::CardKind::Clock, false, "theme-settings", 0, true);
        for (int step = 0; step < 3; ++step) {
            bool foundControl = false;
            QTimer::singleShot(0, &card, [&]() {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                auto* mode = dialog->findChild<QComboBox*>("colorMode");
                foundControl = mode && mode->count() == 2;
                if (foundControl)
                    mode->setCurrentIndex(mode->findData(step == 0));
                if (foundControl && step != 1)
                    dialog->accept();
                else
                    dialog->reject();
            });
            card.showSettingsDialog();
            QVERIFY(foundControl);
            QCOMPARE(card.darkModeEnabled(), step != 2);
            QCOMPARE(AppSettings().value("appearance/darkMode").toBool(), step != 2);
        }
    }

    void darkModeReferenceComparison() {
        const QString wallpaperPath = qEnvironmentVariable("WIDGET_THEME_REFERENCE_BACKGROUND");
        if (wallpaperPath.isEmpty())
            QSKIP("Set WIDGET_THEME_REFERENCE_BACKGROUND for reference wallpaper rendering");
        const QImage wallpaper(wallpaperPath);
        QVERIFY(!wallpaper.isNull());
        DesktopWidget weather(nullptr, DesktopWidget::CardKind::Weather, false, "dark-reference-weather", 1, true);
        DesktopWidget battery(nullptr, DesktopWidget::CardKind::Battery, false, "dark-reference-battery", 1, true);
        weather.m_weatherLocation = QStringLiteral("天心区");
        weather.m_weatherTemperature = "20";
        weather.m_weatherDescription = QStringLiteral("晴朗无云");
        weather.m_weatherHigh = "30";
        weather.m_weatherLow = "20";
        weather.m_weatherCode = 0;
        weather.m_weatherNight = true;
        const QString times[] = {QStringLiteral("6时"), "06:25", QStringLiteral("7时"),
                                 QStringLiteral("8时"), QStringLiteral("9时"), QStringLiteral("10时")};
        for (int i = 0; i < 6; ++i)
            weather.m_weatherHours.append({times[i], QString::number(i < 4 ? 20 + qMin(i, 1) : 23 + (i - 4) * 2), 0, i == 0});
        battery.m_hasBattery = true;
        battery.m_batteryLevel = 95;
        battery.m_connectedDevices = {{QStringLiteral("Mouse"), 47, 1}};
        const QRect bounds[] = {QRect(401, 470, 570, 270), QRect(401, 769, 570, 270)};
        QList<DesktopWidget*> cards{&weather, &battery};
        for (int i = 0; i < cards.size(); ++i) {
            auto* card = cards[i];
            card->m_widgetScale = 270.0 / CardMetrics::kGridBaseCell;
            card->setMinimumSize(0, 0);
            card->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            card->resize(bounds[i].size());
            card->setGlassRadius(LiquidGlassStyle::kCardCornerRadius * card->m_widgetScale);
            card->setRenderScale(1.25f);
            card->setBackgroundImage(wallpaper.copy(bounds[i]));
            card->show();
            QVERIFY(QTest::qWaitForWindowExposed(card));
        }
        for (bool dark : {false, true}) {
            QImage result = wallpaper.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            QPainter painter(&result);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            for (int i = 0; i < cards.size(); ++i) {
                cards[i]->setDarkModeEnabled(dark);
                painter.drawImage(bounds[i], cards[i]->grabFramebuffer());
            }
            painter.end();
            QVERIFY(result.save(dark ? "artifacts/dark-mode-reference-dark.png"
                                     : "artifacts/dark-mode-reference-light.png"));
        }
    }

    void settingsJsonMigratesAndPreservesLayout() {
        {
            AppSettings defaults;
            QCOMPARE(defaults.value("appearance/scale").toDouble(), 1.0);
            QCOMPARE(defaults.value("appearance/darkMode").toBool(), false);
            QCOMPARE(defaults.value("performance/renderScale").toDouble(), 0.60);
            QVERIFY(!defaults.value("appearance/blurStrength").isValid());
            QVERIFY(!defaults.value("appearance/opacity").isValid());
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
                              {"systemBlur", true}, {"blurStrength", 65}, {"opacity", 0.2}}},
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
        QVERIFY(!appearance.contains("opacity"));
        QVERIFY(!appearance.contains("blurStrength"));
        QVERIFY(!appearance.contains("systemBlur"));
        QVERIFY(!root.value("settings").toObject().value("performance").toObject()
                     .contains("renderBackend"));
        QCOMPARE(root.value("settings").toObject().value("weather").toObject()
                     .value("location").toString(), QStringLiteral("长沙"));
        QVERIFY(root.value("settings").toObject().contains("interaction"));

        DesktopWidget::saveAllConfigurations();
        QFile layoutSaved(AppSettings::filePath());
        QVERIFY(layoutSaved.open(QIODevice::ReadOnly));
        root = QJsonDocument::fromJson(layoutSaved.readAll()).object();
        QVERIFY(!root.value("settings").toObject().value("appearance").toObject()
                     .contains("opacity"));
    }

    void responsiveLayoutUsesFontMetricsAndDeviceScale() {
        QImage image(QSize(480, 220), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        QFont font(DesktopWidget::pingFangFontFamily());
        font.setPointSize(72);
        painter.setFont(font);
        painter.setPen(Qt::white);

        // 刻意使用小于字体行高的对齐区域，验证基线绘制能保留完整字形。
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
        for (auto kind : {DesktopWidget::CardKind::Battery, DesktopWidget::CardKind::Weather,
                          DesktopWidget::CardKind::Clock, DesktopWidget::CardKind::Dictionary}) {
            AppSettings().setValue("appearance/scale", 1.0);
            DesktopWidget widget(nullptr, kind, false, QStringLiteral("corner-probe"), 0);
            QCOMPARE(widget.size(), QSize(328, 328));
            QCOMPARE(widget.m_widgetScale, 1.0);
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

        DesktopWidget compact(nullptr, DesktopWidget::CardKind::Battery, false,
                              QStringLiteral("battery-1x1"), 0);
        DesktopWidget wide(nullptr, DesktopWidget::CardKind::Battery, false,
                           QStringLiteral("battery-1x2"), 1);
        DesktopWidget list(nullptr, DesktopWidget::CardKind::Battery, false,
                           QStringLiteral("battery-2x2"), 2);
        QCOMPARE(compact.width(), compact.height());
        QCOMPARE(wide.height(), compact.height());
        QCOMPARE(list.width(), wide.width());
        QCOMPARE(list.width(), list.height());

        for (DesktopWidget* widget : {&compact, &wide, &list}) {
            widget->setDesktopLayerEnabled(false);
            widget->setDesktopCaptureEnabled(false);
            widget->setAnimationEnabled(false);
            QImage background(widget->size(), QImage::Format_RGB32);
            background.fill(QColor(53, 67, 84));
            widget->setBackgroundImage(background);
            widget->m_hasBattery = true;
            widget->m_batteryLevel = 95;
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

        DesktopWidget compact(nullptr, DesktopWidget::CardKind::Weather, false,
                              QStringLiteral("weather-1x1"), 0);
        DesktopWidget hourly(nullptr, DesktopWidget::CardKind::Weather, false,
                             QStringLiteral("weather-1x2"), 1);
        DesktopWidget forecast(nullptr, DesktopWidget::CardKind::Weather, false,
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

        const QVector<DesktopWidget::WeatherHour> hours = {
            {QStringLiteral("11时"), QStringLiteral("30"), 1},
            {QStringLiteral("12时"), QStringLiteral("30"), 1},
            {QStringLiteral("13时"), QStringLiteral("31"), 1},
            {QStringLiteral("14时"), QStringLiteral("31"), 1},
            {QStringLiteral("15时"), QStringLiteral("31"), 1},
            {QStringLiteral("16时"), QStringLiteral("31"), 1}
        };
        const QVector<DesktopWidget::WeatherDay> days = {
            {QStringLiteral("今天"), QStringLiteral("32"), QStringLiteral("21"), 1},
            {QStringLiteral("周三"), QStringLiteral("31"), QStringLiteral("23"), 7},
            {QStringLiteral("周四"), QStringLiteral("29"), QStringLiteral("20"), 7},
            {QStringLiteral("周五"), QStringLiteral("28"), QStringLiteral("21"), 1},
            {QStringLiteral("周六"), QStringLiteral("30"), QStringLiteral("22"), 0},
            {QStringLiteral("周日"), QStringLiteral("29"), QStringLiteral("21"), 1}
        };
        for (DesktopWidget* widget : {&compact, &hourly, &forecast}) {
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
        const auto renderState = [&](DesktopWidget& widget,
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
                                                       : QVector<DesktopWidget::WeatherHour>{
                                                           {QString(), QString(), -1},
                                                           {QStringLiteral("稍后"), QString(), -1}
                                                       };
            widget.m_weatherDays = state.hasForecast ? days
                                                      : QVector<DesktopWidget::WeatherDay>{
                                                           {QStringLiteral("今天"), QString(), QString(), -1},
                                                           {QStringLiteral("周三"), QString(), QString(), -1}
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

        DesktopWidget analog(nullptr, DesktopWidget::CardKind::Clock, false,
                             QStringLiteral("clock-responsive-analog"), 0);
        DesktopWidget digital(nullptr, DesktopWidget::CardKind::Clock, false,
                              QStringLiteral("clock-responsive-digital"), 2);
        DesktopWidget dark(nullptr, DesktopWidget::CardKind::Clock, false,
                           QStringLiteral("clock-responsive-dark"), 1);
        DesktopWidget city(nullptr, DesktopWidget::CardKind::Clock, false,
                           QStringLiteral("clock-responsive-city"), 4);
        DesktopWidget wideDigital(nullptr, DesktopWidget::CardKind::Clock, false,
                                  QStringLiteral("clock-responsive-wide"), 8);
        QVERIFY2(wideDigital.width() > wideDigital.height(),
                 "The 2x1 digital clock must be horizontal");
        DesktopWidget dictionary(nullptr, DesktopWidget::CardKind::Dictionary, false,
                                 QStringLiteral("dictionary-responsive-wide"), 1);
        dictionary.m_dictionaryWord = QStringLiteral("serendipity");
        dictionary.m_dictionaryPhonetic = QStringLiteral("/ˌserənˈdipədē/");
        dictionary.m_dictionaryPartOfSpeech = QStringLiteral("noun");
        dictionary.m_dictionaryDefinition = QStringLiteral("the chance occurrence of a happy discovery");

        for (DesktopWidget* widget : {&analog, &digital, &dark, &city,
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
        const QRect first = DesktopWidget::nearestGridRect(
            DesktopWidget::CardKind::Battery, 0, area.topLeft(), scale);
        QVERIFY(first.isValid());

        DesktopWidget hidden(nullptr, DesktopWidget::CardKind::Battery, false,
                             QStringLiteral("hidden-grid-card"), 0);
        hidden.setGeometry(first);
        hidden.hide();
        const QRect target = DesktopWidget::nearestGridRect(
            DesktopWidget::CardKind::Battery, 0, first.topLeft(), scale);
        QCOMPARE(target, first);
    }

    void freePlacementOnlySnapsBesideVisibleCards() {
        AppSettings().setValue("appearance/scale", 0.4);
        QScreen* screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QRect area = screen->availableGeometry();
        const QPoint freePosition = area.topLeft() + QPoint(85, 95);
        DesktopWidget card(nullptr, DesktopWidget::CardKind::Clock, false,
                           QStringLiteral("free-placement"), 0);
        QVERIFY(card.placeAtDesktopPoint(freePosition + QPoint(card.width() / 2,
                                                               card.height() / 2)));
        QCOMPARE(card.pos(), freePosition);
        QVERIFY(!DesktopWidget::nearbyWidgetRect(DesktopWidget::CardKind::Clock, 0,
                                                  freePosition + QPoint(45, 37), 0.4).isValid());

        card.show();
        QVERIFY(QTest::qWaitForWindowExposed(&card));
        const QPoint beside(card.geometry().right() + 9, card.y());
        const QRect snapped = DesktopWidget::nearbyWidgetRect(
            DesktopWidget::CardKind::Clock, 0, beside, 0.4);
        QVERIFY(snapped.isValid());
        QCOMPARE(snapped.top(), card.y());
        QVERIFY(snapped.left() > card.geometry().right());
        const QPoint alongEdge(beside.x(), card.y() + 30);
        const QRect edgeSnap = DesktopWidget::nearbyWidgetRect(
            DesktopWidget::CardKind::Clock, 0, alongEdge, 0.4);
        QVERIFY(edgeSnap.isValid());
        QCOMPARE(edgeSnap, snapped);
        QCOMPARE(DesktopWidget::nearbyWidgetRect(DesktopWidget::CardKind::Clock, 0,
                     QPoint(beside.x(), card.y() + 55), 0.4), snapped);
        QCOMPARE(DesktopWidget::nearbyWidgetRect(DesktopWidget::CardKind::Clock, 0,
                     beside + QPoint(40, 0), 0.4), snapped);
        QCOMPARE(DesktopWidget::nearbyWidgetRect(DesktopWidget::CardKind::Clock, 0,
                     beside + QPoint(60, 0), 0.4), snapped);
        QVERIFY(!DesktopWidget::nearbyWidgetRect(DesktopWidget::CardKind::Clock, 0,
                     beside + QPoint(130, 0), 0.4).isValid());
        QVERIFY(!DesktopWidget::nearbyWidgetRect(DesktopWidget::CardKind::Clock, 0,
                  area.center(), 0.4).isValid());

        DesktopWidget added(nullptr, DesktopWidget::CardKind::Clock, false,
                            QStringLiteral("overlap-drop"), 0);
        QVERIFY(added.placeAtDesktopPoint(card.geometry().center()));
        QVERIFY(!added.geometry().intersects(card.geometry()));

        DesktopWidget dragged(nullptr, DesktopWidget::CardKind::Clock, false,
                              QStringLiteral("overlap-drag"), 0);
        dragged.move(area.center());
        dragged.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dragged));
        dragged.m_dragOrigin = dragged.pos();
        dragged.move(card.pos());
        dragged.windowDragFinished();
        QVERIFY(!dragged.geometry().intersects(card.geometry()));

        DesktopWidget restored(nullptr, DesktopWidget::CardKind::Clock, false,
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
        DesktopWidget anchor(nullptr, DesktopWidget::CardKind::Clock, false,
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
            const QRect slot = DesktopWidget::nearbyWidgetRect(
                DesktopWidget::CardKind::Clock, 0, probes[index], 0.4);
            QVERIFY2(slot.isValid(), "Every corner needs a placement slot");
            QVERIFY(!slot.intersects(bounds));
            QCOMPARE(DesktopWidget::nearbyWidgetRect(DesktopWidget::CardKind::Clock, 0,
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

    void largeCardsSnapToStaggeredCellSlots() {
        using namespace WidgetGeometry;
        using Kind = DesktopWidget::CardKind;
        QScreen* screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QRect area = screen->availableGeometry();
        for (qreal scale : {.4, .7}) {
            AppSettings().setValue("appearance/scale", scale);
            DesktopWidget anchor(nullptr, Kind::Weather, false, "cell-slot-anchor", 2, true);
            DesktopWidget moving(nullptr, Kind::Weather, false, "cell-slot-moving", 2, true);
            const int pitch = qRound(CardMetrics::kGridBaseCell * scale) + gridGapForArea(area, scale);
            anchor.move(area.center() - QPoint(anchor.width() / 2, anchor.height() / 2));
            anchor.show();
            QVERIFY(QTest::qWaitForWindowExposed(&anchor));
            const QRect bounds = anchor.geometry();
            const int gap = gridGapForArea(area, scale);
            for (int offset : {-1, 0, 1}) {
                const QPoint positions[] = {
                    {bounds.left() + offset * pitch, bounds.top() - gap - moving.height()},
                    {bounds.left() + offset * pitch, bounds.bottom() + gap + 1},
                    {bounds.left() - gap - moving.width(), bounds.top() + offset * pitch},
                    {bounds.right() + gap + 1, bounds.top() + offset * pitch}
                };
                for (const QPoint& slot : positions) {
                    QRect expected(slot, moving.size());
                    const QRect extent = bounds.united(expected);
                    QVERIFY(extent.width() <= area.width());
                    QVERIFY(extent.height() <= area.height());
                    const QPoint shift(
                        qBound(area.left(), extent.left(), area.right() - extent.width() + 1) - extent.left(),
                        qBound(area.top(), extent.top(), area.bottom() - extent.height() + 1) - extent.top());
                    anchor.move(bounds.topLeft() + shift);
                    expected.translate(shift);
                    QVERIFY(area.contains(expected));
                    const QRect actual = DesktopWidget::nearbyWidgetRect(
                        Kind::Weather, 2, expected.topLeft() + QPoint(7, 9), scale, &moving);
                    QCOMPARE(actual, expected);
                    QVERIFY(!actual.intersects(anchor.geometry()));
                }
            }
            anchor.move(area.topLeft() + QPoint(pitch * 2, 10));
            const QRect blockedAnchor = anchor.geometry();
            const QPoint blockedPosition(blockedAnchor.left() - pitch, blockedAnchor.bottom() + gap + 1);
            DesktopWidget blocker(nullptr, Kind::Clock, false, "cell-slot-blocker", 0, true);
            blocker.move(blockedPosition);
            blocker.show();
            QVERIFY(QTest::qWaitForWindowExposed(&blocker));
            const QRect alternative = DesktopWidget::nearbyWidgetRect(
                Kind::Weather, 2, blockedPosition, scale, &moving);
            QVERIFY(alternative.isValid());
            QVERIFY(!alternative.intersects(blocker.geometry()));
            QVERIFY(!alternative.intersects(blockedAnchor));
            blocker.hide();
            QCOMPARE(DesktopWidget::nearbyWidgetRect(Kind::Weather, 2, blockedPosition, scale, &moving),
                     QRect(blockedPosition, moving.size()));
        }
    }

    void wideWeatherPreviewAndDropUseLeftCellSlot() {
        using namespace WidgetGeometry;
        using Kind = DesktopWidget::CardKind;
        const qreal scale = .7;
        AppSettings().setValue("appearance/scale", scale);
        QScreen* screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QRect area = screen->availableGeometry();
        const int cell = qRound(CardMetrics::kGridBaseCell * scale);
        const int pitch = cell + gridGapForArea(area, scale);
        const QPoint origin = area.center() - QPoint(pitch * 3 / 2, pitch * 3 / 2);
        DesktopWidget battery(nullptr, Kind::Battery, false, "left-slot-battery", 1, true);
        DesktopWidget analog(nullptr, Kind::Clock, false, "left-slot-analog", 0, true);
        DesktopWidget digital(nullptr, Kind::Clock, false, "left-slot-digital", 8, true);
        DesktopWidget weather(nullptr, Kind::Weather, false, "left-slot-weather", 1, true);
        battery.move(origin);
        analog.move(origin + QPoint(2 * pitch, 0));
        digital.move(origin + QPoint(pitch, pitch));
        const QRect expected(origin + QPoint(0, 2 * pitch), weather.size());
        const QPoint requested = expected.topLeft() - QPoint(35, 7);
        weather.move(requested);
        for (DesktopWidget* widget : {&battery, &analog, &digital, &weather}) {
            widget->show();
            QVERIFY(QTest::qWaitForWindowExposed(widget));
        }
        const QRect preview = DesktopWidget::nearbyWidgetRect(Kind::Weather, 1, requested, scale, &weather);
        QCOMPARE(preview, expected);
        DesktopWidget::showGridPreviewRect(preview);
        bool foundPreview = false;
        for (QWidget* widget : QApplication::topLevelWidgets())
            if (widget->isVisible() && widget->windowFlags().testFlag(Qt::WindowTransparentForInput)
                && widget->geometry() == expected)
                foundPreview = true;
        QVERIFY(foundPreview);
        weather.m_dragOrigin = requested;
        weather.windowDragFinished();
        QCOMPARE(weather.geometry(), expected);
        QVERIFY(!DesktopWidget::overlapsVisibleWidget(weather.geometry(), &weather));
        weather.hide();
        // 图库添加与桌面拖动共用落点计算，隐藏的旧卡片不占位。
        QVERIFY(weather.placeAtDesktopPoint(requested + QPoint(weather.width() / 2, weather.height() / 2)));
        QCOMPARE(weather.geometry(), expected);
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
        QTRY_VERIFY_WITH_TIMEOUT(!welcomeNext->isVisible(), 1500);
        scale->setValue(85);
        QVERIFY(wizard.grab().save(QStringLiteral("artifacts/first-run-scale.png")));
        scaleNext->click();
        QTRY_VERIFY_WITH_TIMEOUT(!scaleNext->isVisible(), 1500);
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

    void firstRunWizardShowsActualSizeWithoutResizingSurface() {
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
        auto* selector = dynamic_cast<ScaleSelector*>(slider->parentWidget());
        QVERIFY(selector);
        const QSize wizardSize = wizard.size();
        for (int percent : {40, 85, 100, 150, 200, 100}) {
            slider->setValue(percent);
            QApplication::processEvents();
            QCOMPARE(scaleValue->text(), QStringLiteral("%1%").arg(percent));
            QCOMPARE(wizard.size(), wizardSize);
            QCOMPARE(scaleNext->height(), 48);
            const QSize expected = DesktopWidget::cardSize(DesktopWidget::CardKind::Battery,
                0, percent / 100.0, wizard.screen()->availableGeometry());
            QCOMPARE(selector->previewSize(), expected);
            auto* actual = wizard.findChild<QWidget*>(QStringLiteral("actualSizePreview"));
            QVERIFY(actual && actual->isVisible());
            QTRY_COMPARE_WITH_TIMEOUT(actual->size(), expected, 1000);
            DesktopWidget reference(nullptr, DesktopWidget::CardKind::Battery, false,
                QStringLiteral("size-reference"), 0, true);
            reference.applyScale(percent / 100.0);
            QCOMPARE(actual->size(), reference.size());
            QFile::remove(AppSettings::filePath());
            wizard.grab().save(QStringLiteral("artifacts/wizard-scale-%1.png").arg(percent));
        }
        scaleNext->click();
        QTest::qWait(90);
        QVERIFY(pages.at(2)->x() > 0 && pages.at(2)->x() < wizard.width());
        QTest::qWait(320);
        QCOMPARE(pages.at(2)->x(), 0);
        QCOMPARE(wizard.renderScale(), 0.6f);
        QCOMPARE(wizard.font().family(), DesktopWidget::pingFangFontFamily());
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
            background.color = QColor::fromHsv(index * 70, 210, 210);
            background.update();
            QTRY_VERIFY_WITH_TIMEOUT(wizard.frameRevision() > revision, 1000);
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
        QVERIFY(!saved.contains("appearance/blurStrength"));
        QVERIFY(!saved.contains("appearance/opacity"));
        QCOMPARE(saved.value("appearance/fontSmoothing").toInt(), 4);
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
        if (!capture)
            QVERIFY2(presentedFrames < 10, "Static wizard keeps redrawing unchanged glass");
        if (capture)
            QVERIFY2(changedFrames >= qMax<qint64>(10, paintedSourceFrames / 2), "Wizard backdrop is stale");
        QVERIFY(!QFile::exists(AppSettings::filePath()));
    }

    void draggingCardHidesApplicationWindowsButKeepsWidgets() {
        AppSettings().setValue("appearance/scale", 0.4);

        DesktopWidget dragged(nullptr, DesktopWidget::CardKind::Weather, false,
                              QStringLiteral("drag-isolation-active"), 0);
        DesktopWidget visiblePeer(nullptr, DesktopWidget::CardKind::Clock, false,
                                  QStringLiteral("drag-isolation-visible"), 0);
        DesktopWidget hiddenPeer(nullptr, DesktopWidget::CardKind::Dictionary, false,
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

        // 取消和失去焦点应使用相同的窗口恢复路径。
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
        // 验证内部排除路径保留背景二维细节，不通过拉伸边缘像素填补组件区域。
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
        // 隐藏场景不能继续定时重绘。
        glass.hide();
        QTest::qWait(100);
        const quint64 revision = glass.frameRevision();
        QTest::qWait(150);
        QCOMPARE(glass.frameRevision(), revision);
    }

    void draggingKeepsInteractiveRenderCadence() {
        TestGlass glass;
        // 隔离场景调度与捕获回调，验证静态卡片拖动时能独立按交互频率绘制。
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
            // 按下鼠标前已排队的 WGC 请求允许完成，拖动期间不能继续补充。
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
            QCOMPARE(lower.m_backdropTimer->interval(), live ? qMax(1, lower.activeDisplayInterval() / 2) : 10000);
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
        QCOMPARE(lower.m_backdropTimer->interval(), qMax(1, lower.activeDisplayInterval() / 2));
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

        // 系统截图期间暂停实时采样，避免变化的下层窗口把卡片反馈到自身材质。
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
            DesktopWidget::setGlobalFontSmoothing(level);
            QCOMPARE(DesktopWidget::globalFontSmoothing(), level);
            QCOMPARE(AppSettings().value("appearance/fontSmoothing").toInt(), level);
            QFont font(DesktopWidget::pingFangFontFamily());
            DesktopWidget::applyFontSmoothing(font);
            if (level == 0)
                QVERIFY(int(font.styleStrategy()) & int(QFont::NoAntialias));
            else
                QVERIFY(int(font.styleStrategy()) & int(QFont::PreferAntialias));
        }
        DesktopWidget::setGlobalFontSmoothing(2);
    }

    void fontSmoothingChangesRenderedCoverage() {
        const auto renderText = [](int level) {
            DesktopWidget::setGlobalFontSmoothing(level);
            QImage image(260, 100, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::TextAntialiasing, level > 0);
            QFont font(DesktopWidget::pingFangFontFamily(), 48, QFont::DemiBold);
            DesktopWidget::applyFontSmoothing(font);
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
        DesktopWidget::setGlobalFontSmoothing(2);
    }

    void fontSmoothingRefreshesOpenGalleryPreviews() {
        DesktopWidget::setGlobalFontSmoothing(0);
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
        DesktopWidget::setGlobalFontSmoothing(3);
        const QImage smoothed = tile->grab().toImage();
        QVERIFY(!aliased.isNull());
        QVERIFY2(aliased != smoothed,
                 "The open gallery kept the preview rendered with the previous font setting");
        DesktopWidget::setGlobalFontSmoothing(2);
    }

    void clearTextMatchesNativeRasterAtFractionalScale() {
        // 半透明像素计数不能区分清晰与模糊，直接对照原生字号栅格化结果。
        const QString text = QStringLiteral("添加小组件 时钟 28.4 Aa");
        for (int level : {2, 3}) {
            DesktopWidget::setGlobalFontSmoothing(level);
            for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
                for (qreal uiScale : {.7, 1.0}) {
                    for (int size : {12, 16, 28}) {
                        QImage actual(1100, 160, QImage::Format_ARGB32_Premultiplied);
                        actual.setDevicePixelRatio(dpr);
                        actual.fill(Qt::transparent);
                        QFont font(DesktopWidget::pingFangFontFamily());
                        font.setPixelSize(size);
                        DesktopWidget::applyFontSmoothing(font);
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
        DesktopWidget::setGlobalFontSmoothing(2);
    }

    void galleryPreviewsUseNativePixels() {
        DesktopWidget::setGlobalFontSmoothing(3);
        const QSize box(204, 128);
        for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
            const QImage image = DesktopWidget::renderPreview(
                DesktopWidget::CardKind::Dictionary, 0, box, dpr);
            QVERIFY(!image.isNull());
            QCOMPARE(image.devicePixelRatio(), dpr);
            QCOMPARE(image.size(), QSize(qRound(128 * dpr), qRound(128 * dpr)));
        }
        WidgetLibraryDialog library;
        library.selectCategory(1);
        // 布局和层级变化可能调整 QObject 子对象次序。
        auto* tile = library.m_tiles.first();
        QVERIFY(tile);
        const qreal dpr = tile->devicePixelRatioF();
        const QImage reference = DesktopWidget::renderPreview(
            DesktopWidget::CardKind::Battery, 0, tile->size(), dpr);
        QImage rendered(tile->size() * dpr, reference.format());
        rendered.setDevicePixelRatio(dpr);
        rendered.fill(Qt::transparent);
        QPainter tilePainter(&rendered);
        tile->render(&tilePainter, QPoint(), QRegion(), QWidget::DrawChildren);
        tilePainter.end();
        rendered.save(QStringLiteral("artifacts/preview-native-tile.png"));
        reference.save(QStringLiteral("artifacts/preview-native-reference.png"));
        QCOMPARE(rendered, reference);
        for (auto* nav : library.m_navButtons) {
            const QPixmap icon = nav->icon().pixmap(QSize(27, 27), 2.0);
            QCOMPARE(icon.size(), QSize(54, 54));
            QCOMPARE(icon.devicePixelRatio(), 2.0);
        }
        DesktopWidget::setGlobalFontSmoothing(2);
    }

    void galleryDictionaryTextAppearance() {
        DesktopWidget::setGlobalFontSmoothing(3);
        WidgetLibraryDialog library;
        library.prepareBackdrop();
        library.selectCategory(4);
        library.show();
        QVERIFY(QTest::qWaitForWindowExposed(&library));
        QTest::qWait(400);
        QVERIFY(library.grab().save(QStringLiteral("artifacts/gallery-dictionary-native.png")));
        QCOMPARE(qApp->font().family(), DesktopWidget::pingFangFontFamily());
        for (QWidget* control : library.findChildren<QWidget*>())
            QCOMPARE(control->font().family(), DesktopWidget::pingFangFontFamily());
        library.hide();
        DesktopWidget::setGlobalFontSmoothing(2);
    }

    void fontSmoothingReachesGalleryControls() {
        DesktopWidget::setGlobalFontSmoothing(0);
        WidgetLibraryDialog library;
        QFont menuFont(DesktopWidget::pingFangFontFamily());
        menuFont.setPixelSize(16);
        DesktopWidget::applyFontSmoothing(menuFont);
        LiquidGlassMenu menu(menuFont);
        auto* nav = library.findChild<QPushButton*>(QStringLiteral("navSelected"));
        auto* search = library.findChild<QLineEdit*>();
        QVERIFY(nav);
        QVERIFY(search);
        for (int level = 0; level <= 3; ++level) {
            DesktopWidget::setGlobalFontSmoothing(level);
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
        DesktopWidget::setGlobalFontSmoothing(2);
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
        QCOMPARE(glass.m_backdropTimer->interval(), qMax(1, glass.activeDisplayInterval() / 2));
        QVERIFY(glass.m_backdropTimer->isActive());
        QColor observed;
        QTRY_VERIFY_WITH_TIMEOUT((observed = centerPixel(glass), observed.red() > 170), 3000);
        background.color = QColor(20, 205, 45);
        background.repaint();
        QTRY_VERIFY_WITH_TIMEOUT((observed = centerPixel(glass),
                                  observed.green() > 155 && observed.red() < 120), 3000);
        // 合法的纯色帧不能被捕获空洞检测误判为失败并替换成旧帧。
        for (const QColor color : {QColor(Qt::black), QColor(Qt::white), QColor(20, 40, 210)}) {
            background.color = color;
            background.repaint();
            QTRY_VERIFY_WITH_TIMEOUT((observed = centerPixel(glass),
                qAbs(observed.red() - color.red()) < 60
                && qAbs(observed.green() - color.green()) < 60
                && qAbs(observed.blue() - color.blue()) < 60), 3000);
        }
    }

    void stationaryLiveRenderingIsDrivenByBackgroundChanges() {
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
        qInfo() << "Unchanged live surface frames in 250 ms:" << glass.frameRevision() - revision;
        QCOMPARE(glass.frameRevision(), revision);
        QImage changed(200, 140, QImage::Format_RGB32);
        changed.fill(Qt::cyan);
        glass.setBackgroundImage(changed);
        QTRY_VERIFY_WITH_TIMEOUT(glass.frameRevision() > revision, 1000);

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
        QTRY_VERIFY_WITH_TIMEOUT(glass.frameRevision() > revision, 1500);
        glass.setMouseThroughEnabled(true);
        glass.setLowPowerRefreshEnabled(true);
        QCOMPARE(glass.refreshInterval(), 250);
        glass.setLiveBackdropEnabled(false);
    }

    void desktopWidgetFollowsLiveWallpaperHost() {
#ifndef Q_OS_WIN
        QSKIP("Requires a native Windows wallpaper host");
#else
        // 模拟动态壁纸的桌面宿主，验证不会直接回退到静态壁纸文件。
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
        DesktopWidget widget(nullptr, DesktopWidget::CardKind::Battery, false,
                             QStringLiteral("live-wallpaper-probe"), 0);
        // 隔离用户已经打开的桌面卡片。
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
        const int firstPaintedFrame = background.paintedFrames;
        QTest::qWait(2000);
        animation.stop();
        // 排空最后两次在途捕获，再比较已经实际绘制的源帧，而非定时器请求次数。
        QTest::qWait(glass.activeDisplayInterval() * 2);
        const int actualSourceFrames = background.paintedFrames - firstPaintedFrame;
        qInfo() << "Stationary backdrop:" << changedFrames << "changes /"
                << actualSourceFrames << "painted source frames;" << sourceFrames << "requested frames";
        QVERIFY(changedFrames >= qMax(20, actualSourceFrames / 2));
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
            // 保持请求在接收窗口销毁时仍然运行，验证回调生命周期。
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
        QTest::qWait(3000); // 预热驱动、着色器和捕获池。
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
        ColorWindow background;
        background.color = QColor(14, 22, 32);
        background.setGeometry(QRect(QGuiApplication::primaryScreen()->availableGeometry().center()
                                     - QPoint(250, 170), QSize(500, 340)));
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));
        DesktopWidget lower(nullptr, static_cast<DesktopWidget::CardKind>(lowerKind), false);
        DesktopWidget upper(nullptr, static_cast<DesktopWidget::CardKind>(upperKind), false);
        for (DesktopWidget* widget : {&lower, &upper}) {
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
        // 验证真实电量、天气、时钟与词典内容，包含首帧后未再次重绘的卡片。
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
        upper.setLowPowerRefreshEnabled(true); // 外部捕获间隔为 250 毫秒。
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
        // 显式建立背景与玻璃的原生层级，不依赖普通工具窗口显示后的默认位置。
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
            // 验证包括遮挡半边在内的整个着色器输入，折射可能将这些像素带到可见边缘。
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
        // 系统截图允许包含组件，但内部背景必须排除目标卡片与前景，保留未遮挡桌面。
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
        DesktopWidget owner(nullptr, DesktopWidget::CardKind::Clock, false);
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
            DesktopWidget added(nullptr, DesktopWidget::CardKind::Weather, false);
            added.move(owner.pos() + QPoint(200, 0));
            added.show();
            QVERIFY(QTest::qWaitForWindowExposed(&added));
            QVERIFY(!NativeWindows::isTopmost(added.winId()));
            // 图库打开时，拖动卡片仍需位于图库下方。
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

    void libraryHidesApplicationsForWholeSessionAndRestoresOnExit() {
#ifdef Q_OS_WIN
        AppSettings().setValue("appearance/liveBackdrop", false);
        QWidget application;
        application.setWindowTitle(QStringLiteral("Gallery restore regression"));
        application.resize(280, 160);
        application.show();
        QVERIFY(QTest::qWaitForWindowExposed(&application));
        const HWND window = reinterpret_cast<HWND>(application.winId());
        QWidget frameless;
        frameless.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        frameless.setWindowTitle(QStringLiteral("Frameless gallery restore regression"));
        frameless.resize(220, 140);
        frameless.show();
        QVERIFY(QTest::qWaitForWindowExposed(&frameless));
        const HWND framelessWindow = reinterpret_cast<HWND>(frameless.winId());
        QWidget alreadyHidden;
        alreadyHidden.winId();
        TestGlass card;
        card.setDesktopCaptureEnabled(false);
        card.show();
        QVERIFY(QTest::qWaitForWindowExposed(&card));
        {
            WidgetLibraryDialog library;
            library.prepareBackdrop();
            QTRY_VERIFY_WITH_TIMEOUT(!IsWindowVisible(window), 1000);
            QTRY_VERIFY_WITH_TIMEOUT(!IsWindowVisible(framelessWindow), 1000);
            library.show();
            QVERIFY(QTest::qWaitForWindowExposed(&library));
            QTest::qWait(350);
            QVERIFY(card.isVisible());
            QVERIFY(!IsIconic(window));
            library.notifyWidgetDragStarted(0);
            library.notifyWidgetDragFinished(0);
            QVERIFY(!IsWindowVisible(window));
            library.closeGallery();
            QTRY_VERIFY_WITH_TIMEOUT(!library.isVisible(), 1000);
            QTRY_VERIFY_WITH_TIMEOUT(IsWindowVisible(window), 1000);
            QTRY_VERIFY_WITH_TIMEOUT(IsWindowVisible(framelessWindow), 1000);
            library.prepareBackdrop();
            QTRY_VERIFY_WITH_TIMEOUT(!IsWindowVisible(window), 1000);
        }
        QTRY_VERIFY_WITH_TIMEOUT(IsWindowVisible(window), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(IsWindowVisible(framelessWindow), 1000);
        QVERIFY(!IsWindowVisible(reinterpret_cast<HWND>(alreadyHidden.winId())));
#endif
    }

    void librarySearchesAllVariantsAndUsesNaturalGridSizes() {
        AppSettings().setValue("appearance/scale", 1.0);
        WidgetLibraryDialog library;
        QCOMPARE(library.m_tiles.first()->size(), QSize(library.scaled(128), library.scaled(128)));
        QCOMPARE(library.m_tiles.at(4)->size(), QSize(library.scaled(128) * 2 + library.scaled(12), library.scaled(128)));
        QCOMPARE(library.m_tiles.at(5)->size(), QSize(library.scaled(128) * 2 + library.scaled(12), library.scaled(128) * 2 + library.scaled(12)));
        // 所有组件分区保留全部变体，搜索不受建议列表限制。
        for (QFrame* tile : library.m_tiles)
            QVERIFY(library.m_tileLayout->indexOf(tile) >= 0);
        library.m_search->setText(QStringLiteral("词典详情"));
        QCOMPARE(library.m_tileLayout->count(), 1);
        QCOMPARE(library.m_tileLayout->itemAt(0)->widget(), library.m_tiles.at(10));
        library.m_search->clear();
        library.resize(600, 440);
        library.applyTileFilter();
        for (QFrame* tile : library.m_tiles) {
            int row, col, rows, cols;
            library.m_tileLayout->getItemPosition(library.m_tileLayout->indexOf(tile), &row, &col, &rows, &cols);
            QVERIFY(col + cols <= library.tileColumnCount());
        }
    }

    void libraryHonorsGlobalLiveBackdropSetting() {
        AppSettings().setValue(QStringLiteral("appearance/liveBackdrop"), false);
        WidgetLibraryDialog staticLibrary;
        QVERIFY(!staticLibrary.liveBackdropEnabled());
        QCOMPARE(staticLibrary.m_blurRadius, LiquidGlassStyle::kBlurRadius);
        QCOMPARE(staticLibrary.glassOpacity(), 1.0f);
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
        const int interval = liveLibrary.m_liveBackdropTimer.interval();
        QVERIFY(interval == liveLibrary.activeDisplayInterval()
                || interval == qMax(1, liveLibrary.activeDisplayInterval() / 2)
                || interval == qMax(100, liveLibrary.activeDisplayInterval()));
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
        // 以源窗口实际产生的帧为基准比较捕获速度，不假定测试进程收到所有显示器节拍。
        const int expectedSourceSamples = qMax(1, sourceFrames);
        qInfo() << "Library animated backdrop frames:" << changedFrames
                << "changed," << capturedSamples << "samples /" << sourceFrames
                << "source frames at" << interval << "ms cadence in"
                << sampleMs << "ms";
        QVERIFY2(capturedSamples >= expectedSourceSamples * 4 / 5,
                 qPrintable(QStringLiteral("library sampled only %1/%2 emitted source frames")
                                .arg(capturedSamples).arg(expectedSourceSamples)));
        // 允许 DWM 和 WGC 合并相邻帧，要求持续获取变化画面，而非每个节拍独立回读。
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
            // 原生提升必须在呈现前重定向，不能依靠下一轮事件循环修复。
            QVERIFY(belowApp());
        }
        first.raise();
        QVERIFY(belowApp());
        // 即使禁用捕获，拓扑监听也必须修复绕过窗口位置消息的层级变更。
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
        DesktopWidget::setGlobalFontSmoothing(3);
        QFont font(DesktopWidget::pingFangFontFamily());
        font.setPixelSize(16);
        DesktopWidget::applyFontSmoothing(font);
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
        QFont font(DesktopWidget::pingFangFontFamily());
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
        // 工作线程仍在采样时销毁菜单，验证对象上下文和请求代次保护失效接收者。
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
        for (auto kind : {DesktopWidget::CardKind::Clock, DesktopWidget::CardKind::Weather}) {
            DesktopWidget card(nullptr, kind, false, QStringLiteral("context-menu-test"), 0);
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
        DesktopWidget host(nullptr, DesktopWidget::CardKind::Clock, false,
                           QStringLiteral("menu-library-host"), 0);
        DesktopWidget guest(nullptr, DesktopWidget::CardKind::Weather, false,
                            QStringLiteral("menu-library-guest"), 0);
        host.m_ownsTray = true;
        guest.showWidgetLibrary();
        QVERIFY(host.m_library && host.m_library->isVisible());
        QVERIFY(!guest.m_library);
        auto* shared = host.m_library;
        guest.showWidgetLibrary();
        QCOMPARE(host.m_library, shared);
        host.m_library->hide();
    }

    void libraryCancelDoesNotDrop() {
        DesktopWidget::setGlobalFontSmoothing(3);
        WidgetLibraryDialog library;
        library.prepareBackdrop();
        QScreen* libraryScreen = QGuiApplication::screenAt(QCursor::pos());
        if (!libraryScreen)
            libraryScreen = QGuiApplication::primaryScreen();
        QVERIFY(libraryScreen);
        // 首次显示必须位于工作区之外，避免打开动画前闪现完整面板。
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
        DesktopWidget::setGlobalFontSmoothing(2);
    }

    void libraryFollowsAppearanceScale() {
        AppSettings().setValue("appearance/scale", 0.7);
        WidgetLibraryDialog library;
        auto* tile = library.findChild<QFrame*>("widgetTile");
        auto* search = library.findChild<QLineEdit*>();
        auto* footer = library.findChild<QFrame*>("footer");
        QVERIFY(tile && search && footer);
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const auto expectedSize = [screen](qreal scale) {
            const qreal fitted = WidgetLibraryDialog::fittedScaleForArea(screen->availableGeometry(), scale);
            return QSize(qRound(1980 * fitted), qRound(1200 * fitted));
        };
        const QSize expected = expectedSize(.7);
        QCOMPARE(library.size(), expected);
        const QSize tileSize = tile->size();
        QCOMPARE(search->height(), library.scaled(36));
        QCOMPARE(tileSize.width(), library.scaled(128));
        const qreal interfaceScale = library.m_interfaceScale;
        for (qreal desktopScale : {.4, .7, 1.25, 2.0}) {
            AppSettings().setValue("appearance/scale", desktopScale);
            WidgetLibraryDialog other;
            QCOMPARE(other.size(), expectedSize(desktopScale));
            QCOMPARE(other.m_tiles.first()->width(), other.scaled(128));
            QCOMPARE(other.m_search->height(), other.scaled(36));
            QCOMPARE(other.m_footer->height(), other.scaled(62));
            QVERIFY(other.m_interfaceScale <= desktopScale);
            if (desktopScale == .4) {
                QCOMPARE(other.size(), QSize(792, 480));
                QCOMPARE(other.m_search->height(), 22);
                QCOMPARE(other.m_tiles.first()->size(), QSize(77, 77));
            }
        }
        AppSettings().setValue("appearance/scale", .7);
        library.prepareBackdrop();
        library.show();
        QVERIFY(QTest::qWaitForWindowExposed(&library));
        QTest::qWait(400);
        QVERIFY(screen->availableGeometry().contains(library.geometry()));
        const QImage gallery = library.grab().toImage().convertToFormat(QImage::Format_ARGB32);
        QVERIFY(gallery.save("artifacts/gallery-unified-scale-70.png"));
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                QCOMPARE(qAlpha(gallery.pixel(x, y)), 0);
                QCOMPARE(qAlpha(gallery.pixel(gallery.width() - 1 - x, y)), 0);
                QCOMPARE(qAlpha(gallery.pixel(x, gallery.height() - 1 - y)), 0);
                QCOMPARE(qAlpha(gallery.pixel(gallery.width() - 1 - x, gallery.height() - 1 - y)), 0);
            }
        }
        library.hide();
        AppSettings().setValue("appearance/scale", .7);
        DesktopWidget owner(nullptr, DesktopWidget::CardKind::Clock, false,
                            QStringLiteral("scale-library-owner"), 0);
        owner.m_ownsTray = true;
        owner.showWidgetLibrary();
        QVERIFY(owner.m_library);
        auto* shared = owner.m_library;
        QCOMPARE(shared->m_interfaceScale, interfaceScale);
        const QSize initialCardSize = owner.size();
        owner.applyScale(1.25);
        QVERIFY(owner.size() != initialCardSize);
        QCOMPARE(owner.m_library, shared);
        QCOMPARE(shared->size(), expectedSize(1.25));
        QVERIFY(shared->m_interfaceScale > interfaceScale);
        owner.syncGroupSettings(.7, false, false, false, 1.25, 2);
        QCOMPARE(shared->m_blurRadius, LiquidGlassStyle::kBlurRadius);
        QCOMPARE(shared->glassOpacity(), 1.0f);
        QCOMPARE(shared->renderScale(), 1.25f);
        QCOMPARE(shared->m_tiles.first()->size(), tileSize);
        QTest::qWait(400);
        QCOMPARE(shared->size(), expected);
        auto* scroll = shared->findChild<QScrollArea*>();
        QVERIFY(scroll);
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        QVERIFY(shared->grab().save("artifacts/gallery-unified-scale-updated.png"));
        shared->hide();
    }

    void libraryScaleFitsDifferentWorkAreas() {
        for (const QRect& area : {QRect(0, 0, 1280, 720), QRect(-1920, 0, 1920, 1080),
                                 QRect(0, 0, 3840, 2160), QRect(0, 0, 3440, 1440),
                                 QRect(0, 0, 800, 600), QRect(0, 0, 320, 240)}) {
            QSize previous;
            for (qreal requested : {.4, .7, 1.0, 1.25, 2.0}) {
                const qreal fitted = WidgetLibraryDialog::fittedScaleForArea(area, requested);
                const QSize size(qRound(1980 * fitted), qRound(1200 * fitted));
                QVERIFY(size.width() <= area.width() - 32);
                QVERIFY(size.height() <= area.height() - 32);
                QVERIFY(fitted <= requested);
                QVERIFY(size.width() >= previous.width());
                QVERIFY(size.height() >= previous.height());
                if (1980 * requested <= area.width() - 32
                    && 1200 * requested <= area.height() - 32) {
                    QCOMPARE(fitted, requested);
                    QCOMPARE(size, QSize(qRound(1980 * requested), qRound(1200 * requested)));
                }
                QVERIFY(qAbs(size.width() * 1200 - size.height() * 1980) <= 1590);
                previous = size;
            }
        }
    }

    void hiddenConfigurationRoundTrip() {
        AppSettings().setValue("appearance/scale", 0.4);
        const QPoint point = QGuiApplication::primaryScreen()->availableGeometry().center();
        auto* widget = DesktopWidget::addWidget(DesktopWidget::CardKind::Clock, point);
        QVERIFY(widget);
        const QPoint saved = widget->pos();
        widget->hide();
        widget->saveConfiguration();
        delete widget;
        const auto restored = DesktopWidget::restoreWidgets();
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

        const auto restored = DesktopWidget::restoreWidgets();
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
        // 重新指定原生父窗口可能销毁 GL 上下文。
        QWidget parent;
        parent.resize(300, 220);
        parent.show();
        glass.setParent(&parent);
        glass.move(20, 20);
        glass.show();
        QTest::qWait(100);
        QVERIFY(glass.isValid());
        QVERIFY(centerPixel(glass).red() > 130);
        glass.setParent(nullptr); // 栈上子对象必须比临时父窗口存活更久。
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

    void gaussianRadiusSoftensBackdrop() {
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
                glass.setBlurRadius(0.5f + strength / 100.0f * 9.5f);
                glass.setBlurIterations(1 + qRound(strength / 100.0f * 2.0f));
                glass.setRefractionPower(0); // 单独测量模糊，排除镜头折射干扰。
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
        DesktopWidget card(nullptr, DesktopWidget::CardKind::Weather, false,
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
                QString::number(31 - i), 1});
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
            DesktopWidget::setGlobalFontSmoothing(level);
            card.update();
            QTest::qWait(70);
            const QImage frame = card.grabFramebuffer();
            QVERIFY(frame.save(QStringLiteral("artifacts/actual-weather-aa-%1.png")
                               .arg(level)));
            // 按横向天气卡片的归一化温度区域测量数字边缘覆盖，排除高亮字形内部。
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
        const QString referencePath = qEnvironmentVariable("WIDGET_GLASS_REFERENCE_FRAME");
        if (!referencePath.isEmpty()) {
            QImage comparison(referencePath);
            QVERIFY(!comparison.isNull());
            const QRect cardBounds(184, 208, 688, 328);
            card.m_widgetScale = 1.0 / card.devicePixelRatioF();
            card.setGlassRadius(LiquidGlassStyle::kCardCornerRadius * card.m_widgetScale);
            card.setFixedSize(QSize(qRound(cardBounds.width() / card.devicePixelRatioF()),
                qRound(cardBounds.height() / card.devicePixelRatioF())));
            card.m_weatherLocation = QStringLiteral("天心区");
            card.m_weatherTemperature = QStringLiteral("21");
            card.m_weatherDescription = QStringLiteral("晴朗无云");
            card.m_weatherHigh = QStringLiteral("30");
            card.m_weatherLow = QStringLiteral("19");
            card.m_weatherCode = 0;
            card.m_weatherHours.clear();
            for (int hour = 0; hour < 6; ++hour)
                card.m_weatherHours.append({QString::number(2 + hour) + QStringLiteral("时"),
                    QString::number(hour == 0 || hour == 5 ? 21 : 20), 0, hour < 5});
            card.setBackgroundImage(comparison.copy(cardBounds));
            DesktopWidget::setGlobalFontSmoothing(4);
            const QImage frame = card.grabFramebuffer();
            QPainter preview(&comparison);
            preview.drawImage(cardBounds, frame);
            preview.end();
            QVERIFY(comparison.save(QStringLiteral("artifacts/optical-glass-weather-reference.png")));
            PipelineProbe material;
            material.resize(card.size());
            material.addGlassObject(QPointF(), material.size());
            material.setGlassObjectCornerRadius(0, float(card.m_widgetScale * LiquidGlassStyle::kCardCornerRadius));
            material.setRenderScale(1.5f);
            material.setNoiseAmount(0);
            material.setBackgroundImage(QImage(referencePath).copy(cardBounds));
            material.show();
            QVERIFY(QTest::qWaitForWindowExposed(&material));
            QVERIFY(material.grabFramebuffer().save(QStringLiteral("artifacts/reference-material-only.png")));
            const QString galleryPath = qEnvironmentVariable("WIDGET_GLASS_GALLERY_FRAME");
            if (!galleryPath.isEmpty()) {
                material.hide();
                card.hide();
                AppSettings().setValue("appearance/scale", .4);
                AppSettings().setValue("appearance/liveBackdrop", false);
                WidgetLibraryDialog library;
                library.setDesktopCaptureEnabled(false);
                library.setRenderScale(1.5f);
                library.setNoiseAmount(0);
                library.show();
                QVERIFY(QTest::qWaitForWindowExposed(&library));
                QTest::qWait(350);
                library.resize(780, 400);
                library.m_recommendedTiles = {7, 0, 4, 9, 12};
                library.m_selectedCategory = -1;
                library.applyTileFilter();
                const QImage backdrop(galleryPath);
                QVERIFY(!backdrop.isNull());
                library.setBackgroundImage(backdrop);
                QTest::qWait(50);
                QVERIFY(library.grab().save(QStringLiteral("artifacts/reference-gallery-window.png")));
                library.hide();
            }
        }
        DesktopWidget::setGlobalFontSmoothing(2);
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
            DesktopWidget::setGlobalFontSmoothing(level);
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
        // 背景画质不能改变文字最终像素覆盖率。
        const QImage nativeText = glass.grabFramebuffer();
        glass.setRenderScale(.5f);
        QTest::qWait(60);
        QCOMPARE(glass.grabFramebuffer(), nativeText);
        DesktopWidget::setGlobalFontSmoothing(2);
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

    void renderDiagnosticsAreThreadSafeBoundedAndPreserveQtMessages() {
        QTemporaryDir logs;
        QVERIFY(logs.isValid());
        QVERIFY(RenderDiagnostics::initialize(logs.path()));
        const auto cleanup = qScopeGuard([]() { RenderDiagnostics::shutdown(); });
        QCOMPARE(RenderDiagnostics::logPath(), logs.filePath("render.log"));
        RenderDiagnostics::write("unicode", QStringLiteral("托盘渲染\nsecond line"));
        QTest::ignoreMessage(QtWarningMsg, "render diagnostic warning probe");
        qWarning("render diagnostic warning probe");
        QList<std::shared_ptr<std::thread>> writers;
        for (int thread = 0; thread < 4; ++thread)
            writers.append(std::make_shared<std::thread>([thread]() {
                for (int index = 0; index < 60; ++index)
                    RenderDiagnostics::write("concurrent", QString("writer=%1 item=%2").arg(thread).arg(index));
            }));
        for (const auto& writer : writers)
            writer->join();
        QFile current(RenderDiagnostics::logPath());
        QVERIFY(current.open(QIODevice::ReadOnly));
        const QByteArray records = current.readAll();
        current.close();
        QCOMPARE(records.count("[concurrent]"), 240);
        QVERIFY(records.contains(QStringLiteral("托盘渲染\\nsecond line").toUtf8()));
        QVERIFY(records.contains("render diagnostic warning probe"));
        for (int index = 0; index < 1000; ++index)
            RenderDiagnostics::write("throttled", "repeat", 10000);
        QVERIFY(current.open(QIODevice::ReadOnly));
        QCOMPARE(current.readAll().count("[throttled]"), 1);
        current.close();
        for (int index = 0; index < 2200; ++index)
            RenderDiagnostics::write("rotation", QString(4096, QLatin1Char('x')));
        RenderDiagnostics::shutdown();
        QVERIFY(!RenderDiagnostics::enabled());
        for (const QString& file : {QString("render.log"), QString("render.log.1"),
                                    QString("render.log.2"), QString("render.log.3")}) {
            const QFileInfo info(logs.filePath(file));
            QVERIFY(info.exists());
            QVERIFY(info.size() <= 2 * 1024 * 1024);
        }
        QVERIFY(!QFile::exists(logs.filePath("render.log.4")));
        // 日志初始化失败不能替换 QtTest 消息处理器。
        QFile blocked(logs.filePath("not-a-directory"));
        QVERIFY(blocked.open(QIODevice::WriteOnly));
        blocked.close();
        QVERIFY(!RenderDiagnostics::initialize(blocked.fileName()));
        QTest::ignoreMessage(QtWarningMsg, "message handler after failed log open");
        qWarning("message handler after failed log open");
    }

    void renderDiagnosticsRecordRendererErrorsAndHeartbeat() {
        QTemporaryDir logs;
        QVERIFY(RenderDiagnostics::initialize(logs.path()));
        const auto cleanup = qScopeGuard([]() { RenderDiagnostics::shutdown(); });
        TestGlass glass;
        glass.setDesktopCaptureEnabled(false);
        glass.overlay = QColor(25, 220, 50);
        glass.setAnimationEnabled(true);
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        glass.makeCurrent();
        glass.context()->functions()->glEnable(GLenum(0xffffffff));
        glass.reportGlErrors("diagnostic-probe");
        glass.doneCurrent();
        QTest::qWait(5200);
        QVERIFY(!glass.surfaceSnapshot().isNull());
        QFile current(RenderDiagnostics::logPath());
        QVERIFY(current.open(QIODevice::ReadOnly));
        const QByteArray records = current.readAll();
        current.close();
        QVERIFY(records.contains("[gl.context.create]"));
        QVERIFY(records.contains("renderer="));
        QVERIFY(records.contains("DPR="));
        QVERIFY(records.contains("[gl.fbo.create]"));
        QVERIFY(records.contains("diagnostic-probe"));
        QVERIFY(records.contains("code=0x500"));
        QVERIFY(records.contains("[render.heartbeat]"));
        QVERIFY(QFile::exists(logs.filePath("render.log")));
        QFile::remove("artifacts/render-diagnostics-sample.log");
        QVERIFY(QFile::copy(logs.filePath("render.log"), "artifacts/render-diagnostics-sample.log"));
    }

    void backgroundUploadHandlesPaddedRowsAndForeignState() {
        TestGlass glass;
        glass.setDesktopCaptureEnabled(false);
        glass.setFixedSize(201, 141);
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        const int stride = 128 * 4;
        QByteArray storage(stride * 97, char(0x55));
        QImage padded(reinterpret_cast<uchar*>(storage.data()), 101, 97, stride,
                      QImage::Format_RGB32);
        for (int y = 0; y < padded.height(); ++y)
            for (int x = 0; x < padded.width(); ++x)
                padded.setPixel(x, y, qRgb(40 + x, 60 + y, (x + y) % 100));
        glass.setBackgroundImage(padded.copy());
        const QImage reference = glass.grabFramebuffer();
        glass.setBackgroundImage(padded);
        QCOMPARE(glass.grabFramebuffer(), reference);

        glass.makeCurrent();
        auto* gl = glass.context()->extraFunctions();
        gl->glActiveTexture(GL_TEXTURE1);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 104);
        gl->glPixelStorei(GL_UNPACK_SKIP_PIXELS, 1);
        gl->glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
        // 直接测试纹理上传，防止 Qt 提前重置外部状态；填充内存保证错误读取也不会越界。
        glass.m_bgDirty = true;
        glass.loadBackgroundTexture();
        GLint value = 0;
        gl->glGetIntegerv(GL_UNPACK_ROW_LENGTH, &value);
        QCOMPARE(value, 104);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl->glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        gl->glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        gl->glActiveTexture(GL_TEXTURE0);
        glass.doneCurrent();
        QCOMPARE(glass.grabFramebuffer(), reference);
    }

    void snapshotIgnoresForeignPixelPackBuffer() {
        TestGlass glass;
        glass.setDesktopCaptureEnabled(false);
        glass.overlay = QColor(25, 220, 50);
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        const QImage expected = glass.surfaceSnapshot();
        QVERIFY(!expected.isNull());
        glass.makeCurrent();
        auto* gl = glass.context()->extraFunctions();
        GLuint buffer = 0;
        gl->glGenBuffers(1, &buffer);
        gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
        gl->glBufferData(GL_PIXEL_PACK_BUFFER, 1024 * 1024, nullptr, GL_STREAM_READ);
        QOpenGLFramebufferObject foreignTarget(16, 16);
        foreignTarget.bind();
        glass.m_snapshotRevision = 0;
        const QImage actual = glass.surfaceSnapshot();
        GLint binding = 0, framebuffer = 0;
        gl->glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &binding);
        gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        gl->glDeleteBuffers(1, &buffer);
        while (gl->glGetError() != GL_NO_ERROR) {}
        glass.doneCurrent();
        QCOMPARE(GLuint(binding), buffer);
        QCOMPARE(GLuint(framebuffer), foreignTarget.handle());
        QCOMPARE(actual, expected);
    }

    void sceneRecoversForeignDrawState() {
        TestGlass glass;
        glass.setDesktopCaptureEnabled(false);
        QImage background(200, 140, QImage::Format_RGB32);
        background.fill(QColor(160, 80, 30));
        glass.setBackgroundImage(background);
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        const QImage expected = glass.surfaceSnapshot();
        glass.makeCurrent();
        auto* gl = glass.context()->extraFunctions();
        gl->glClearColor(1, 0, 1, 1);
        gl->glClear(GL_COLOR_BUFFER_BIT);
        gl->glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        gl->glEnable(GL_DEPTH_TEST);
        gl->glEnable(GL_STENCIL_TEST);
        gl->glStencilFunc(GL_NEVER, 0, 0xff);
        gl->glEnable(GL_SCISSOR_TEST);
        gl->glScissor(0, 0, 1, 1);
        gl->glActiveTexture(GL_TEXTURE1);
        gl->glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        glass.m_blurCacheDirty = true;
        glass.paintGL();
        const QImage actual = glass.surfaceSnapshot();
        glass.doneCurrent();
        QCOMPARE(actual, expected);
    }

    void popupRecompositionPreservesMultipleGlassWindows() {
        TestGlass left, right;
        const QPoint origin = QGuiApplication::primaryScreen()->availableGeometry().center();
        QList<QImage> expected;
        int index = 0;
        for (auto* glass : {&left, &right}) {
            glass->setDesktopCaptureEnabled(false);
            glass->move(origin + QPoint(index * 215 - 210, -70));
            glass->setRenderScale(index ? 1.25f : .6f);
            glass->overlay = index ? QColor(25, 220, 50) : QColor(220, 50, 25);
            QImage background(200, 140, QImage::Format_RGB32);
            background.fill(QColor(40, 60, 90));
            glass->setBackgroundImage(background);
            glass->show();
            QVERIFY(QTest::qWaitForWindowExposed(glass));
            expected.append(glass->grabFramebuffer());
            ++index;
        }
        QMenu popup;
        popup.addAction(QStringLiteral("Tray list composition regression"));
        popup.addAction(QStringLiteral("Second tray item"));
        for (int cycle = 0; cycle < 30; ++cycle) {
            popup.popup(origin + QPoint(cycle % 2 ? 20 : -120, -50));
            QTest::qWait(20);
            for (int i = 0; i < 2; ++i) {
                TestGlass* glass = i ? &right : &left;
                glass->setRenderingSuspended(true);
                // 故意污染废弃 FBO，验证菜单改变 DWM 层级时暂停卡片仍完整重绘。
                glass->makeCurrent();
                auto* gl = glass->context()->functions();
                gl->glClearColor(1, 0, 1, 1);
                gl->glClear(GL_COLOR_BUFFER_BIT);
                glass->doneCurrent();
                QCOMPARE(glass->grabFramebuffer(), expected.at(i));
                glass->setRenderingSuspended(false);
            }
            popup.hide();
            QTest::qWait(20);
        }
        QVERIFY(left.grabFramebuffer().save(QStringLiteral("artifacts/tray-popup-left.png")));
        QVERIFY(right.grabFramebuffer().save(QStringLiteral("artifacts/tray-popup-right.png")));
    }

    void suspendedSceneStillPaintsExposedFramebuffer() {
        TestGlass glass;
        glass.setDesktopCaptureEnabled(false);
        glass.overlay = QColor(25, 220, 50);
        QImage background(200, 140, QImage::Format_RGB32);
        background.fill(QColor(160, 80, 30));
        glass.setBackgroundImage(background);
        glass.show();
        QVERIFY(QTest::qWaitForWindowExposed(&glass));
        const QImage expected = glass.grabFramebuffer();
        glass.setRenderingSuspended(true);
        // 模拟显示或重组前 FBO 内容失效，不依赖特定驱动的残留数据。
        glass.makeCurrent();
        auto* gl = glass.context()->functions();
        gl->glClearColor(1, 0, 1, 1);
        gl->glClear(GL_COLOR_BUFFER_BIT);
        glass.doneCurrent();
        QCOMPARE(glass.grabFramebuffer(), expected);
        QVERIFY(!glass.m_timer->isActive());
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
        // 等待遮挡发生前排队的捕获完成，再记录暂停渲染基线。
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
        DesktopWidget weather(nullptr, DesktopWidget::CardKind::Weather, false);
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
        DesktopWidget battery(nullptr, DesktopWidget::CardKind::Battery, false);
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
        DesktopWidget dictionary(nullptr, DesktopWidget::CardKind::Dictionary, false);
        auto* empty = new FakeReply("[]", &dictionary);
        empty->setProperty("dictionarySerial", dictionary.m_dictionaryReplySerial);
        empty->setProperty("dictionaryWord", dictionary.m_dictionaryWord);
        dictionary.handleDictionaryReply(empty);
        QVERIFY(!dictionary.m_dictionaryDefinition.isEmpty());
        QVERIFY(!dictionary.m_dictionaryDefinition.contains(QStringLiteral("读取在线")));
        const int serial = dictionary.m_dictionaryReplySerial;
        const QPoint button(qRound(50 * dictionary.m_widgetScale),
                            dictionary.height() - qRound(36 * dictionary.m_widgetScale));
        mouse(dictionary, QEvent::MouseButtonPress, button, dictionary.pos() + button, Qt::LeftButton, Qt::LeftButton);
        mouse(dictionary, QEvent::MouseButtonRelease, button, dictionary.pos() + button, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(dictionary.m_dictionaryReplySerial, serial + 1);
        DesktopWidget weather(nullptr, DesktopWidget::CardKind::Weather, false);
        weather.m_weatherTemperature = "24";
        auto* invalid = new FakeReply("{\"current_condition\":[{}]}", &weather);
        invalid->setProperty("weatherSerial", weather.m_weatherReplySerial);
        weather.handleWeatherReply(invalid);
        QCOMPARE(weather.m_weatherTemperature, QString("24"));
        QVERIFY(weather.m_weatherDescription.contains(QStringLiteral("更新失败")));
    }
};

QTEST_MAIN(WidgetRegression)
#include "widget_regression.moc"
