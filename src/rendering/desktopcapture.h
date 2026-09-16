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
};
// All geometry is in Qt desktop coordinates. Capture is performed on a single
// worker; callbacks run on the GUI thread only while their context is alive.
void request(const QRect& area, qreal pixelScale, WId target, QObject* context,
             std::function<void(Frame)> completed, int priority = 0);
Frame grab(const QRect& area, qreal pixelScale, WId target = 0);
bool validate(Frame& frame);

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
    WId target = 0;
    qreal scale = 1;
    QRegion recentlyCovered;
    QElapsedTimer coverageClock;
};
}
