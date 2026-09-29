#pragma once

#include <QImage>
#include <QElapsedTimer>
#include <QRect>
#include <QRegion>
#include <QtGui/qwindowdefs.h>
#include <functional>

class QObject;

namespace DesktopCapture {
struct Frame {
    QImage image;
    QRegion valid; // pixel coordinates; never includes a window above target
    QRect area;
    quint64 scene = 0; // identity/order/geometry of lower external windows
    WId target = 0;
    qreal scale = 1;
    QRect targetBounds; // physical coordinates at the instant of capture
    bool validated = false; // checked on the GUI thread immediately before delivery
    quint64 topology = 0; // WGC source identities/order, independent of their positions
};
// All geometry is in Qt desktop coordinates. Capture is performed on a single
// worker pool; callbacks run on the GUI thread only while their context is alive.
void request(const QRect& area, qreal pixelScale, WId target, QObject* context,
             std::function<void(Frame)> completed, int priority = 0);
Frame grab(const QRect& area, qreal pixelScale, WId target = 0);
bool validate(Frame& frame);
// Release persistent WGC sessions after the last glass surface is destroyed.
// This is also useful for deterministic teardown between independent hosts.
void resetWorkerStreams();

// Holds only verified external pixels. Glass surfaces are composed afterwards
// and must never be written back to this cache (which would cause feedback).
class Cache {
public:
    QImage merge(Frame frame, const std::function<QImage()>& fallback);
    bool current() const;
    void clear();
private:
    QImage image;
    QRect area;
    quint64 scene = 0;
    quint64 topology = 0;
    WId target = 0;
    qreal scale = 1;
    QRegion recentlyCovered;
    QElapsedTimer coverageClock;
};
}
