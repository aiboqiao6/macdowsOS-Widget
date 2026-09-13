#include "liquidglasswidget.h"

#include <QGuiApplication>
#include <QHideEvent>
#include <QFileInfo>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRegion>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QSettings>
#include <QStringList>
#include <QTimer>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>

namespace {
// QImage is implicitly shared, so keeping one process-wide wallpaper canvas
// avoids a full-screen RGBA allocation per widget (which is especially costly
// on 4K/high-DPI desktops). Each instance still owns its small local crop.
QImage s_sharedWallpaperCanvas;
QString s_sharedWallpaperSignature;
QString s_sharedWallpaperScreenName;
QSize s_sharedWallpaperPixelSize;

// These compositor entry points are resolved dynamically so the same binary
// runs on Windows 10 and Windows 11 without importing APIs that are absent on
// older builds. The shared BlurBehind policy avoids the heavy gray Acrylic
// tint that some Windows 11 revisions apply to transparent OpenGL windows.
using SetWindowCompositionAttributeFn = BOOL (WINAPI *)(HWND, void*);
using DwmSetWindowAttributeFn = HRESULT (WINAPI *)(HWND, DWORD, const void*, DWORD);

struct AccentPolicy {
    int state;
    int flags;
    DWORD gradientColor;
    int animationId;
};

struct WindowCompositionAttributeData {
    int attribute;
    void* data;
    SIZE_T dataSize;
};

constexpr int kWcaAccentPolicy = 19;
constexpr int kAccentDisabled = 0;
constexpr int kAccentEnableBlurBehind = 3;
constexpr DWORD kDwmAttributeSystemBackdropType = 38;
constexpr DWORD kDwmAttributeUseImmersiveDarkMode = 20;
constexpr int kDwmSystemBackdropNone = 1;

void setNativeBackdrop(HWND window, bool enabled, qreal opacity)
{
    if (!window)
        return;

    HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
    auto setDwmAttribute = dwm
        ? reinterpret_cast<DwmSetWindowAttributeFn>(
              GetProcAddress(dwm, "DwmSetWindowAttribute"))
        : nullptr;
    if (setDwmAttribute) {
        // Clear the Win11 backdrop attribute and use the cross-version blur
        // policy below. DWMSBT_TRANSIENT_WINDOW adds a heavy gray acrylic
        // tint on some builds and is the source of the opaque rectangle seen
        // on older cards.
        const int backdrop = kDwmSystemBackdropNone;
        setDwmAttribute(window, kDwmAttributeSystemBackdropType,
                        &backdrop, sizeof(backdrop));
        const BOOL darkMode = TRUE;
        setDwmAttribute(window, kDwmAttributeUseImmersiveDarkMode,
                        &darkMode, sizeof(darkMode));
    }

    // Windows 10 and older Windows 11 builds do not accept
    // DWMWA_SYSTEMBACKDROP_TYPE. Use the documented-in-practice Accent
    // policy as a compatible acrylic/blur-behind fallback.
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto setComposition = user32
        ? reinterpret_cast<SetWindowCompositionAttributeFn>(
              GetProcAddress(user32, "SetWindowCompositionAttribute"))
        : nullptr;
    if (setComposition) {
        AccentPolicy policy{};
        policy.state = enabled ? kAccentEnableBlurBehind : kAccentDisabled;
        // Blur-behind has no acrylic material tint. Keep only a subtle neutral
        // alpha so the wallpaper stays bright instead of becoming a gray slab.
        const int alpha = qBound(0, qRound(0x12 * opacity), 0x20);
        policy.gradientColor = enabled ? (DWORD(alpha) << 24) : 0u;
        WindowCompositionAttributeData data{};
        data.attribute = kWcaAccentPolicy;
        data.data = &policy;
        data.dataSize = sizeof(policy);
        setComposition(window, &data);
    }
}
} // namespace
#endif

LiquidGlassWidget::LiquidGlassWidget(QWidget* parent)
    : QtGlassFlowScene(parent)
{
    setWindowTitle(QStringLiteral("macdowsOS Widget"));
    // Keep the card on the desktop layer.  It remains draggable, but regular
    // application windows stay above it just like a macOS desktop widget.
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus
                   | Qt::WindowStaysOnBottomHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setMouseTracking(true);
    setFixedSize(390, 238);

    configureGlass();
    m_glassObjectIndex = addGlassObject(QPointF(m_glassMarginsX, m_glassMarginsY),
                                        QSizeF(width() - m_glassMarginsX * 2,
                                               height() - m_glassMarginsY * 2),
                                        3.8f);
    setGlassObjectCornerRadius(m_glassObjectIndex, float(m_glassRadius));
    updateWindowMask();

    // Recalculate the wallpaper crop at 60 Hz while the card is moving. When
    // stationary, the coarse sampler only checks for a changed desktop and
    // unchanged frames are skipped, so no foreground window can enter the
    // glass texture without paying for a full capture every frame.
    m_backdropTimer = new QTimer(this);
    // The wallpaper canvas is static while the widget is idle. Poll it once
    // per second to detect an external wallpaper change; dragging switches
    // this timer back to a precise 60 Hz cadence so the glass follows the card
    // without lag. A coarse idle timer avoids a high-resolution system timer
    // on every independent widget window.
    m_backdropTimer->setTimerType(Qt::CoarseTimer);
    m_backdropTimer->setInterval(1000);
    connect(m_backdropTimer, &QTimer::timeout,
            this, &LiquidGlassWidget::captureDesktopBackdrop);
    m_backdropTimer->start();
}

void LiquidGlassWidget::setSystemBlurEnabled(bool enabled)
{
    if (m_systemBlur == enabled) {
        applySystemBlurEffect();
        return;
    }
    m_systemBlur = enabled;
    QSettings settings;
    settings.setValue(QStringLiteral("appearance/systemBlur"), enabled);
    // Keep the same wallpaper-only FBO path in both materials. In blur-only
    // mode the glass shader samples the blurred backdrop at 1:1 (no optical
    // refraction), avoiding the opaque rectangular DWM surface produced by
    // Acrylic on transparent QOpenGLWidget windows.
    setGlassRenderingEnabled(true);
    setBlurOnlyEnabled(enabled);
    updateMaterialParameters();
    applySystemBlurEffect();
    captureDesktopBackdrop();
    update();
}

void LiquidGlassWidget::setMaterialBlurStrength(int percent)
{
    m_blurStrength = qBound(0, percent, 100);
    QSettings settings;
    settings.setValue(QStringLiteral("appearance/blurStrength"), m_blurStrength);
    updateMaterialParameters();
    update();
}

void LiquidGlassWidget::updateMaterialParameters()
{
    const qreal amount = m_blurStrength / 100.0;
    if (m_systemBlur) {
        // Pure blur can use a wider kernel because there is no refracted edge
        // motion to amplify. The iteration count changes in coarse steps so
        // low settings also reduce GPU cost.
        setBlurRadius(float(0.5 + amount * 11.5));
        setBlurIterations(1 + qRound(amount * 4.0));
        setNoiseAmount(0.0f);
        setRefractionPower(0.0f);
    } else {
        setBlurRadius(float(0.5 + amount * 8.5));
        setBlurIterations(1 + qRound(amount * 3.0));
        setNoiseAmount(0.008f);
        setRefractionPower(1.32f);
    }
}

void LiquidGlassWidget::setGlassOpacity(qreal opacity)
{
    m_glassOpacity = qBound<qreal>(0.05, opacity, 1.0);
    QSettings settings;
    settings.setValue(QStringLiteral("appearance/opacity"), m_glassOpacity);
    QtGlassFlowScene::setGlassOpacity(float(m_glassOpacity));
    applySystemBlurEffect();
    update();
}

void LiquidGlassWidget::setMouseThroughEnabled(bool enabled)
{
    if (m_mouseThrough == enabled)
        return;
    m_mouseThrough = enabled;
    QSettings settings;
    settings.setValue(QStringLiteral("interaction/mouseThrough"), enabled);
    setAttribute(Qt::WA_TransparentForMouseEvents, enabled);
    setMouseTracking(!enabled);

    Qt::WindowFlags flags = windowFlags();
    flags.setFlag(Qt::WindowTransparentForInput, enabled);
    const bool wasVisible = isVisible();
    const QRect oldGeometry = geometry();
    setWindowFlags(flags);
    if (wasVisible) {
        setGeometry(oldGeometry);
        show();
        applySystemBlurEffect();
    }
    updateRefreshRate();
}

void LiquidGlassWidget::setLowPowerRefreshEnabled(bool enabled)
{
    m_lowPowerRefresh = enabled;
    QSettings settings;
    settings.setValue(QStringLiteral("interaction/lowPowerRefresh"), enabled);
    updateRefreshRate();
}

void LiquidGlassWidget::setDesktopCaptureEnabled(bool enabled)
{
    if (m_desktopCaptureEnabled == enabled)
        return;
    m_desktopCaptureEnabled = enabled;
    if (m_backdropTimer) {
        if (enabled)
            m_backdropTimer->start();
        else
            m_backdropTimer->stop();
    }
}

void LiquidGlassWidget::updateRefreshRate()
{
    // A transparent, click-through widget has no interaction-driven reason
    // to repaint at 60 FPS. The scene timer sleeps entirely for static cards;
    // this setting only controls the cadence while input/animation is active.
    const bool lowPower = m_mouseThrough && m_lowPowerRefresh;
    const int interval = lowPower ? 250 : 16;
    setRefreshInterval(interval);
    if (m_backdropTimer) {
        const bool dragging = m_dragging && !lowPower;
        m_backdropTimer->setTimerType(dragging ? Qt::PreciseTimer : Qt::CoarseTimer);
        m_backdropTimer->setInterval(dragging ? 16 : 1000);
    }
}

void LiquidGlassWidget::applySystemBlurEffect()
{
#ifdef Q_OS_WIN
    if (!windowHandle() && !isVisible())
        return;
    const HWND window = reinterpret_cast<HWND>(winId());
    // Clear any stale DWM policy left by a previous process version. The
    // visible blur is rendered from the wallpaper FBO so it follows the
    // rounded SDF exactly on both Windows 10 and Windows 11.
    setNativeBackdrop(window, false, m_glassOpacity);
#endif
}

void LiquidGlassWidget::setGlassMargins(int horizontal, int vertical)
{
    m_glassMarginsX = qMax(0, horizontal);
    m_glassMarginsY = vertical < 0 ? m_glassMarginsX : qMax(0, vertical);
    updateGlassObjectGeometry();
    updateWindowMask();
}

void LiquidGlassWidget::setGlassRadius(qreal radius)
{
    m_glassRadius = qMax<qreal>(0.0, radius);
    if (m_glassObjectIndex >= 0)
        setGlassObjectCornerRadius(m_glassObjectIndex, float(m_glassRadius));
    updateWindowMask();
    update();
}

void LiquidGlassWidget::configureGlass(float powerFactor, float refractionPower,
                                       float blurRadius, int blurIterations,
                                       float noiseAmount)
{
    setPowerFactor(powerFactor);
    setRefractionPower(refractionPower);
    setBlurRadius(blurRadius);
    setBlurIterations(blurIterations);
    setNoiseAmount(noiseAmount);
    setAttractionDistance(0.0f);
}

void LiquidGlassWidget::paintGlassSurfaceFrame(QPainter& p, const QRectF& card,
                                                qreal radius) const
{
    // This is deliberately a content-independent edge pass.  Blur, clipping
    // and corner geometry are owned by LiquidGlassWidget/QtGlassFlowScene;
    // concrete widgets only draw their payload inside this shared surface.
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);
    if (m_systemBlur) {
        QPen edge(QColor(255, 255, 255, 62), 1.0);
        edge.setCosmetic(true);
        p.setPen(edge);
        p.drawRoundedRect(card.adjusted(.75, .75, -.75, -.75),
                          qMax<qreal>(0.0, radius - .75),
                          qMax<qreal>(0.0, radius - .75));
    } else {
        // One restrained directional rim is enough: the shader already owns
        // the SDF alpha edge. The previous base stroke + gradient stroke +
        // conical stroke + top line stacked into a blue/white double border,
        // especially along the lower-right edge.
        QLinearGradient rim(card.topLeft(), card.bottomRight());
        rim.setColorAt(0.00, QColor(255, 255, 255, 105));
        rim.setColorAt(0.22, QColor(245, 250, 255, 54));
        rim.setColorAt(0.52, QColor(220, 232, 246, 18));
        rim.setColorAt(0.78, QColor(224, 238, 252, 28));
        rim.setColorAt(1.00, QColor(255, 255, 255, 62));
        QPen edge(QBrush(rim), 1.0);
        edge.setCosmetic(true);
        p.setPen(edge);
        p.drawRoundedRect(card.adjusted(.75, .75, -.75, -.75),
                          qMax<qreal>(0.0, radius - .75),
                          qMax<qreal>(0.0, radius - .75));

        p.save();
        QPainterPath tintPath;
        tintPath.addRoundedRect(card, radius, radius);
        p.setClipPath(tintPath);
        QLinearGradient gloss(card.topLeft(),
                              QPointF(card.left(), card.top() + card.height() * .40));
        gloss.setColorAt(0.0, QColor(255, 255, 255, 22));
        gloss.setColorAt(0.30, QColor(255, 255, 255, 5));
        gloss.setColorAt(1.0, QColor(255, 255, 255, 0));
        p.fillRect(card, gloss);
        p.restore();
    }
    p.restore();
}

void LiquidGlassWidget::updateGlassObjectGeometry()
{
    if (m_glassObjectIndex < 0)
        return;
    const QSizeF glassSize(qMax(1, width() - m_glassMarginsX * 2),
                           qMax(1, height() - m_glassMarginsY * 2));
    setGlassObjectGeometry(m_glassObjectIndex,
                           QPointF(m_glassMarginsX, m_glassMarginsY), glassSize);
}

void LiquidGlassWidget::updateWindowMask()
{
    QPainterPath shape;
    // The OpenGL glass object is laid out from (0,0) to the widget bounds.
    // Keep the native window region on that exact same silhouette; an extra
    // inset here produces a second, visibly smaller rounded rectangle.
    shape.addRoundedRect(QRectF(rect()), m_glassRadius, m_glassRadius);
    setMask(QRegion(shape.toFillPolygon().toPolygon()));
}

void LiquidGlassWidget::moveToDesktopCorner(int margin)
{
    const QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;
    const QRect area = screen->availableGeometry();
    move(area.right() - width() - margin + 1, area.bottom() - height() - margin + 1);
}

bool LiquidGlassWidget::rebuildWallpaperCanvas(QScreen* screen)
{
#ifdef Q_OS_WIN
    QSettings desktop(QStringLiteral("HKEY_CURRENT_USER\\Control Panel\\Desktop"),
                      QSettings::NativeFormat);
    const QString path = desktop.value(QStringLiteral("WallPaper")).toString();
    const QString style = desktop.value(QStringLiteral("WallpaperStyle"),
                                        QStringLiteral("10")).toString();
    const bool tiled = desktop.value(QStringLiteral("TileWallpaper"),
                                     QStringLiteral("0")).toString() == QStringLiteral("1");
    const QString background = desktop.value(QStringLiteral("Background"),
                                             QStringLiteral("0 0 0")).toString();
#else
    const QString path;
    const QString style = QStringLiteral("10");
    const bool tiled = false;
    const QString background = QStringLiteral("0 0 0");
#endif

    const qreal dpr = screen->devicePixelRatio();
    const QSize nativeSize(qMax(1, qRound(screen->size().width() * dpr)),
                           qMax(1, qRound(screen->size().height() * dpr)));
    // A full 4K/200% desktop can exceed 130 MB in RGBA form.  A 2560-pixel
    // long edge is visually lossless for a small glass card and keeps one
    // shared process-wide canvas inexpensive; the crop below scales it back
    // to the widget's native framebuffer size.
    constexpr int kBackdropMaxDimension = 2560;
    const qreal canvasScale = qMin<qreal>(1.0,
        kBackdropMaxDimension / qreal(qMax(nativeSize.width(), nativeSize.height())));
    const QSize pixelSize(qMax(1, qRound(nativeSize.width() * canvasScale)),
                          qMax(1, qRound(nativeSize.height() * canvasScale)));
    const qint64 modified = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    const QString signature = path + QChar('|') + style + QChar('|')
                              + QString::number(tiled) + QChar('|') + background
                              + QChar('|') + QString::number(modified);
    if (!m_wallpaperCanvas.isNull() && signature == m_wallpaperSignature
        && screen->name() == m_wallpaperScreenName
        && pixelSize == m_wallpaperPixelSize) {
        return false;
    }

    if (!s_sharedWallpaperCanvas.isNull()
        && signature == s_sharedWallpaperSignature
        && screen->name() == s_sharedWallpaperScreenName
        && pixelSize == s_sharedWallpaperPixelSize) {
        m_wallpaperCanvas = s_sharedWallpaperCanvas;
        m_wallpaperSignature = s_sharedWallpaperSignature;
        m_wallpaperScreenName = s_sharedWallpaperScreenName;
        m_wallpaperPixelSize = s_sharedWallpaperPixelSize;
        m_lastBackdropGeometry = QRect();
        return true;
    }

    QColor backgroundColor(Qt::black);
    const QStringList components = background.split(QChar(' '), Qt::SkipEmptyParts);
    if (components.size() >= 3)
        backgroundColor = QColor(components[0].toInt(), components[1].toInt(),
                                 components[2].toInt());

    QImage canvas(pixelSize, QImage::Format_RGBA8888);
    canvas.fill(backgroundColor);
    const QImage wallpaper(path);
    if (!wallpaper.isNull()) {
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QRectF bounds(QPointF(0, 0), QSizeF(pixelSize));

        if (tiled) {
            painter.drawTiledPixmap(bounds.toRect(), QPixmap::fromImage(wallpaper));
        } else if (style == QStringLiteral("2") || style == QStringLiteral("22")) {
            // Stretch, and Span on the active screen.  Span uses one image for
            // the virtual desktop; on a single monitor this is pixel-identical.
            painter.drawImage(bounds, wallpaper);
        } else if (style == QStringLiteral("0")) {
            const QPointF topLeft((pixelSize.width() - wallpaper.width()) * .5,
                                  (pixelSize.height() - wallpaper.height()) * .5);
            painter.drawImage(topLeft, wallpaper);
        } else {
            const bool fill = style == QStringLiteral("10");
            const qreal sx = pixelSize.width() / qreal(wallpaper.width());
            const qreal sy = pixelSize.height() / qreal(wallpaper.height());
            const qreal scale = fill ? qMax(sx, sy) : qMin(sx, sy);
            const QSizeF scaled(wallpaper.width() * scale, wallpaper.height() * scale);
            const QRectF target((pixelSize.width() - scaled.width()) * .5,
                                (pixelSize.height() - scaled.height()) * .5,
                                scaled.width(), scaled.height());
            painter.drawImage(target, wallpaper);
        }
    }

    s_sharedWallpaperCanvas = canvas;
    s_sharedWallpaperSignature = signature;
    s_sharedWallpaperScreenName = screen->name();
    s_sharedWallpaperPixelSize = pixelSize;
    m_wallpaperCanvas = s_sharedWallpaperCanvas;
    m_wallpaperSignature = signature;
    m_wallpaperScreenName = screen->name();
    m_wallpaperPixelSize = pixelSize;
    m_lastBackdropGeometry = QRect();
    return true;
}

void LiquidGlassWidget::captureDesktopBackdrop()
{
    if (!m_desktopCaptureEnabled || !isVisible())
        return;

    QScreen* screen = QGuiApplication::screenAt(geometry().center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;

    const bool screenChanged = screen->name() != m_wallpaperScreenName;
    bool canvasChanged = false;
    if (screenChanged || m_wallpaperCanvas.isNull() || m_wallpaperPollTick == 0)
        canvasChanged = rebuildWallpaperCanvas(screen);
    // Keep wallpaper-change detection at roughly one second regardless of
    // whether the backdrop timer is in idle (1 Hz) or drag (60 Hz) mode.
    const int pollEvery = m_dragging ? 60 : 1;
    m_wallpaperPollTick = (m_wallpaperPollTick + 1) % pollEvery;

    if (!canvasChanged && geometry() == m_lastBackdropGeometry)
        return;

    const qreal dpr = screen->devicePixelRatio();
    const QPoint local = geometry().topLeft() - screen->geometry().topLeft();
    const QSize nativeScreen(qMax(1, qRound(screen->size().width() * dpr)),
                             qMax(1, qRound(screen->size().height() * dpr)));
    const qreal sx = m_wallpaperCanvas.width() / qreal(nativeScreen.width());
    const qreal sy = m_wallpaperCanvas.height() / qreal(nativeScreen.height());
    const QRect pixelRect(qRound(local.x() * dpr * sx), qRound(local.y() * dpr * sy),
                          qMax(1, qRound(width() * dpr * sx)),
                          qMax(1, qRound(height() * dpr * sy)));
    // The glass surface is ultimately filtered and composited by the native
    // widget framebuffer. Capturing the backdrop at the same reduced scale
    // used by QtGlassFlow avoids an unnecessarily large screenshot/upload,
    // while bilinear sampling keeps the final card dimensions unchanged.
    const qreal captureScale = renderScale();
    QImage sample(QSize(qMax(1, qRound(width() * dpr * captureScale)),
                        qMax(1, qRound(height() * dpr * captureScale))),
                  QImage::Format_RGBA8888);
    {
        QPainter painter(&sample);
        painter.drawImage(QRectF(QPointF(0, 0), QSizeF(sample.size())),
                          m_wallpaperCanvas, pixelRect);
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    sample = sample.flipped(Qt::Vertical);
#else
    sample = sample.mirrored(false, true);
#endif
    setBackgroundImageFlipped(sample);
    m_lastBackdropGeometry = geometry();
}

void LiquidGlassWidget::resizeEvent(QResizeEvent* event)
{
    QtGlassFlowScene::resizeEvent(event);
    updateGlassObjectGeometry();
    updateWindowMask();
}

void LiquidGlassWidget::showEvent(QShowEvent* event)
{
    QtGlassFlowScene::showEvent(event);
    if (m_backdropTimer && m_desktopCaptureEnabled) {
        updateRefreshRate();
        m_backdropTimer->start();
    }
#ifdef Q_OS_WIN
    const HWND window = reinterpret_cast<HWND>(winId());
    if (window && m_desktopLayerEnabled) {
        SetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
                         | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    }
    applySystemBlurEffect();
#endif
    QTimer::singleShot(0, this, &LiquidGlassWidget::captureDesktopBackdrop);
}

void LiquidGlassWidget::hideEvent(QHideEvent* event)
{
    // Hidden cards do not contribute pixels. Stop the desktop sampler as well
    // as the scene timer in the shared base, so tray hide/show does not leave
    // one periodic callback per widget running in the background.
    if (m_backdropTimer)
        m_backdropTimer->stop();
    QtGlassFlowScene::hideEvent(event);
}

void LiquidGlassWidget::mousePressEvent(QMouseEvent* event)
{
    if (m_mouseThrough) {
        event->ignore();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
        setInteractionActive(true);
        updateRefreshRate();
        event->accept();
        return;
    }
    QtGlassFlowScene::mousePressEvent(event);
}

void LiquidGlassWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_mouseThrough) {
        event->ignore();
        return;
    }
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - m_dragOffset);
        event->accept();
        return;
    }
    QtGlassFlowScene::mouseMoveEvent(event);
}

void LiquidGlassWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_mouseThrough) {
        event->ignore();
        return;
    }
    const bool finishedDrag = m_dragging && event->button() == Qt::LeftButton;
    if (event->button() == Qt::LeftButton)
        m_dragging = false;
    if (event->button() == Qt::LeftButton) {
        setInteractionActive(false);
        updateRefreshRate();
    }
    QtGlassFlowScene::mouseReleaseEvent(event);
    if (finishedDrag)
        captureDesktopBackdrop();
}
