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
constexpr int kWallpaperIdleIntervalMs = 10000; // 关闭实时采样时每十秒刷新一次。

// 全进程共享壁纸画布，每个组件只保留局部裁剪，减少高 DPI 桌面的内存占用。
QImage s_sharedWallpaperCanvas;
QString s_sharedWallpaperSignature;
QString s_sharedWallpaperScreenName;
QSize s_sharedWallpaperPixelSize;


QList<LiquidGlassWidget*> s_liveGlassWidgets;
QPointer<QTimer> s_compositeRefreshTimer;
quint64 s_surfaceRevision = 0;

}

LiquidGlassWidget::LiquidGlassWidget(QWidget* parent)
    : QtGlassFlowScene(parent)
{
    s_liveGlassWidgets.append(this);
    connect(this, &QtGlassFlowScene::frameRendered, this, &LiquidGlassWidget::queueCompositeRefresh);
    setWindowTitle(QStringLiteral("macdowsOS Widget"));
    // 原生窗口层级由 updateWindowLayer 动态管理；静态置底标志会干扰拖动时临时提升。
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

    m_glassObjectIndex = addGlassObject(QPointF(m_glassMarginsX, m_glassMarginsY),
                                        QSizeF(width() - m_glassMarginsX * 2,
                                               height() - m_glassMarginsY * 2),
                                        LiquidGlassStyle::kShapePower);
    setGlassObjectCornerRadius(m_glassObjectIndex, float(m_glassRadius));
    updateWindowMask();

    // 界面定时器只提交数量受限的工作请求并裁剪缓存，背景不变时跳过纹理上传与模糊。
    m_backdropTimer = new QTimer(this);
    m_backdropTimer->setTimerType(Qt::CoarseTimer);
    m_backdropTimer->setInterval(kWallpaperIdleIntervalMs);
    connect(m_backdropTimer, &QTimer::timeout,
            this, &LiquidGlassWidget::captureDesktopBackdrop);
    // 检测系统截图工具，选择区域期间暂停背景采样。
    m_captureMonitorTimer = new QTimer(this);
    m_captureMonitorTimer->setTimerType(Qt::CoarseTimer);
    m_captureMonitorTimer->setInterval(200);
    connect(m_captureMonitorTimer, &QTimer::timeout,
            this, &LiquidGlassWidget::updateSystemCaptureMode);
    NativeWindows::observeTopology(this, [this]() {
        // 外部窗口层级变化时及时重新合成，不等待空闲刷新周期。
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
    // 内部捕获通过进程内注册表排除玻璃窗口；系统截图仍可正常捕获组件。
    const bool targetVisible = excluded && excluded->isVisible();
    const qreal scale = screen->devicePixelRatio() * renderScale;
    const QRect captureArea = targetVisible
        ? area.adjusted(-48, -48, 48, 48) : area;
    const WId target = excluded && excluded->isVisible() ? excluded->winId() : 0;
    auto frame = DesktopCapture::grab(captureArea, scale, target);
    // 同步捕获期间窗口层级可能变化，失败后重试一次，避免向调用者交付不完整画面。
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
    // 稳定面板只捕获当前矩形，无需为拖动预留额外背景区域。
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
        // 窗口重建期间层级暂时未知，不能加入未经验证的下层窗口。
        if (targetIndex < 0)
            return;
    }
    QPainter painter(&image);
    painter.scale(image.width() / qreal(area.width()), image.height() / qreal(area.height()));
    painter.translate(-area.topLeft());
    // 按从底到顶的次序合成同一份已完成帧，跳过隐藏窗口和桌面宿主。
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
        // 捕获缓存剔除了整个玻璃矩形；先用下层窗口的原始背景补洞，再叠加其透明输出。
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
        // 修改定时器间隔不会自动启动，偏好变更时需要显式恢复采样。
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
    // 低频刷新只应用于鼠标穿透卡片，保留可交互组件的响应速度。
    const bool lowPower = m_lowPowerRefresh && m_mouseThrough;
    const int displayInterval = activeDisplayInterval();
    setRefreshInterval(lowPower ? 250 : displayInterval);
    // 实时背景与内容动画独立，静止时也保持显示器刷新节奏。
    const bool desktopDrag = desktopDragActive();
    setContinuousRenderingEnabled(m_liveBackdropEnabled && !desktopDrag);
    if (m_backdropTimer) {
        const bool dragging = m_dragging && !lowPower;
        const bool live = !wallpaperOnlyWhenIdle() && !desktopDrag;
        // 实时采样使用精确定时器；在途请求数量限制负责背压，避免粗略定时器延迟。
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
    // 原生窗口区域是二值裁剪，会破坏小数 DPI 的平滑圆角；使用帧缓冲 alpha 保留抗锯齿。
    clearMask();
#else
    QPainterPath shape;
    // 窗口区域与完整玻璃表面保持一致，额外内缩会出现第二层边框。
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
    // 超采样画质使用相应分辨率的壁纸源，避免仅放大低分辨率图像。
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
            // 拉伸与跨屏壁纸；单显示器下跨屏等同于拉伸。
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
    // 关闭 Windows 非客户区矩形描边，圆角边缘由 GL 着色器和覆盖层统一绘制。
    using SetDwmAttribute = HRESULT (WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    static const auto setDwmAttribute = reinterpret_cast<SetDwmAttribute>(
        GetProcAddress(GetModuleHandleW(L"dwmapi.dll"), "DwmSetWindowAttribute"));
    if (setDwmAttribute) {
        const int disabled = 1; // 禁用非客户区渲染。
        const DWORD noBorder = 0xfffffffe; // 不绘制 DWM 边框颜色。
        setDwmAttribute(reinterpret_cast<HWND>(id), 2, &disabled, sizeof(disabled));
        setDwmAttribute(reinterpret_cast<HWND>(id), 34, &noBorder, sizeof(noBorder));
    }
    if (m_registeredGlassWindow && m_registeredGlassWindow != id)
        NativeWindows::unregisterGlassWindow(m_registeredGlassWindow);
    m_registeredGlassWindow = id;
    NativeWindows::registerGlassWindow(id);
    // 进程内注册标记只控制自身背景合成，不改变系统截图或录屏策略。
    m_captureExcluded = true;
#endif
}

void LiquidGlassWidget::updateSystemCaptureMode()
{
#ifdef Q_OS_WIN
    if (!isVisible())
        return;
    // 定时检查作为原生层级事件的后备，仅修复层级错误的卡片。
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

    // 系统截图覆盖层活动时暂停采样，避免把过渡画面写入背景缓存。
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
    // 向下取整刷新间隔，避免低于显示器频率；最终呈现由 Qt 和 DWM 同步。
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
    // 粗略定时器允许提前触发，容忍该误差可避免隔次跳过空闲刷新。
    const int maxAge = live && !m_dragging ? 0
                     : (m_dragging ? 14 : qMax(1, m_backdropTimer->interval() * 94 / 100));
    if (m_compositeCaptureClock.isValid() && m_compositeCaptureClock.elapsed() < maxAge
        && m_lastCompositeGeometry == geometry())
        return true;
    if (m_dragging && m_compositeCaptureClock.isValid()
        && m_compositeCaptureClock.elapsed() < maxAge)
        return true;
    // 捕获区域预留少量移动余量，并限制为两个在途请求，兼顾拖动响应与像素传输量。
    const int padding = live && !m_dragging ? 0 : (m_dragging ? 96 : 56);
    const QRect targetGeometry = frameGeometry();
    const QRect area = targetGeometry.adjusted(-padding, -padding, padding, padding);
    const quint64 generation = m_captureGeneration;
    ++m_capturesInFlight;
    m_compositeCaptureClock.restart();
    // 只缓存外部桌面；玻璃层按当前位置与已完成帧单独合成。
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
        // 交付结果后立即提交下一次捕获，避免排队定时器额外损失一帧。
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
    // 拖动时背景由当前壁纸与下层卡片即时合成，不能被异步旧结果覆盖。
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
    // 无变化的裁剪不读取捕获像素；真正合成前仍须验证层级与重叠关系。
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
    // 使用 QImage 的优化行比较，避免逐像素哈希的完整扫描开销。
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

    // 拖动时普通应用窗口已隐藏，直接合成共享壁纸与下层玻璃，无需等待异步捕获。
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

    // 关闭实时背景时，静止卡片只采样壁纸；拖动时切换至下层合成。
    if ((wallpaperOnlyWhenIdle() || (directBackdropDuringDrag() && desktopDragActive()))
        && !m_dragging) {
        // 从遮挡暂停切回壁纸模式时恢复绘制循环，并同步原生窗口层级。
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
    // 完全遮挡的卡片跳过捕获与 GPU 绘制，但继续检测可见性变化。
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
    // 超采样壁纸使用平滑缩放，避免最近邻放大产生重复像素块。
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
    // 显示窗口可能重置层级，强制同步一次原生层级。
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
    // 隐藏时同时停止背景采样与场景定时器，避免后台无效唤醒。
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
    // 先吸附并隐藏放置预览，再降低层级与捕获最终背景，避免把预览写入材质。
    windowDragFinished();
    if (directBackdropDuringDrag())
        refreshDragRendering();
    updateWindowLayer();
    // 立即丢弃拖动或临时置顶期间的旧捕获结果，包括尚未完成的请求。
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
                // 拖动与面板之外的卡片只修正插入锚点，避免点击或 Qt 提升窗口时盖住应用。
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
        // 合并原生移动事件；拖动时在几何稳定后只裁剪一次，释放时立即刷新最终位置。
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
            // 窗口拖动期间显式启用显示器刷新节奏，结束后恢复静态卡片的事件驱动绘制。
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
