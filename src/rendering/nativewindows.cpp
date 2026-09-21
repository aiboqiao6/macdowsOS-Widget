#include "nativewindows.h"
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <utility>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
NativeWindows::Stack cached;
QElapsedTimer cacheClock;
QMutex glassWindowsMutex;
QSet<WId> glassWindows;

class TopologyWatcher final : public QObject {
public:
    explicit TopologyWatcher(QObject* parent) : QObject(parent) {
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, this, [this]() {
            NativeWindows::invalidate();
            // Callbacks can create/destroy other windows. Iterate a copy and
            // guard each receiver, as Qt does for signal delivery.
            const auto pending = listeners;
            for (const auto& listener : pending)
                if (listener.first) listener.second();
            for (int i = listeners.size() - 1; i >= 0; --i)
                if (!listeners.at(i).first) listeners.removeAt(i);
        });
#ifdef Q_OS_WIN
        active = this;
        objectHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                                     &TopologyWatcher::changed, 0, 0, WINEVENT_OUTOFCONTEXT);
        foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                         &TopologyWatcher::changed, 0, 0, WINEVENT_OUTOFCONTEXT);
#endif
    }
    ~TopologyWatcher() override {
#ifdef Q_OS_WIN
        active = nullptr;
        if (objectHook) UnhookWinEvent(objectHook);
        if (foregroundHook) UnhookWinEvent(foregroundHook);
#endif
    }
    QList<std::pair<QPointer<QObject>, std::function<void()>>> listeners;
private:
    QTimer timer;
#ifdef Q_OS_WIN
    inline static TopologyWatcher* active = nullptr;
    HWINEVENTHOOK objectHook = nullptr;
    HWINEVENTHOOK foregroundHook = nullptr;
    static void CALLBACK changed(HWINEVENTHOOK, DWORD event, HWND, LONG object, LONG child, DWORD, DWORD) {
        if (!active || (event != EVENT_SYSTEM_FOREGROUND && (object != OBJID_WINDOW || child != 0)))
            return;
        if (event != EVENT_SYSTEM_FOREGROUND && event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE
            && event != EVENT_OBJECT_REORDER && event != EVENT_OBJECT_LOCATIONCHANGE)
            return;
        NativeWindows::invalidate();
        if (!active->timer.isActive())
            active->timer.start(0);
    }
#endif
};
}

void NativeWindows::observeTopology(QObject* context, std::function<void()> changed)
{
    static QPointer<TopologyWatcher> watcher;
    if (!watcher)
        watcher = new TopologyWatcher(QCoreApplication::instance());
    watcher->listeners.append({context, std::move(changed)});
}

NativeWindows::Stack NativeWindows::snapshot()
{
    Stack stack;
#ifdef Q_OS_WIN
    // EnumWindows is safe when other processes reorder/destroy windows.
    // A GetWindow(GW_HWNDNEXT) walk can revisit an HWND indefinitely.
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto& stack = *reinterpret_cast<Stack*>(parameter);
        using GetAttribute = HRESULT (WINAPI *)(HWND, DWORD, PVOID, DWORD);
        static const auto getAttribute = reinterpret_cast<GetAttribute>(
            GetProcAddress(GetModuleHandleW(L"dwmapi.dll"), "DwmGetWindowAttribute"));
        if (!IsWindowVisible(window) || IsIconic(window))
            return TRUE;
        DWORD cloaked = 0;
        if (getAttribute && SUCCEEDED(getAttribute(window, 14, &cloaked, sizeof(cloaked))) && cloaked)
            return TRUE;
        RECT rect{};
        if (!GetWindowRect(window, &rect) || rect.right <= rect.left || rect.bottom <= rect.top)
            return TRUE;
        wchar_t name[128]{};
        GetClassNameW(window, name, 128);
        const bool desktop = window == GetShellWindow() || wcscmp(name, L"Progman") == 0
                             || wcscmp(name, L"WorkerW") == 0;
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        DWORD affinity = 0;
        GetWindowDisplayAffinity(window, &affinity);
        // Glass surfaces are kept out of our own desktop sampler through a
        // process-local registry.  They remain capturable by Snipping Tool,
        // Game Bar and other system screenshot/recording APIs because their
        // native display affinity is no longer WDA_EXCLUDEFROMCAPTURE.
        const bool excluded = affinity == 0x11
                              || NativeWindows::isGlassWindow(reinterpret_cast<WId>(window));
        stack.append({reinterpret_cast<WId>(window),
                      QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top),
                      desktop, !desktop && !(style & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) && !excluded,
                      excluded, bool(style & WS_EX_TOPMOST)});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&stack));
#endif
    return stack;
}

const NativeWindows::Stack& NativeWindows::cachedStack()
{
    if (!cacheClock.isValid() || cacheClock.elapsed() >= 12) {
        cached = snapshot();
        cacheClock.restart();
    }
    return cached;
}

void NativeWindows::invalidate() { cacheClock.invalidate(); }

void NativeWindows::registerGlassWindow(WId id)
{
    if (!id)
        return;
    QMutexLocker locker(&glassWindowsMutex);
    glassWindows.insert(id);
    cacheClock.invalidate();
}

void NativeWindows::unregisterGlassWindow(WId id)
{
    if (!id)
        return;
    QMutexLocker locker(&glassWindowsMutex);
    glassWindows.remove(id);
    cacheClock.invalidate();
}

bool NativeWindows::isGlassWindow(WId id)
{
    if (!id)
        return false;
    QMutexLocker locker(&glassWindowsMutex);
    return glassWindows.contains(id);
}

bool NativeWindows::isSystemCaptureActive()
{
#ifdef Q_OS_WIN
    const HWND foreground = GetForegroundWindow();
    if (!foreground)
        return false;

    wchar_t className[128]{};
    GetClassNameW(foreground, className, 128);
    const QString windowClass = QString::fromWCharArray(className).toLower();
    if (windowClass.contains(QStringLiteral("snipping"))
        || windowClass.contains(QStringLiteral("screenclipping"))
        || windowClass.contains(QStringLiteral("screensketch")))
        return true;

    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    if (!processId)
        return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return false;
    wchar_t path[1024]{};
    DWORD length = DWORD(std::size(path));
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &length);
    CloseHandle(process);
    if (!ok || !length)
        return false;
    const QString executable = QString::fromWCharArray(path, int(length)).section(
        QLatin1Char('\\'), -1).toLower();
    return executable == QStringLiteral("snippingtool.exe")
           || executable == QStringLiteral("screenclippinghost.exe")
           || executable == QStringLiteral("screensketch.exe")
           || executable == QStringLiteral("gamebar.exe");
#else
    return false;
#endif
}

NativeWindows::HiddenWindows NativeWindows::hideApplicationWindows(
    const QSet<WId>& preservedWindows)
{
    HiddenWindows hidden;
#ifdef Q_OS_WIN
    struct Enumeration {
        const QSet<WId>* preserved = nullptr;
        HiddenWindows* hidden = nullptr;
    } enumeration{&preservedWindows, &hidden};

    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto* enumeration = reinterpret_cast<Enumeration*>(parameter);
        const WId id = reinterpret_cast<WId>(window);
        if (!IsWindowVisible(window) || IsIconic(window)
            || enumeration->preserved->contains(id))
            return TRUE;

        wchar_t className[128]{};
        GetClassNameW(window, className, int(std::size(className)));
        const bool shellInfrastructure = window == GetShellWindow()
            || wcscmp(className, L"Progman") == 0
            || wcscmp(className, L"WorkerW") == 0
            || wcscmp(className, L"Shell_TrayWnd") == 0
            || wcscmp(className, L"Shell_SecondaryTrayWnd") == 0
            || wcscmp(className, L"NotifyIconOverflowWindow") == 0;
        if (shellInfrastructure)
            return TRUE;

        const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
        const LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if ((style & WS_CHILD)
            || ((extendedStyle & WS_EX_TOOLWINDOW)
                && !(extendedStyle & WS_EX_APPWINDOW))
            || ((extendedStyle & WS_EX_NOACTIVATE)
                && !(extendedStyle & WS_EX_APPWINDOW)))
            return TRUE;

        // Captioned windows and explicit app windows are the user-facing
        // top-level windows. Skipping anonymous popup/overlay HWNDs prevents
        // tooltips, input surfaces and shell implementation windows from
        // being mistaken for applications.
        if (!(style & WS_CAPTION) && !(extendedStyle & WS_EX_APPWINDOW))
            return TRUE;

        DWORD cloaked = 0;
        using GetAttribute = HRESULT (WINAPI *)(HWND, DWORD, PVOID, DWORD);
        static const auto getAttribute = reinterpret_cast<GetAttribute>(
            GetProcAddress(GetModuleHandleW(L"dwmapi.dll"),
                           "DwmGetWindowAttribute"));
        if (getAttribute
            && SUCCEEDED(getAttribute(window, 14, &cloaked, sizeof(cloaked)))
            && cloaked)
            return TRUE;

        enumeration->hidden->append({id, IsZoomed(window) != FALSE});
        ShowWindowAsync(window, SW_HIDE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&enumeration));
    invalidate();
#else
    Q_UNUSED(preservedWindows);
#endif
    return hidden;
}

void NativeWindows::restoreApplicationWindows(const HiddenWindows& windows)
{
#ifdef Q_OS_WIN
    // Restore bottom-to-top to retain the relative stacking order captured by
    // EnumWindows. SWP_SHOWWINDOW preserves the window's existing normal or
    // maximized placement without activating it, unlike ShowWindow(SW_SHOW).
    for (auto it = windows.crbegin(); it != windows.crend(); ++it) {
        const HWND window = reinterpret_cast<HWND>(it->id);
        if (!IsWindow(window))
            continue;
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                     | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW
                     | SWP_ASYNCWINDOWPOS);
    }
    invalidate();
#else
    Q_UNUSED(windows);
#endif
}

int NativeWindows::indexOf(const Stack& stack, WId id)
{
    for (int i = 0; i < stack.size(); ++i)
        if (stack.at(i).id == id)
            return i;
    return -1;
}

bool NativeWindows::isTopmost(WId id)
{
#ifdef Q_OS_WIN
    return GetWindowLongPtrW(reinterpret_cast<HWND>(id), GWL_EXSTYLE) & WS_EX_TOPMOST;
#else
    Q_UNUSED(id);
    return false;
#endif
}

void NativeWindows::placeDesktop(WId id)
{
#ifdef Q_OS_WIN
    const HWND window = reinterpret_cast<HWND>(id);
    constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    // Never insert after a topmost HWND: Windows would promote this window
    // back into the topmost band, even immediately after HWND_NOTOPMOST.
    HWND previousNormal = HWND_TOP;
    HWND after = HWND_BOTTOM;
    for (const Window& entry : snapshot()) {
        if (entry.id == id || entry.topmost)
            continue;
        if (entry.desktop) { after = previousNormal; break; }
        previousNormal = reinterpret_cast<HWND>(entry.id);
    }
    SetWindowPos(window, after, 0, 0, 0, 0, flags);
    // An anchor can be promoted concurrently by another process.
    if (isTopmost(id))
        SetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0, flags);
#else
    Q_UNUSED(id);
#endif
    invalidate();
}

void NativeWindows::placeDragging(WId id, WId panelAbove)
{
#ifdef Q_OS_WIN
    constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    SetWindowPos(reinterpret_cast<HWND>(id), HWND_TOPMOST, 0, 0, 0, 0, flags);
    if (panelAbove && IsWindowVisible(reinterpret_cast<HWND>(panelAbove)))
        SetWindowPos(reinterpret_cast<HWND>(id), reinterpret_cast<HWND>(panelAbove), 0, 0, 0, 0, flags);
#else
    Q_UNUSED(id);
    Q_UNUSED(panelAbove);
#endif
    invalidate();
}
