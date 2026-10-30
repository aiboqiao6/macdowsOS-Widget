#pragma once

#include <QRect>
#include <QSet>
#include <QVector>
#include <QtGui/qwindowdefs.h>
#include <functional>

class QObject;

// 捕获、遮挡和放置共用原生窗口拓扑；坐标为物理像素，不可变快照可安全交给工作线程。
namespace NativeWindows {
struct Window {
    WId id = 0;
    QRect bounds;
    bool desktop = false;
    bool opaque = false;
    bool excluded = false;
    bool topmost = false;
};
using Stack = QVector<Window>;
struct HiddenWindow {
    WId id = 0;
    bool maximized = false;
};
using HiddenWindows = QVector<HiddenWindow>;
Stack snapshot();
const Stack& cachedStack(); // 仅界面线程调用，每帧最多枚举一次。
void invalidate();
int indexOf(const Stack& stack, WId id);
// 注册玻璃窗口用于内部合成，与影响系统截图的 SetWindowDisplayAffinity 无关。
void registerGlassWindow(WId id);
void unregisterGlassWindow(WId id);
bool isGlassWindow(WId id);
// 创建 WGC 捕获项前检查句柄，排除临时弹窗、受保护窗口及未带 APPWINDOW 的工具窗口。
bool isCaptureCandidate(WId id);
// 检测前台系统截图覆盖层，用于临时暂停内部采样；不改变组件的系统截图可见性。
bool isSystemCaptureActive();
// 临时隐藏可见且未最小化的应用窗口，保留指定组件和系统桌面宿主。
HiddenWindows hideApplicationWindows(const QSet<WId>& preservedWindows);
void restoreApplicationWindows(const HiddenWindows& windows);
void placeDesktop(WId id);
WId desktopInsertAfter(WId id);
bool isAtDesktopLayer(WId id, const QSet<WId>& desktopWidgets);
void placeDragging(WId id, WId panelAbove = 0);
bool isTopmost(WId id);
void observeTopology(QObject* context, std::function<void()> changed);
}
