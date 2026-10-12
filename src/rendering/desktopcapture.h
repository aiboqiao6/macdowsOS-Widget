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
    QRegion valid; // 物理像素坐标；不包含目标上方的窗口。
    QRect area;
    quint64 scene = 0; // 下层外部窗口的身份、次序与几何位置。
    WId target = 0;
    qreal scale = 1;
    QRect targetBounds; // 捕获时的物理坐标。
    bool validated = false; // 交付前已在界面线程验证。
    quint64 topology = 0; // WGC 捕获源身份与次序，不包含位置。
};
// 接口几何使用 Qt 桌面逻辑坐标；捕获在单线程池执行，仅向仍存活的界面对象回调。
// latestOnly 仅合并同一接收对象与目标尚未执行的请求；被合并的请求仍以空帧完成以释放背压计数。
void request(const QRect& area, qreal pixelScale, WId target, QObject* context,
             std::function<void(Frame)> completed, int priority = 0, bool latestOnly = false);
Frame grab(const QRect& area, qreal pixelScale, WId target = 0);
bool validate(Frame& frame);
// 最后一个玻璃窗口销毁后释放持续捕获会话，也供独立测试清理使用。
void resetWorkerStreams();

// 只缓存验证过的外部像素；玻璃合成结果不得写回此缓存，以免产生反馈。
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
