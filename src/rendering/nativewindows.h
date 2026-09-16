#pragma once

#include <QRect>
#include <QSet>
#include <QVector>
#include <QtGui/qwindowdefs.h>
#include <functional>

class QObject;

// Native topology is shared by capture, occlusion and placement. Coordinates
// are physical desktop pixels; snapshots are immutable and safe on a worker.
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
const Stack& cachedStack(); // GUI thread only, at most one enumeration per frame
void invalidate();
int indexOf(const Stack& stack, WId id);
// Register the app's glass surfaces for internal desktop composition.  This
// is deliberately separate from SetWindowDisplayAffinity: the latter hides a
// window from every system screenshot, while the registry only prevents our
// own backdrop sampler from feeding a glass surface back into itself.
void registerGlassWindow(WId id);
void unregisterGlassWindow(WId id);
bool isGlassWindow(WId id);
// True while a Windows screenshot/clipping surface owns the foreground.
// This allows cards to become capturable for that short interval only.
bool isSystemCaptureActive();
// Temporarily hide visible, non-minimized application windows while keeping
// the supplied widget HWNDs and Windows shell infrastructure untouched.
HiddenWindows hideApplicationWindows(const QSet<WId>& preservedWindows);
void restoreApplicationWindows(const HiddenWindows& windows);
void placeDesktop(WId id);
void placeDragging(WId id, WId panelAbove = 0);
bool isTopmost(WId id);
void observeTopology(QObject* context, std::function<void()> changed);
}
