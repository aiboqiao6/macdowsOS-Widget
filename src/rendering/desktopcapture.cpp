#include "desktopcapture.h"
#include "nativewindows.h"
#include <QPainter>
#include <cstring>

#include <QGuiApplication>
#include <QCache>
#include <QHash>
#include <QPointer>
#include <QRunnable>
#include <QScreen>
#include <QThreadPool>
#include <QVector>
#include <QtMath>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
struct CapturePart {
    QRect source;
    QRect destination;
};

#ifdef Q_OS_WIN
class MonitorOrigins final : public QObject {
public:
    explicit MonitorOrigins(QObject* parent) : QObject(parent) {
        const auto watch = [this](QScreen* screen) {
            connect(screen, &QScreen::geometryChanged, this, [this]() { dirty = true; });
            dirty = true;
        };
        for (QScreen* screen : QGuiApplication::screens())
            watch(screen);
        connect(qApp, &QGuiApplication::screenAdded, this, watch);
        connect(qApp, &QGuiApplication::screenRemoved, this, [this]() { dirty = true; });
    }
    QPoint get(QScreen* screen) {
        if (dirty) {
            positions.clear();
            EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
                auto* origins = reinterpret_cast<MonitorOrigins*>(data);
                MONITORINFOEXW info{};
                info.cbSize = sizeof(info);
                if (GetMonitorInfoW(monitor, &info))
                    origins->positions.insert(QString::fromWCharArray(info.szDevice),
                                               QPoint(info.rcMonitor.left, info.rcMonitor.top));
                return TRUE;
            }, reinterpret_cast<LPARAM>(this));
            dirty = false;
        }
        return positions.value(screen->name(), screen->geometry().topLeft());
    }
private:
    bool dirty = true;
    QHash<QString, QPoint> positions;
};
#endif

QVector<CapturePart> captureParts(const QRect& area, qreal scale)
{
    QVector<CapturePart> parts;
    for (QScreen* screen : QGuiApplication::screens()) {
        const QRect intersection = area.intersected(screen->geometry());
        if (intersection.isEmpty())
            continue;
        QPoint nativeOrigin = screen->geometry().topLeft();
#ifdef Q_OS_WIN
        static QPointer<MonitorOrigins> origins;
        if (!origins)
            origins = new MonitorOrigins(qApp);
        nativeOrigin = origins->get(screen);
#endif
        const qreal dpr = screen->devicePixelRatio();
        const QPoint local = intersection.topLeft() - screen->geometry().topLeft();
        const QPoint offset = intersection.topLeft() - area.topLeft();
        parts.append({QRect(nativeOrigin + QPoint(qRound(local.x() * dpr), qRound(local.y() * dpr)),
                            QSize(qRound(intersection.width() * dpr), qRound(intersection.height() * dpr))),
                      QRect(QPoint(qRound(offset.x() * scale), qRound(offset.y() * scale)),
                            QPoint(qRound((offset.x() + intersection.width()) * scale) - 1,
                                   qRound((offset.y() + intersection.height()) * scale) - 1))});
    }
    return parts;
}

QImage grabParts(const QVector<CapturePart>& parts, const QSize& size)
{
    if (parts.isEmpty() || size.isEmpty())
        return {};
#ifdef Q_OS_WIN
    // Reuse the DIB and memory DC on the capture thread. Only the reduced
    // pixels are copied back to Qt; no full-screen pixmap or GPU readback.
    struct Surface {
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous = nullptr;
        void* bits = nullptr;
        QSize size;
        ~Surface() {
            if (previous) SelectObject(dc, previous);
            if (bitmap) DeleteObject(bitmap);
            if (dc) DeleteDC(dc);
        }
        bool resize(const QSize& next) {
            if (!dc) return false;
            if (size == next && bitmap) return true;
            if (previous) SelectObject(dc, previous);
            if (bitmap) DeleteObject(bitmap);
            bitmap = nullptr;
            bits = nullptr;
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = next.width();
            info.bmiHeader.biHeight = -next.height();
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!bitmap) return false;
            previous = SelectObject(dc, bitmap);
            size = next;
            return true;
        }
    };
    // Different card sizes share the worker. Retain a bounded set of DIBs
    // instead of reallocating when a clock capture follows a wide card.
    thread_local QCache<quint64, Surface> surfaces(16 * 1024);
    const quint64 key = (quint64(size.width()) << 32) | quint32(size.height());
    Surface* cached = surfaces.object(key);
    std::unique_ptr<Surface> oversized;
    if (!cached) {
        auto* created = new Surface;
        if (!created->resize(size)) {
            delete created;
            return {};
        }
        const qint64 cost = qMax<qint64>(1, qint64(size.width()) * size.height() / 256);
        if (cost > surfaces.maxCost())
            oversized.reset(created);
        else
            surfaces.insert(key, created, int(cost));
        cached = created;
    }
    Surface& surface = *cached;
    HDC desktop = GetDC(nullptr);
    if (!desktop)
        return {};
    PatBlt(surface.dc, 0, 0, size.width(), size.height(), BLACKNESS);
    SetStretchBltMode(surface.dc, COLORONCOLOR);
    bool ok = true;
    for (const CapturePart& part : parts) {
        const QRect& s = part.source;
        const QRect& d = part.destination;
        ok = StretchBlt(surface.dc, d.x(), d.y(), d.width(), d.height(),
                        desktop, s.x(), s.y(), s.width(), s.height(),
                        SRCCOPY | CAPTUREBLT) != FALSE && ok;
    }
    GdiFlush();
    ReleaseDC(nullptr, desktop);
    if (!ok)
        return {};
    return QImage(static_cast<const uchar*>(surface.bits), size.width(), size.height(),
                  size.width() * 4, QImage::Format_RGB32).copy();
#else
    Q_UNUSED(parts);
    Q_UNUSED(size);
    return {};
#endif
}

struct Plan {
    QRegion valid;
    quint64 scene = 1469598103934665603ull;
};

Plan capturePlan(const QVector<CapturePart>& parts, const QSize& size, WId target,
                 const NativeWindows::Stack& stack)
{
    const int targetIndex = target ? NativeWindows::indexOf(stack, target) : -1;
    if (target && targetIndex < 0)
        return {{}, 0};
    Plan plan{QRegion(QRect(QPoint(), size))};
    const auto hash = [&plan](quint64 value) { plan.scene = (plan.scene ^ value) * 1099511628211ull; };
    for (int i = 0; i < stack.size(); ++i) {
        const auto& window = stack.at(i);
        if (window.id == target || window.excluded)
            continue;
        bool intersects = false;
        for (const auto& part : parts) {
            // Shadows and DWM's rounded frame extend beyond GetWindowRect on
            // some themes. Be conservative even for layered/translucent covers.
            const QRect bounds = window.bounds.adjusted(-16, -16, 16, 16);
            if (!bounds.intersects(part.source))
                continue;
            intersects = true;
            if (i < targetIndex) {
                const QRect overlap = bounds.intersected(part.source);
                const qreal sx = part.destination.width() / qreal(part.source.width());
                const qreal sy = part.destination.height() / qreal(part.source.height());
                const int left = qFloor((overlap.left() - part.source.left()) * sx);
                const int top = qFloor((overlap.top() - part.source.top()) * sy);
                const int right = qCeil((overlap.right() + 1 - part.source.left()) * sx);
                const int bottom = qCeil((overlap.bottom() + 1 - part.source.top()) * sy);
                plan.valid -= QRect(part.destination.topLeft() + QPoint(left, top), QSize(right - left, bottom - top));
            }
        }
        if (i > targetIndex && intersects) {
            hash(window.id);
            hash(window.bounds.x()); hash(window.bounds.y());
            hash(window.bounds.width()); hash(window.bounds.height());
        }
    }
    return plan;
}

Plan capturePlan(const QVector<CapturePart>& parts, const QSize& size, WId target)
{
    return capturePlan(parts, size, target, NativeWindows::snapshot());
}

DesktopCapture::Frame grabFrame(const QVector<CapturePart>& parts, const QSize& size,
                                const QRect& area, qreal scale, WId target, const Plan& queued)
{
    const Plan before = capturePlan(parts, size, target);
    if (!queued.scene || queued.scene != before.scene)
        return {};
    if ((queued.valid & before.valid).isEmpty()) {
        QImage blank(size, QImage::Format_RGB32);
        blank.fill(Qt::black);
        return {std::move(blank), {}, area, before.scene, target, scale};
    }
    QImage image = grabParts(parts, size);
    const Plan after = capturePlan(parts, size, target);
    if (before.scene != after.scene || image.isNull())
        return {};
    return {std::move(image), queued.valid & before.valid & after.valid, area, after.scene, target, scale};
}

class CaptureWorker final : public QObject {
public:
    explicit CaptureWorker(QObject* parent) : QObject(parent) {
        pool.setMaxThreadCount(1);
        pool.setExpiryTimeout(-1);
    }
    ~CaptureWorker() override { pool.clear(); pool.waitForDone(); }
    QThreadPool pool;
};
}

void DesktopCapture::request(const QRect& area, qreal pixelScale, WId target, QObject* context,
                             std::function<void(Frame)> completed, int priority)
{
    static QPointer<CaptureWorker> worker;
    if (!worker)
        worker = new CaptureWorker(qApp);
    CaptureWorker* owner = worker;
    const auto parts = captureParts(area, pixelScale);
    const QSize size(qMax(1, qRound(area.width() * pixelScale)),
                     qMax(1, qRound(area.height() * pixelScale)));
    const auto queued = capturePlan(parts, size, target);
    const QPointer<QObject> guard(context);
    owner->pool.start(QRunnable::create([owner, parts, size, area, pixelScale, target, queued, guard,
                                       completed = std::move(completed)]() {
        Frame frame = grabFrame(parts, size, area, pixelScale, target, queued);
        QMetaObject::invokeMethod(owner, [guard, completed, frame = std::move(frame)]() mutable {
            if (guard) {
                validate(frame);
                completed(std::move(frame));
            }
        }, Qt::QueuedConnection);
    }), priority);
}

DesktopCapture::Frame DesktopCapture::grab(const QRect& area, qreal pixelScale, WId target)
{
    const auto parts = captureParts(area, pixelScale);
    const QSize size(qMax(1, qRound(area.width() * pixelScale)),
                     qMax(1, qRound(area.height() * pixelScale)));
    return grabFrame(parts, size, area, pixelScale, target, capturePlan(parts, size, target));
}

bool DesktopCapture::validate(Frame& frame)
{
    if (frame.image.isNull() || !frame.scene)
        return false;
    const Plan current = capturePlan(captureParts(frame.area, frame.scale), frame.image.size(), frame.target);
    if (current.scene != frame.scene) {
        frame = {};
        return false;
    }
    frame.valid &= current.valid;
    return true;
}

void DesktopCapture::Cache::clear()
{
    image = {};
    area = {};
    scene = 0;
    target = 0;
    recentlyCovered = {};
    coverageClock.invalidate();
}

bool DesktopCapture::Cache::current() const
{
    return scene && scene == capturePlan(captureParts(area, scale), image.size(), target,
                                         NativeWindows::cachedStack()).scene;
}

QImage DesktopCapture::Cache::merge(Frame frame, const std::function<QImage()>& fallback)
{
    if (!validate(frame))
        return {};
    if (area != frame.area || image.size() != frame.image.size() || scene != frame.scene) {
        const QImage previous = image;
        const QRect previousArea = area;
        const bool sameScene = scene == frame.scene;
        const QImage base = fallback();
        image = base.isNull() ? QImage() : base.scaled(frame.image.size(), Qt::IgnoreAspectRatio,
                                                       Qt::FastTransformation).convertToFormat(QImage::Format_RGB32);
        if (image.isNull()) {
            image = QImage(frame.image.size(), QImage::Format_RGB32);
            image.fill(Qt::black);
        }
        if (sameScene && !previous.isNull() && previousArea.intersects(frame.area)) {
            const QRect overlap = previousArea.intersected(frame.area);
            const qreal oldX = previous.width() / qreal(previousArea.width());
            const qreal oldY = previous.height() / qreal(previousArea.height());
            const qreal newX = image.width() / qreal(frame.area.width());
            const qreal newY = image.height() / qreal(frame.area.height());
            QPainter painter(&image);
            painter.drawImage(QRectF((overlap.x() - frame.area.x()) * newX,
                                     (overlap.y() - frame.area.y()) * newY,
                                     overlap.width() * newX, overlap.height() * newY), previous,
                              QRectF((overlap.x() - previousArea.x()) * oldX,
                                     (overlap.y() - previousArea.y()) * oldY,
                                     overlap.width() * oldX, overlap.height() * oldY));
        }
        area = frame.area;
        scene = frame.scene;
        recentlyCovered = {};
        coverageClock.invalidate();
    }
    target = frame.target;
    scale = frame.scale;
    // DWM can present the previous position for a frame after native geometry
    // changes. Hold newly uncovered pixels briefly instead of admitting ghosts.
    const QRegion covered = QRegion(frame.image.rect()) - frame.valid;
    if (!coverageClock.isValid() || coverageClock.elapsed() > 120) {
        recentlyCovered = covered;
        coverageClock.restart();
    }
    const QRegion valid = frame.valid - recentlyCovered;
    recentlyCovered |= covered;
    if (valid.isEmpty())
        return image;
    if (valid == QRegion(image.rect())) {
        // Retain the shared image/cache key on static desktops. Changed full
        // frames can be adopted directly without a second allocation or blit.
        if (image != frame.image)
            image = std::move(frame.image);
        return image;
    }
    bool changed = false;
    for (const QRect& rect : valid) {
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            if (std::memcmp(image.constScanLine(y) + rect.x() * 4,
                            frame.image.constScanLine(y) + rect.x() * 4, size_t(rect.width()) * 4)) {
                changed = true;
                break;
            }
        }
        if (changed) break;
    }
    if (!changed)
        return image;
    QPainter painter(&image);
    painter.setClipRegion(valid);
    painter.drawImage(0, 0, frame.image);
    painter.end();
    return image;
}
