#include "rendering/liquidglasswidget.h"

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

namespace {
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

namespace {
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

// GDI captures are intentionally performed with WDA_NONE so external
// screenshots can always include a card. The target HWND can therefore be
// present in the sampled frame; replace just that rectangle with a smooth
// interpolation of the pixels immediately outside its edge before the frame
// enters the backdrop cache. This keeps the sampler self-feedback-free while
// retaining the surrounding app/wallpaper colours instead of a hard fallback.
void scrubTargetFromCapture(DesktopCapture::Frame& frame, const QRect& target)
{
    if (frame.image.isNull() || frame.area.isEmpty() || target.isEmpty())
        return;
    const qreal sx = frame.image.width() / qreal(frame.area.width());
    const qreal sy = frame.image.height() / qreal(frame.area.height());
    const QRect clipped = target.intersected(frame.area);
    if (clipped.isEmpty())
        return;
    const int left = qBound(0, qFloor((clipped.left() - frame.area.left()) * sx),
                            frame.image.width() - 1);
    const int top = qBound(0, qFloor((clipped.top() - frame.area.top()) * sy),
                           frame.image.height() - 1);
    const int right = qBound(left, qCeil((clipped.right() + 1 - frame.area.left()) * sx) - 1,
                             frame.image.width() - 1);
    const int bottom = qBound(top, qCeil((clipped.bottom() + 1 - frame.area.top()) * sy) - 1,
                              frame.image.height() - 1);
    if (left <= 0 && right >= frame.image.width() - 1
        && top <= 0 && bottom >= frame.image.height() - 1)
        return;

    const QImage source = frame.image;
    QRegion usable = frame.valid;
    usable -= QRect(left, top, right - left + 1, bottom - top + 1);
    QPainter painter(&frame.image);
    for (int y = top; y <= bottom; ++y) {
        int sampleLeftX = left - 1;
        while (sampleLeftX >= 0 && !usable.contains(QPoint(sampleLeftX, y)))
            --sampleLeftX;
        int sampleRightX = right + 1;
        while (sampleRightX < source.width() && !usable.contains(QPoint(sampleRightX, y)))
            ++sampleRightX;
        if (sampleLeftX < 0 && sampleRightX >= source.width())
            continue;
        if (sampleLeftX < 0)
            sampleLeftX = sampleRightX;
        if (sampleRightX >= source.width())
            sampleRightX = sampleLeftX;
        const QColor leftColor = source.pixelColor(sampleLeftX, y);
        const QColor rightColor = source.pixelColor(sampleRightX, y);
        QLinearGradient gradient(QPointF(left, y), QPointF(qMax(left + 1, right), y));
        gradient.setColorAt(0.0, leftColor);
        gradient.setColorAt(1.0, rightColor);
        painter.fillRect(QRect(left, y, right - left + 1, 1), gradient);
    }
    painter.end();
}

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
    m_backdropTimer->setInterval(1000);
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
        if (isVisible() && m_desktopCaptureEnabled)
            captureDesktopBackdrop();
    });
}

LiquidGlassWidget::~LiquidGlassWidget()
{
    NativeWindows::unregisterGlassWindow(m_registeredGlassWindow);
    s_liveGlassWidgets.removeAll(this);
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
    // The card remains capturable by external screenshot APIs. The internal
    // sample uses a small runway around it so the target pixels can be
    // replaced from real desktop colours before entering the cache.
    const bool targetVisible = excluded && excluded->isVisible();
    const qreal scale = screen->devicePixelRatio() * renderScale;
    const QRect captureArea = targetVisible
        ? area.adjusted(-24, -24, 24, 24) : area;
    auto frame = DesktopCapture::grab(captureArea, scale,
                                      excluded && excluded->isVisible() ? excluded->winId() : 0);
#ifdef Q_OS_WIN
    if (targetVisible)
        scrubTargetFromCapture(frame, excluded->frameGeometry());
#endif
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
    // A visible panel can occupy virtually the entire requested rectangle.
    // Capture a small real-desktop runway around it so self-capture scrubbing
    // always has valid pixels on both horizontal sides; otherwise the panel
    // recursively enters its own backdrop and produces bright feedback
    // flashes on successive live updates.
    const QRect captureArea = isVisible()
        ? area.adjusted(-32, -32, 32, 32).intersected(screen->geometry())
        : area;
    // Keep WDA_NONE at all times. The target rectangle is scrubbed from the
    // returned frame instead, so Snipping Tool and screen recorders never see
    // an excluded/blank card.
    const QRect targetGeometry = frameGeometry();
    DesktopCapture::request(captureArea, scale, winId(), this,
        [this, area, captureArea, scale, targetGeometry,
         completed = std::move(completed)](DesktopCapture::Frame frame) {
#ifdef Q_OS_WIN
            scrubTargetFromCapture(frame, targetGeometry);
#endif
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

void LiquidGlassWidget::compositeGlassWindows(QImage& image, const QRect& area,
                                              const LiquidGlassWidget* excluded)
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
    const WId target = excluded && excluded->isVisible() ? excluded->winId() : 0;
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
        painter.drawImage(widget->frameGeometry(), snapshot);
        painter.restore();
    }
#else
    Q_UNUSED(image);
    Q_UNUSED(area);
    Q_UNUSED(excluded);
#endif
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
    // Keep the same live backdrop FBO path in both materials. In blur-only
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
        // Fewer passes are compensated with a slightly wider kernel so the
        // perceived softness stays close to the previous material.
        setBlurRadius(float(0.5 + amount * 13.5));
        // Two or three separable passes cover the useful radius range. The
        // old 5-pass ceiling multiplied fullscreen texture traffic without a
        // proportional visual gain on a small card.
        setBlurIterations(1 + qRound(amount * 2.0));
        setNoiseAmount(0.0f);
        setRefractionPower(0.0f);
    } else {
        setBlurRadius(float(0.5 + amount * 9.5));
        setBlurIterations(1 + qRound(amount * 2.0));
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
    if (enabled)
        finishWindowDrag();
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
    m_windowLayerInitialized = false;
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
    setRefreshInterval(lowPower ? 250 : 16);
    if (m_backdropTimer) {
        const bool dragging = m_dragging && !lowPower;
        m_backdropTimer->setTimerType(dragging ? Qt::PreciseTimer : Qt::CoarseTimer);
        const int interval = dragging ? 16 : (lowPower ? 250 : (m_unchangedCaptures >= 8 ? 100 : 33));
        if (m_backdropTimer->interval() != interval)
            m_backdropTimer->setInterval(interval);
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

void LiquidGlassWidget::applySystemBlurEffect()
{
#ifdef Q_OS_WIN
    if (!windowHandle() && !isVisible())
        return;
    const HWND window = reinterpret_cast<HWND>(winId());
    // Clear any stale DWM policy left by a previous process version. The
    // visible blur is rendered from the captured backdrop FBO so it follows the
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
    setSystemCaptureMode(NativeWindows::isSystemCaptureActive());
#endif
}

void LiquidGlassWidget::setSystemCaptureMode(bool active)
{
    if (m_systemCaptureActive == active)
        return;
    m_systemCaptureActive = active;
    ++m_captureGeneration;
    m_capturePending = false;
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

bool LiquidGlassWidget::captureCompositedBackdrop(QScreen* screen)
{
    if (!screen || !m_captureExcluded)
        return false;
    updateCompositeCrop();
    if (m_capturePending)
        return true;
    // Coarse timers may fire up to 5% early; allow that tolerance instead of
    // unintentionally skipping every other idle tick.
    const int maxAge = m_dragging ? 14 : qMax(1, m_backdropTimer->interval() * 94 / 100);
    if (m_compositeCaptureClock.isValid() && m_compositeCaptureClock.elapsed() < maxAge
        && m_lastCompositeGeometry == geometry())
        return true;
    if (m_dragging && m_compositeCaptureClock.isValid()
        && m_compositeCaptureClock.elapsed() < maxAge)
        return true;
    // A padded capture lets movement crop immediately from RAM while the
    // next desktop frame arrives from the worker. At most one request per
    // widget can be in flight, so input never builds a queue of stale frames.
    // Keep a small movement runway so crops stay responsive while dragging,
    // but avoid the 3x pixel area of the previous 128 px pad.
    // A small idle runway provides external pixels on both sides of the card
    // for self-capture scrubbing. It is proportional to the reference canvas
    // and remains negligible compared with the full desktop capture.
    const int padding = m_dragging ? 72 : 24;
    const QRect targetGeometry = frameGeometry();
    const QRect area = targetGeometry.adjusted(-padding, -padding, padding, padding);
    const quint64 generation = m_captureGeneration;
    m_capturePending = true;
    m_compositeCaptureClock.restart();
    // Store only the external desktop. Glass layers are composed at the
    // current position from completed frames, independently of capture age.
    const qreal scale = screen->devicePixelRatio() * renderScale();
    DesktopCapture::request(area, scale, winId(), this,
        [this, area, scale, targetGeometry, generation](DesktopCapture::Frame frame) {
#ifdef Q_OS_WIN
        scrubTargetFromCapture(frame, targetGeometry);
#endif
        m_capturePending = false;
        if (generation != m_captureGeneration || !isVisible() || !m_desktopCaptureEnabled)
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
    if (wallpaperOnlyWhenIdle() && !m_dragging)
        return;
    if (m_compositeCanvas.isNull() || !m_compositeArea.contains(frameGeometry()))
        return;
    if (!m_backdropCache.current()) {
        // Keep the last complete frame until a verified worker capture arrives.
        // Clearing the texture here creates a one-frame transparent/black flash
        // whenever the lower window stack changes during a drag.
        return;
    }
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
    QScreen* screen = QGuiApplication::screenAt(geometry().center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;

    // Desktop widgets stay visually quiet when not moving: their glass samples
    // the wallpaper only, avoiding periodic lower-window captures and texture
    // churn. Dragging switches to the live composited path below immediately.
    if (wallpaperOnlyWhenIdle() && !m_dragging) {
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
#ifdef Q_OS_WIN
    applySystemBlurEffect();
#endif
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
    } else if (!m_windowLayerInitialized || actualTopmost) {
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
        if (!m_dragging)
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
            windowDragStarted();
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
