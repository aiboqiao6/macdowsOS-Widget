#include "rendering/liquidglasswidget.h"
#include "app/appsettings.h"

#include "desktopcapture.h"
#include "nativewindows.h"

#include <QApplication>
#include <QMoveEvent>
#include <QGuiApplication>
#include <QEvent>
#include <QOperatingSystemVersion>
#include <QHideEvent>
#include <QFileInfo>
#include <QList>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QRegion>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QSettings>
#include <QStringList>
#include <QTimer>
#include <QtMath>
#include <utility>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>

#endif

namespace {
constexpr int kWallpaperIdleIntervalMs = 10000; // 0.1 FPS when live sampling is off

// QImage is implicitly shared, so keeping one process-wide wallpaper canvas
// avoids a full-screen RGBA allocation per widget (which is especially costly
// on 4K/high-DPI desktops). Each instance still owns its small local crop.
QImage s_sharedWallpaperCanvas;
QString s_sharedWallpaperSignature;
QString s_sharedWallpaperScreenName;
QSize s_sharedWallpaperPixelSize;


QList<LiquidGlassWidget*> s_liveGlassWidgets;
QPointer<QTimer> s_compositeRefreshTimer;
quint64 s_surfaceRevision = 0;

} // namespace

LiquidGlassWidget::LiquidGlassWidget(QWidget* parent)
    : QtGlassFlowScene(parent)
{
    s_liveGlassWidgets.append(this);
    connect(this, &QtGlassFlowScene::frameRendered, this, &LiquidGlassWidget::queueCompositeRefresh);
    setWindowTitle(QStringLiteral("macdowsOS Widget"));
    // Keep the card on the desktop layer. It remains draggable, but regular
    // application windows stay above it just like a macOS desktop widget.
    // On Windows the native Z order is switched dynamically in
    // updateWindowLayer(). Do not also set WindowStaysOnBottomHint there:
    // Qt can reapply that static hint while processing a move and demote a
    // card immediately after we promoted it for dragging.
    Qt::WindowFlags flags = Qt::Tool | Qt::FramelessWindowHint
                            | Qt::WindowDoesNotAcceptFocus;
#ifndef Q_OS_WIN
    flags |= Qt::WindowStaysOnBottomHint;
#endif
    setWindowFlags(flags);
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

    // The GUI timer only schedules bounded worker requests and local crops.
    // Unchanged desktop pixels skip texture uploads and blur passes.
    m_backdropTimer = new QTimer(this);
    m_backdropTimer->setTimerType(Qt::CoarseTimer);
    m_backdropTimer->setInterval(kWallpaperIdleIntervalMs);
    connect(m_backdropTimer, &QTimer::timeout,
            this, &LiquidGlassWidget::captureDesktopBackdrop);
    // Polling detects screenshot tools so the backdrop sampler can pause while
    // a selection surface is changing the desktop underneath the card.
    m_captureMonitorTimer = new QTimer(this);
    m_captureMonitorTimer->setTimerType(Qt::CoarseTimer);
    m_captureMonitorTimer->setInterval(200);
    connect(m_captureMonitorTimer, &QTimer::timeout,
            this, &LiquidGlassWidget::updateSystemCaptureMode);
    NativeWindows::observeTopology(this, [this]() {
        // Invalidate/recompose promptly when another application changes
        // layers, even while the static-desktop timer is running at 10 Hz.
        if (isVisible()) {
            updateWindowLayer();
            if (m_desktopCaptureEnabled)
                captureDesktopBackdrop();
        }
    });
}

LiquidGlassWidget::~LiquidGlassWidget()
{
    NativeWindows::unregisterGlassWindow(m_registeredGlassWindow);
    s_liveGlassWidgets.removeAll(this);
    if (m_dragging)
        refreshDragRendering();
    if (s_liveGlassWidgets.isEmpty())
        DesktopCapture::resetWorkerStreams();
    queueCompositeRefresh();
}

void LiquidGlassWidget::queueCompositeRefresh()
{
    ++s_surfaceRevision;
    if (!s_compositeRefreshTimer) {
        s_compositeRefreshTimer = new QTimer(qApp);
        s_compositeRefreshTimer->setSingleShot(true);
        QObject::connect(s_compositeRefreshTimer, &QTimer::timeout, qApp, []() {
            for (LiquidGlassWidget* widget : std::as_const(s_liveGlassWidgets)) {
                if (widget->isVisible() && widget->m_desktopCaptureEnabled)
                    widget->updateCompositeCrop();
            }
        });
    }
    if (!s_compositeRefreshTimer->isActive())
        s_compositeRefreshTimer->start(0);
}

QImage LiquidGlassWidget::captureDesktopComposite(QScreen* screen, const QRect& area,
                                                  LiquidGlassWidget* excluded, qreal renderScale)
{
    if (!screen || area.isEmpty())
        return {};
    // The card remains capturable normally. It is excluded only for the few
    // milliseconds of this internal sample so its actual backdrop is read.
    const bool targetVisible = excluded && excluded->isVisible();
    const qreal scale = screen->devicePixelRatio() * renderScale;
    const QRect captureArea = targetVisible
        ? area.adjusted(-48, -48, 48, 48) : area;
    const WId target = excluded && excluded->isVisible() ? excluded->winId() : 0;
    auto frame = DesktopCapture::grab(captureArea, scale, target);
    // A foreground animation can change the native stack during the first
    // synchronous sample. Retry once instead of exposing a null/partial frame
    // to gallery hosts or explicit snapshot callers.
    if (frame.image.isNull())
        frame = DesktopCapture::grab(captureArea, scale, target);
    DesktopCapture::Cache temporary;
    QImage image = (excluded ? excluded->m_backdropCache : temporary).merge(std::move(frame), [=]() {
        return excluded ? excluded->wallpaperBackdrop(captureArea, scale) : QImage();
    });
    compositeGlassWindows(image, captureArea, excluded);
    if (!targetVisible || captureArea == area)
        return image;
    const qreal sx = image.width() / qreal(captureArea.width());
    const qreal sy = image.height() / qreal(captureArea.height());
    const QRect crop(qRound((area.left() - captureArea.left()) * sx),
                     qRound((area.top() - captureArea.top()) * sy),
                     qMax(1, qRound(area.width() * sx)),
                     qMax(1, qRound(area.height() * sy)));
    return image.copy(crop.intersected(image.rect()));
}

void LiquidGlassWidget::requestDesktopComposite(const QRect& area,
                                                 std::function<void(QImage)> completed)
{
    QScreen* screen = QGuiApplication::screenAt(area.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen || !m_captureExcluded || area.isEmpty()) {
        completed({});
        return;
    }
    const qreal scale = screen->devicePixelRatio() * renderScale();
    // Callers use this path for a settled interactive surface. Capture only
    // the requested rectangle: unlike the desktop-card drag path, it does not
    // need a movement runway, and the smaller transfer keeps live gallery
    // updates lighter.
    const QRect captureArea = area.intersected(screen->geometry());
    DesktopCapture::request(captureArea, scale, winId(), this,
        [this, area, captureArea, scale,
         completed = std::move(completed)](DesktopCapture::Frame frame) {
            QImage image = m_backdropCache.merge(
                std::move(frame),
                [=]() { return wallpaperBackdrop(captureArea, scale); });
            compositeGlassWindows(image, captureArea, this);
            if (!image.isNull() && captureArea != area) {
                const qreal sx = image.width() / qreal(captureArea.width());
                const qreal sy = image.height() / qreal(captureArea.height());
                const QRect crop(
                    qRound((area.left() - captureArea.left()) * sx),
                    qRound((area.top() - captureArea.top()) * sy),
                    qMax(1, qRound(area.width() * sx)),
                    qMax(1, qRound(area.height() * sy)));
                image = image.copy(crop.intersected(image.rect()));
            }
            completed(std::move(image));
        });
}

void LiquidGlassWidget::compositeDesktopGlass(QImage& image, const QRect& area, WId target)
{
    compositeGlassWindows(image, area, nullptr, target);
}

void LiquidGlassWidget::compositeGlassWindows(QImage& image, const QRect& area,
                                              const LiquidGlassWidget* excluded, WId target)
{
#ifdef Q_OS_WIN
    if (image.isNull())
        return;
    QList<LiquidGlassWidget*> candidates;
    for (LiquidGlassWidget* widget : std::as_const(s_liveGlassWidgets)) {
        if (widget != excluded && widget->isVisible() && widget->m_captureExcluded
            && area.intersects(widget->frameGeometry()))
            candidates.append(widget);
    }
    if (candidates.isEmpty())
        return;
    const auto& stack = NativeWindows::cachedStack();
    QHash<WId, LiquidGlassWidget*> glassWindows;
    for (LiquidGlassWidget* widget : std::as_const(s_liveGlassWidgets)) {
        if (widget->isVisible())
            glassWindows.insert(widget->winId(), widget);
    }
    if (excluded && excluded->isVisible())
        target = excluded->winId();
    int targetIndex = -1;
    if (target) {
        for (int i = 0; i < stack.size(); ++i)
            if (stack.at(i).id == target) { targetIndex = i; break; }
        // A native recreation can remove the target for one event-loop turn.
        // Do not insert windows of unknown relative order during that turn.
        if (targetIndex < 0)
            return;
    }
    QPainter painter(&image);
    painter.scale(image.width() / qreal(area.width()), image.height() / qreal(area.height()));
    painter.translate(-area.topLeft());
    // Paint lower layers bottom to top, using the same stack and the same
    // completed frame for all consumers. Cloaked/shell windows are never
    // opaque covers, and known glass windows are not subtracted as rectangles.
    for (int i = stack.size() - 1; i > targetIndex; --i) {
        LiquidGlassWidget* widget = glassWindows.value(stack.at(i).id);
        if (!widget || !candidates.contains(widget))
            continue;
        QRegion visible(widget->frameGeometry().intersected(area));
        const qreal dpr = widget->devicePixelRatioF();
        const QPoint origin = stack.at(i).bounds.topLeft();
        for (int j = targetIndex + 1; j < i && !visible.isEmpty(); ++j) {
            const NativeWindows::Window& above = stack.at(j);
            if (!above.opaque || glassWindows.contains(above.id))
                continue;
            visible -= QRect(widget->x() + qRound((above.bounds.x() - origin.x()) / dpr),
                             widget->y() + qRound((above.bounds.y() - origin.y()) / dpr),
                             qRound(above.bounds.width() / dpr), qRound(above.bounds.height() / dpr));
        }
        if (visible.isEmpty())
            continue;
        const QImage snapshot = widget->surfaceSnapshot();
        if (snapshot.isNull())
            continue;
        painter.save();
        painter.setClipRegion(visible);
        // The capture cache deliberately rejects the complete rectangle of a
        // lower glass window. Rebuild that hole from the lower surface's own
        // pristine shader input before drawing its translucent output. Using
        // wallpaper as the hole fill made rounded corners sample a different
        // colour, and repeated overlap/refraction appeared as growing glare.
        if (!widget->m_lastCompositedBackdrop.isNull())
            painter.drawImage(widget->frameGeometry(),
                              widget->m_lastCompositedBackdrop);
        painter.drawImage(widget->frameGeometry(), snapshot);
        painter.restore();
    }
#else
    Q_UNUSED(image);
    Q_UNUSED(area);
    Q_UNUSED(excluded);
    Q_UNUSED(target);
#endif
}

void LiquidGlassWidget::setMaterialBlurStrength(int percent)
{
    m_blurStrength = qBound(0, percent, 100);
    AppSettings settings;
    settings.setValue(QStringLiteral("appearance/blurStrength"), m_blurStrength);
    updateMaterialParameters();
    update();
}

void LiquidGlassWidget::updateMaterialParameters()
{
    const qreal amount = m_blurStrength / 100.0;
    setBlurRadius(float(0.5 + amount * 9.5));
    setBlurIterations(1 + qRound(amount * 2.0));
    setNoiseAmount(0.008f);
    setRefractionPower(1.32f);
}

void LiquidGlassWidget::setGlassOpacity(qreal opacity)
{
    m_glassOpacity = qBound<qreal>(0.05, opacity, 1.0);
    AppSettings settings;
    settings.setValue(QStringLiteral("appearance/opacity"), m_glassOpacity);
    QtGlassFlowScene::setGlassOpacity(float(m_glassOpacity));
    update();
}

void LiquidGlassWidget::setMouseThroughEnabled(bool enabled)
{
    if (m_mouseThrough == enabled)
        return;
    if (enabled)
        finishWindowDrag();
    m_mouseThrough = enabled;
    AppSettings settings;
    settings.setValue(QStringLiteral("interaction/mouseThrough"), enabled);
    setAttribute(Qt::WA_TransparentForMouseEvents, enabled);
    setMouseTracking(!enabled);

    Qt::WindowFlags flags = windowFlags();
    flags.setFlag(Qt::WindowTransparentForInput, enabled);
    const bool wasVisible = isVisible();
    const QRect oldGeometry = geometry();
    setWindowFlags(flags);
    m_windowLayerInitialized = false;
    if (wasVisible) {
        setGeometry(oldGeometry);
        show();
    }
    updateRefreshRate();
}

void LiquidGlassWidget::setLowPowerRefreshEnabled(bool enabled)
{
    m_lowPowerRefresh = enabled;
    AppSettings settings;
    settings.setValue(QStringLiteral("interaction/lowPowerRefresh"), enabled);
    updateRefreshRate();
}

void LiquidGlassWidget::setLiveBackdropEnabled(bool enabled, bool persist)
{
    if (m_liveBackdropEnabled == enabled)
        return;
    m_liveBackdropEnabled = enabled;
    if (persist) {
        AppSettings settings;
        settings.setValue(QStringLiteral("appearance/liveBackdrop"), enabled);
    }
    ++m_captureGeneration;
    m_backdropCache.clear();
    m_compositeCanvas = {};
    m_lastCompositedBackdrop = {};
    m_compositeCaptureClock.invalidate();
    m_unchangedCaptures = 0;
    updateRefreshRate();
    if (isVisible() && m_desktopCaptureEnabled) {
        // setInterval() does not make a stopped timer live.  Be explicit here
        // because the sampler may have been stopped while hidden or during a
        // system capture when this preference is changed.
        if (m_backdropTimer && !m_systemCaptureActive)
            m_backdropTimer->start();
        captureDesktopBackdrop();
    }
}

void LiquidGlassWidget::setRenderScale(float scale)
{
    const float before = renderScale();
    QtGlassFlowScene::setRenderScale(scale);
    if (qFuzzyCompare(before, renderScale()))
        return;

    ++m_captureGeneration;
    m_backdropCache.clear();
    m_compositeCanvas = {};
    m_lastCompositedBackdrop = {};
    m_compositeCaptureClock.invalidate();
    m_wallpaperCanvas = {};
    m_wallpaperPixelSize = {};
    m_wallpaperRefreshClock.invalidate();
    if (isVisible() && m_desktopCaptureEnabled)
        QTimer::singleShot(0, this, &LiquidGlassWidget::captureDesktopBackdrop);
}

void LiquidGlassWidget::setDesktopCaptureEnabled(bool enabled)
{
    if (m_desktopCaptureEnabled == enabled)
        return;
    m_desktopCaptureEnabled = enabled;
    updateCaptureExclusion();
    if (m_backdropTimer) {
        if (enabled && isVisible())
            m_backdropTimer->start();
        else
            m_backdropTimer->stop();
    }
}

void LiquidGlassWidget::updateRefreshRate()
{
    // Reduced cadence is safe only for click-through cards: interactive cards
    // must remain responsive even if the preference was left enabled earlier.
    const bool lowPower = m_lowPowerRefresh && m_mouseThrough;
    const int displayInterval = activeDisplayInterval();
    setRefreshInterval(lowPower ? 250 : displayInterval);
    // Live mode must present at the same cadence even without pointer input
    // or changing background pixels. Keep payload animation independent.
    const bool desktopDrag = desktopDragActive();
    setContinuousRenderingEnabled(m_liveBackdropEnabled && !desktopDrag);
    if (m_backdropTimer) {
        const bool dragging = m_dragging && !lowPower;
        const bool live = !wallpaperOnlyWhenIdle() && !desktopDrag;
        // Live sampling is presentation work, not a housekeeping task.  A
        // coarse timer may be delayed by tens of milliseconds while the card
        // is stationary, which made the enabled setting look frozen.  The
        // bounded in-flight requests below already provide back-pressure.
        m_backdropTimer->setTimerType((dragging || live) ? Qt::PreciseTimer
                                                        : Qt::VeryCoarseTimer);
        const int interval = dragging ? displayInterval
                           : (lowPower ? 250
                           : (live ? displayInterval
                                   : kWallpaperIdleIntervalMs));
        if (m_backdropTimer->interval() != interval)
            m_backdropTimer->setInterval(interval);
    }
}

bool LiquidGlassWidget::desktopDragActive()
{
    for (const auto* widget : std::as_const(s_liveGlassWidgets))
        if (widget->m_dragging && widget->directBackdropDuringDrag())
            return true;
    return false;
}

void LiquidGlassWidget::refreshDragRendering()
{
    if (desktopDragActive())
        DesktopCapture::resetWorkerStreams();
    for (auto* widget : std::as_const(s_liveGlassWidgets)) {
        ++widget->m_captureGeneration;
        widget->m_compositeCaptureClock.invalidate();
        widget->updateRefreshRate();
        QTimer::singleShot(0, widget, &LiquidGlassWidget::captureDesktopBackdrop);
    }
}

bool LiquidGlassWidget::hasOverlappingDesktopWidget() const
{
    if (!m_desktopLayerEnabled || !isVisible())
        return false;
    const QRect bounds = frameGeometry();
    for (LiquidGlassWidget* widget : std::as_const(s_liveGlassWidgets)) {
        if (!widget || widget == this || !widget->m_desktopLayerEnabled
            || !widget->isVisible())
            continue;
        if (bounds.intersects(widget->frameGeometry()))
            return true;
    }
    return false;
}

void LiquidGlassWidget::refreshBackdropTopology()
{
    for (LiquidGlassWidget* widget : std::as_const(s_liveGlassWidgets)) {
        if (!widget || !widget->m_desktopLayerEnabled || !widget->isVisible())
            continue;
        widget->updateRefreshRate();
        if (widget->hasOverlappingDesktopWidget())
            widget->captureDesktopBackdrop();
    }
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
#ifdef Q_OS_WIN
    // A native HRGN is binary and clips the alpha-smoothed OpenGL edge back
    // to whole logical pixels.  On fractional DPI scales that produces the
    // staircase visible around rounded cards.  Windows' translucent
    // compositor already honours the framebuffer alpha, so keep the native
    // window rectangular and let the shader own the antialiased silhouette.
    clearMask();
#else
    QPainterPath shape;
    // The OpenGL glass object is laid out from (0,0) to the widget bounds.
    // Keep the native window region on that exact same silhouette; an extra
    // inset here produces a second, visibly smaller rounded rectangle.
    shape.addRoundedRect(QRectF(rect()), m_glassRadius, m_glassRadius);
    setMask(QRegion(shape.toFillPolygon().toPolygon()));
#endif
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
    // Let supersampled quality modes draw from a correspondingly larger
    // wallpaper source. This keeps 125%/150% rendering from merely enlarging
    // an already reduced image, while the native desktop size remains the cap.
    const int kBackdropMaxDimension = qRound(2560 * qMax<qreal>(1.0, renderScale()));
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
        return true;
    }

    QColor backgroundColor(Qt::black);
    const QStringList components = background.split(QChar(' '), Qt::SkipEmptyParts);
    if (components.size() >= 3)
        backgroundColor = QColor(components[0].toInt(), components[1].toInt(),
                                 components[2].toInt());

    QImage canvas(pixelSize, QImage::Format_RGBA8888);
    canvas.fill(backgroundColor);
    const QImage wallpaper = path.isEmpty() ? QImage() : QImage(path);
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
    return true;
}

void LiquidGlassWidget::updateCaptureExclusion(bool resetBackdrop)
{
    ++m_captureGeneration;
    if (resetBackdrop) {
        m_backdropCache.clear();
        m_compositeCanvas = {};
        m_compositeCaptureClock.invalidate();
    }
#ifdef Q_OS_WIN
    if (!isVisible())
        return;
    if (QOperatingSystemVersion::current()
        < QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 19041))
        return;
    const WId id = winId();
    // Qt's translucent HWND still receives a Windows 11 non-client outline
    // on some themes. That rectangular border is outside our GL alpha mask.
    // The glass shader and overlay own the entire rounded edge.
    using SetDwmAttribute = HRESULT (WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    static const auto setDwmAttribute = reinterpret_cast<SetDwmAttribute>(
        GetProcAddress(GetModuleHandleW(L"dwmapi.dll"), "DwmSetWindowAttribute"));
    if (setDwmAttribute) {
        const int disabled = 1; // DWMNCRP_DISABLED
        const DWORD noBorder = 0xfffffffe; // DWMWA_COLOR_NONE
        setDwmAttribute(reinterpret_cast<HWND>(id), 2, &disabled, sizeof(disabled));
        setDwmAttribute(reinterpret_cast<HWND>(id), 34, &noBorder, sizeof(noBorder));
    }
    if (m_registeredGlassWindow && m_registeredGlassWindow != id)
        NativeWindows::unregisterGlassWindow(m_registeredGlassWindow);
    m_registeredGlassWindow = id;
    NativeWindows::registerGlassWindow(id);
    // Keep the card visible to every system screenshot and recording surface.
    // NativeWindows' process-local glass registry still keeps it out of our
    // own backdrop sampler.
    // m_captureExcluded is an internal registration flag used only by the
    // compositor and sampler. It does not alter native screenshot policy.
    m_captureExcluded = true;
#endif
}

void LiquidGlassWidget::updateSystemCaptureMode()
{
#ifdef Q_OS_WIN
    if (!isVisible())
        return;
    // Reuse the existing monitor tick as a fallback for native callers that
    // suppress WINDOWPOSCHANGING or do not emit a usable reorder event. This
    // check is independent of live capture and only repositions misplaced cards.
    updateWindowLayer();
    setSystemCaptureMode(NativeWindows::isSystemCaptureActive());
#endif
}

void LiquidGlassWidget::setSystemCaptureMode(bool active)
{
    if (m_systemCaptureActive == active)
        return;
    m_systemCaptureActive = active;
    ++m_captureGeneration;
    m_compositeCaptureClock.invalidate();

    // Pause sampling while the external screenshot surface is active; this
    // avoids ingesting an intermediate frame into the backdrop cache.
    updateCaptureExclusion(false);
    if (m_systemCaptureActive) {
        if (m_backdropTimer)
            m_backdropTimer->stop();
        return;
    }
    if (m_backdropTimer && m_desktopCaptureEnabled && isVisible()) {
        updateRefreshRate();
        m_backdropTimer->start();
        QTimer::singleShot(0, this, &LiquidGlassWidget::captureDesktopBackdrop);
    }
}

int LiquidGlassWidget::activeDisplayInterval() const
{
    QScreen* screen = QGuiApplication::screenAt(frameGeometry().center());
    if (!screen)
        screen = this->screen();
    qreal refreshRate = screen ? screen->refreshRate() : 60.0;
    if (!qIsFinite(refreshRate) || refreshRate < 24.0)
        refreshRate = 60.0;
    // Use floor rather than round so the timer never undershoots the display
    // rate (59.94 -> 16 ms, 144 -> 6 ms, 240 -> 4 ms). Actual presentation is
    // synchronized by Qt and the Windows desktop compositor.
    return qBound(2, qFloor(1000.0 / qMin<qreal>(refreshRate, 500.0)), 42);
}

bool LiquidGlassWidget::captureCompositedBackdrop(QScreen* screen)
{
    if (!screen || !m_captureExcluded)
        return false;
    updateCompositeCrop();
    const bool live = m_liveBackdropEnabled && !(m_lowPowerRefresh && m_mouseThrough);
    if (m_capturesInFlight >= (live && !m_dragging ? 2 : 1))
        return true;
    // Coarse timers may fire up to 5% early; allow that tolerance instead of
    // unintentionally skipping every other idle tick.
    const int maxAge = live && !m_dragging ? 0
                     : (m_dragging ? 14 : qMax(1, m_backdropTimer->interval() * 94 / 100));
    if (m_compositeCaptureClock.isValid() && m_compositeCaptureClock.elapsed() < maxAge
        && m_lastCompositeGeometry == geometry())
        return true;
    if (m_dragging && m_compositeCaptureClock.isValid()
        && m_compositeCaptureClock.elapsed() < maxAge)
        return true;
    // A padded capture lets movement crop immediately from RAM while the
    // next desktop frame arrives from the worker. Live surfaces use a bounded
    // two-request pipeline so GPU presentation cannot leave the worker idle.
    // Keep a small movement runway so crops stay responsive while dragging,
    // but avoid the 3x pixel area of the previous 128 px pad.
    // A small idle runway keeps motion crops responsive without expanding the
    // capture to a large part of a high-DPI desktop.
    const int padding = live && !m_dragging ? 0 : (m_dragging ? 96 : 56);
    const QRect targetGeometry = frameGeometry();
    const QRect area = targetGeometry.adjusted(-padding, -padding, padding, padding);
    const quint64 generation = m_captureGeneration;
    ++m_capturesInFlight;
    m_compositeCaptureClock.restart();
    // Store only the external desktop. Glass layers are composed at the
    // current position from completed frames, independently of capture age.
    const qreal scale = screen->devicePixelRatio() * renderScale();
    DesktopCapture::request(area, scale, winId(), this,
        [this, area, scale, generation](DesktopCapture::Frame frame) {
        --m_capturesInFlight;
        if (generation != m_captureGeneration || !isVisible() || !m_desktopCaptureEnabled
            || m_systemCaptureActive || fullyOccluded())
            return;
        QImage image = m_backdropCache.merge(std::move(frame), [=]() { return wallpaperBackdrop(area, scale); });
        if (!image.isNull()) {
            if (m_compositeArea != area || m_compositeCanvas != image) {
                m_compositeCanvas = std::move(image);
                m_compositeArea = area;
                m_unchangedCaptures = 0;
            } else
                m_unchangedCaptures = qMin(8, m_unchangedCaptures + 1);
            updateRefreshRate();
            m_lastCompositeGeometry = geometry();
            ++s_surfaceRevision;
            updateCompositeCrop();
        }
        // Start the next asynchronous request before returning to Qt's
        // vsynced composition. A queued zero timer here lost a display tick.
        if (m_liveBackdropEnabled && !(m_lowPowerRefresh && m_mouseThrough)
            && !desktopDragActive())
            captureDesktopBackdrop();
    }, m_dragging ? 1 : 0);
    return true;
}

bool LiquidGlassWidget::fullyOccluded() const
{
#ifdef Q_OS_WIN
    if (!m_desktopLayerEnabled || m_dragging || !isVisible())
        return false;
    const WId current = winId();
    const auto& stack = NativeWindows::cachedStack();
    int index = -1;
    for (int i = 0; i < stack.size(); ++i)
        if (stack.at(i).id == current) { index = i; break; }
    if (index < 0)
        return false;
    QRegion visible(stack.at(index).bounds);
    for (int i = 0; i < index; ++i) {
        const NativeWindows::Window& above = stack.at(i);
        if (above.opaque) {
            visible -= above.bounds.adjusted(12, 12, -12, -12);
            if (visible.isEmpty())
                return true;
        }
    }
#endif
    return false;
}

void LiquidGlassWidget::updateCompositeCrop()
{
    // A desktop drag owns a fresh wallpaper/lower-card composition. Neither a
    // queued capture nor another card's frame signal may restore an old crop.
    if (directBackdropDuringDrag() && desktopDragActive())
        return;
    if (fullyOccluded())
        return;
    if (wallpaperOnlyWhenIdle() && !m_dragging)
        return;
    if (m_compositeCanvas.isNull() || !m_compositeArea.contains(frameGeometry()))
        return;
    const bool sourceUnchanged = m_lastCropCanvasKey == m_compositeCanvas.cacheKey()
                                 && m_lastCropGeometry == frameGeometry();
    bool overlapping = false;
    for (LiquidGlassWidget* widget : std::as_const(s_liveGlassWidgets)) {
        if (widget != this && widget->isVisible() && frameGeometry().intersects(widget->frameGeometry())) {
            overlapping = true;
            break;
        }
    }
    if (sourceUnchanged && overlapping == m_lastCropHadLayers
        && (!overlapping || m_lastCropSceneRevision == s_surfaceRevision))
        return;
    // A no-op crop does not consume any captured pixels. Avoid rebuilding the
    // native capture plan on every render tick in that case; still validate
    // before every actual composition, including topology/overlap changes.
    if (!m_backdropCache.current())
        return;
    m_lastCropHadLayers = overlapping;
    m_lastCropSceneRevision = s_surfaceRevision;
    m_lastCropCanvasKey = m_compositeCanvas.cacheKey();
    m_lastCropGeometry = frameGeometry();
    const qreal sx = m_compositeCanvas.width() / qreal(m_compositeArea.width());
    const qreal sy = m_compositeCanvas.height() / qreal(m_compositeArea.height());
    const QPoint offset = frameGeometry().topLeft() - m_compositeArea.topLeft();
    QImage sample = m_compositeCanvas.copy(qRound(offset.x() * sx), qRound(offset.y() * sy),
                                          qMax(1, qRound(width() * sx)), qMax(1, qRound(height() * sy)));
    compositeGlassWindows(sample, frameGeometry(), this);
    // QImage's comparison uses optimized row comparisons and returns early.
    // A per-pixel 64-bit hash was slower and still required a full image scan.
    if (sample == m_lastCompositedBackdrop)
        return;
    m_lastCompositedBackdrop = sample;
    setBackgroundImage(sample);
}

void LiquidGlassWidget::captureDesktopBackdrop()
{
    if (!m_desktopCaptureEnabled || !isVisible() || m_systemCaptureActive)
        return;
    if (desktopDragActive() && !directBackdropDuringDrag())
        return;
    QScreen* screen = QGuiApplication::screenAt(geometry().center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;

    // During a desktop-card drag ordinary application windows are hidden by
    // the host. Build the exact current backdrop locally from the shared
    // wallpaper plus lower glass surfaces. This path is independent of the
    // asynchronous capture worker, whose result cannot be accepted while the
    // target HWND is changing position, and therefore remains smooth even
    // across movements larger than the capture runway.
    if (m_dragging && directBackdropDuringDrag()) {
        setRenderingSuspended(false);
        QImage sample = wallpaperBackdrop(frameGeometry(),
                                          screen->devicePixelRatio() * renderScale());
        compositeGlassWindows(sample, frameGeometry(), this);
        if (sample != m_lastCompositedBackdrop) {
            m_lastCompositedBackdrop = sample;
            setBackgroundImage(sample);
        }
        return;
    }

    // Desktop widgets stay visually quiet when not moving: their glass samples
    // the wallpaper only, avoiding periodic lower-window captures and texture
    // churn. Dragging switches to the live composited path below immediately.
    if ((wallpaperOnlyWhenIdle() || (directBackdropDuringDrag() && desktopDragActive()))
        && !m_dragging) {
        // A card may have been suspended while it was covered during a prior
        // live-capture phase. Idle wallpaper mode must always restore its own
        // paint loop and keep the native desktop layer in sync as well.
        updateWindowLayer();
        setRenderingSuspended(false);
        const QImage sample = wallpaperBackdrop(frameGeometry(),
                                                 screen->devicePixelRatio() * renderScale());
        if (sample != m_lastCompositedBackdrop) {
            m_lastCompositedBackdrop = sample;
            setBackgroundImage(sample);
        }
        return;
    }
    // Covered desktop cards cannot contribute visible pixels. Keep checking
    // visibility at the normal cadence, but avoid captures and GPU work.
    updateWindowLayer();
    const bool occluded = fullyOccluded();
    setRenderingSuspended(occluded);
    if (occluded)
        return;

    if (captureCompositedBackdrop(screen))
        return;

    QImage sample = wallpaperBackdrop(frameGeometry(), screen->devicePixelRatio() * renderScale());
    compositeGlassWindows(sample, frameGeometry(), this);
    if (sample != m_lastCompositedBackdrop) {
        m_lastCompositedBackdrop = sample;
        setBackgroundImage(sample);
    }
}

QImage LiquidGlassWidget::wallpaperBackdrop(const QRect& area, qreal scale)
{
    QImage result(QSize(qMax(1, qRound(area.width() * scale)), qMax(1, qRound(area.height() * scale))),
                   QImage::Format_RGB32);
    result.fill(Qt::black);
    QPainter painter(&result);
    // Supersampled quality requests a fractional enlargement of the native
    // wallpaper canvas. Nearest-neighbour copies create repeating pixel blocks
    // before the blur even begins, especially at 125%/150% quality.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (QScreen* screen : QGuiApplication::screens()) {
        const QRect part = area.intersected(screen->geometry());
        if (part.isEmpty())
            continue;
        if (screen->name() != m_wallpaperScreenName || m_wallpaperCanvas.isNull()
            || !m_wallpaperRefreshClock.isValid() || m_wallpaperRefreshClock.elapsed() >= 1000) {
            rebuildWallpaperCanvas(screen);
            m_wallpaperRefreshClock.restart();
        }
        const qreal sx = m_wallpaperCanvas.width() / qreal(screen->geometry().width());
        const qreal sy = m_wallpaperCanvas.height() / qreal(screen->geometry().height());
        painter.drawImage(QRectF((part.x() - area.x()) * scale, (part.y() - area.y()) * scale,
                                 part.width() * scale, part.height() * scale), m_wallpaperCanvas,
                          QRectF((part.x() - screen->geometry().x()) * sx,
                                 (part.y() - screen->geometry().y()) * sy,
                                 part.width() * sx, part.height() * sy));
    }
    return result;
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
    updateCaptureExclusion();
    // Showing a native window can reset its position in the Z band. Force one
    // layer update even when the cached state says it was already demoted.
    m_windowLayerInitialized = false;
    if (m_panelWindow)
        preparePanelOpening();
    updateWindowLayer();
    if (m_captureMonitorTimer)
        m_captureMonitorTimer->start();
    if (m_backdropTimer && m_desktopCaptureEnabled) {
        updateRefreshRate();
        m_backdropTimer->start();
    }
    QTimer::singleShot(0, this, &LiquidGlassWidget::captureDesktopBackdrop);
    QTimer::singleShot(0, this, &LiquidGlassWidget::refreshBackdropTopology);
}

void LiquidGlassWidget::hideEvent(QHideEvent* event)
{
    finishWindowDrag();
    // Hidden cards do not contribute pixels. Stop the desktop sampler as well
    // as the scene timer in the shared base, so tray hide/show does not leave
    // one periodic callback per widget running in the background.
    if (m_backdropTimer)
        m_backdropTimer->stop();
    if (m_captureMonitorTimer)
        m_captureMonitorTimer->stop();
    if (m_systemCaptureActive)
        setSystemCaptureMode(false);
    QtGlassFlowScene::hideEvent(event);
    QTimer::singleShot(0, this, &LiquidGlassWidget::refreshBackdropTopology);
}

void LiquidGlassWidget::updateWindowLayer()
{
#ifdef Q_OS_WIN
    if (!m_desktopLayerEnabled || !isVisible())
        return;
    const bool actualTopmost = NativeWindows::isTopmost(winId());
    if (m_dragging) {
        if (!m_windowLayerInitialized || !actualTopmost) {
            WId panel = 0;
            for (LiquidGlassWidget* widget : std::as_const(s_liveGlassWidgets))
                if (widget->m_panelWindow && widget->isVisible())
                    panel = widget->winId();
            NativeWindows::placeDragging(winId(), panel);
        }
    } else {
        QSet<WId> desktopWidgets;
        for (LiquidGlassWidget* widget : std::as_const(s_liveGlassWidgets))
            if (widget->m_desktopLayerEnabled && widget->isVisible())
                desktopWidgets.insert(widget->winId());
        if (!m_windowLayerInitialized || actualTopmost
            || !NativeWindows::isAtDesktopLayer(winId(), desktopWidgets))
            NativeWindows::placeDesktop(winId());
    }
    m_windowLayerInitialized = true;
#endif
}

void LiquidGlassWidget::preparePanelOpening()
{
    for (LiquidGlassWidget* widget : std::as_const(s_liveGlassWidgets)) {
        if (!widget->m_desktopLayerEnabled)
            continue;
        widget->finishWindowDrag();
        widget->m_windowLayerInitialized = false;
        widget->updateWindowLayer();
    }
}

void LiquidGlassWidget::finishWindowDrag()
{
    m_dragPending = false;
    if (!m_dragging)
        return;
    m_dragging = false;
    ++m_captureGeneration;
    m_compositeCaptureClock.invalidate();
    setInteractionActive(false);
    // Snap and hide the preview before demoting the card or taking its final
    // background, so the preview cannot become part of the stored material.
    windowDragFinished();
    if (directBackdropDuringDrag())
        refreshDragRendering();
    updateWindowLayer();
    // Drop pixels captured while this card was in the drag/topmost band
    // immediately, including when an earlier worker request is still pending.
    m_backdropCache.clear();
    m_compositeCanvas = {};
    QImage fallback = wallpaperBackdrop(frameGeometry(), devicePixelRatioF() * renderScale());
    if (!wallpaperOnlyWhenIdle())
        compositeGlassWindows(fallback, frameGeometry(), this);
    m_lastCompositedBackdrop = fallback;
    setBackgroundImage(fallback);
    updateRefreshRate();
    captureDesktopBackdrop();
    QTimer::singleShot(0, this, &LiquidGlassWidget::refreshBackdropTopology);
}

bool LiquidGlassWidget::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG" && message && m_desktopLayerEnabled
        && !m_dragging && isVisible()) {
        const auto* native = static_cast<MSG*>(message);
        if (native->message == WM_MOUSEACTIVATE) {
            *result = MA_NOACTIVATE;
            return true;
        }
        if (native->message == WM_WINDOWPOSCHANGING) {
            auto* position = reinterpret_cast<WINDOWPOS*>(native->lParam);
            if (position && !(position->flags & SWP_NOZORDER)) {
                // Prevent click/owner/Qt raises before Windows presents the
                // card above applications. Dragging and library panels retain
                // their own layer policy. Only change the insertion anchor:
                // Windows ignores NOACTIVATE/NOOWNERZORDER edits in this message.
                position->hwndInsertAfter = reinterpret_cast<HWND>(
                    NativeWindows::desktopInsertAfter(reinterpret_cast<WId>(native->hwnd)));
            }
        }
    }
#endif
    return QtGlassFlowScene::nativeEvent(eventType, message, result);
}

bool LiquidGlassWidget::event(QEvent* event)
{
#ifdef Q_OS_WIN
    if (event->type() == QEvent::Move || event->type() == QEvent::Resize
        || event->type() == QEvent::Show || event->type() == QEvent::Hide
        || event->type() == QEvent::ZOrderChange || event->type() == QEvent::WinIdChange) {
        NativeWindows::invalidate();
        queueCompositeRefresh();
    }
#endif
    if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate)
        finishWindowDrag();
    if (event->type() == QEvent::ZOrderChange && m_desktopLayerEnabled && !m_dragging) {
        m_windowLayerInitialized = false;
        QTimer::singleShot(0, this, &LiquidGlassWidget::updateWindowLayer);
    }
    if (event->type() == QEvent::WinIdChange && isVisible()) {
        m_windowLayerInitialized = false;
        QTimer::singleShot(0, this, [this]() { updateCaptureExclusion(); });
    }
    if (event->type() == QEvent::DevicePixelRatioChange) {
        ++m_captureGeneration;
        m_backdropCache.clear();
        m_compositeCanvas = {};
        m_lastCompositedBackdrop = {};
        m_compositeCaptureClock.invalidate();
        updateWindowMask();
        QTimer::singleShot(0, this, &LiquidGlassWidget::captureDesktopBackdrop);
    }
    return QtGlassFlowScene::event(event);
}

void LiquidGlassWidget::moveEvent(QMoveEvent* event)
{
    QtGlassFlowScene::moveEvent(event);
    if (m_desktopCaptureEnabled && isVisible()) {
        // Coalesce native move events through the existing sampler. A release
        // explicitly invalidates the age to refresh the final snapped cell.
        // During a drag mouseMoveEvent() performs the crop once after the
        // geometry has settled; doing it here as well caused two full QImage
        // copies per pointer event and produced visible judder.
        if (m_dragging)
            updateRefreshRate();
        else
            m_compositeCaptureClock.invalidate();
    }
}

void LiquidGlassWidget::mousePressEvent(QMouseEvent* event)
{
    if (m_mouseThrough) {
        event->ignore();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        m_dragPending = true;
        m_pressPosition = event->globalPosition().toPoint();
        m_dragOffset = m_pressPosition - frameGeometry().topLeft();
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
    if (m_dragPending && (event->buttons() & Qt::LeftButton)) {
        if (!m_dragging && (event->globalPosition().toPoint() - m_pressPosition).manhattanLength()
                            >= QApplication::startDragDistance()) {
            m_dragging = true;
            ++m_captureGeneration;
            m_compositeCaptureClock.invalidate();
            // Static cards normally stop their scene timer completely.  A
            // window drag is still an interactive animation even though no
            // internal glass object is being dragged, so explicitly enable
            // the active display's render cadence until finishWindowDrag()
            // disables it.
            // Without this, frames are presented only when the asynchronous
            // backdrop sampler completes (typically around four times a
            // second), independently of the selected render quality.
            setInteractionActive(true);
            windowDragStarted();
            if (directBackdropDuringDrag())
                refreshDragRendering();
            updateWindowLayer();
            updateRefreshRate();
        }
        if (m_dragging) {
            move(event->globalPosition().toPoint() - m_dragOffset);
            captureDesktopBackdrop();
        }
        event->accept();
        return;
    }
    QtGlassFlowScene::mouseMoveEvent(event);
}

void LiquidGlassWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        finishWindowDrag();
        event->accept();
        return;
    }
    QtGlassFlowScene::mouseReleaseEvent(event);
}
