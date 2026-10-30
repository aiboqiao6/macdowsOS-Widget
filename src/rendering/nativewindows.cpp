#include "nativewindows.h"
#include "renderdiagnostics.h"
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

#ifdef Q_OS_WIN
bool isTransientCaptureWindow(HWND window)
{
    wchar_t name[128]{};
    if (!GetClassNameW(window, name, int(std::size(name))))
        return true;
    const QString windowClass = QString::fromWCharArray(name);
    return windowClass == QStringLiteral("SysShadow")
        || windowClass == QStringLiteral("#32768")
        || windowClass == QStringLiteral("ComboLBox")
        || windowClass == QStringLiteral("tooltips_class32")
        || windowClass.contains(QStringLiteral("QWindowPopup"))
        || windowClass.contains(QStringLiteral("QWindowToolTip"));
}
#endif

class TopologyWatcher final : public QObject {
public:
    explicit TopologyWatcher(QObject* parent) : QObject(parent) {
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, this, [this]() {
            NativeWindows::invalidate();
            // 回调可能创建或销毁窗口，遍历副本并验证接收对象仍然存活。
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
    static void CALLBACK changed(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD) {
        if (!active || (event != EVENT_SYSTEM_FOREGROUND && (object != OBJID_WINDOW || child != 0)))
            return;
        if (event != EVENT_SYSTEM_FOREGROUND && event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE
            && event != EVENT_OBJECT_REORDER && event != EVENT_OBJECT_LOCATIONCHANGE)
            return;
        NativeWindows::invalidate();
        if (RenderDiagnostics::enabled()) {
            wchar_t name[128]{};
            GetClassNameW(window, name, 128);
            DWORD process = 0;
            GetWindowThreadProcessId(window, &process);
            RenderDiagnostics::write(event == EVENT_SYSTEM_FOREGROUND ? "window.foreground" : "window.topology",
                QString("event=0x%1 hwnd=0x%2 sourcePid=%3 class=%4")
                    .arg(event, 0, 16).arg(reinterpret_cast<quintptr>(window), 0, 16)
                    .arg(process).arg(QString::fromWCharArray(name)), 250);
        }
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
    // 使用 EnumWindows 避免并发重排窗口时沿句柄链循环遍历。
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
        // 系统菜单阴影是装饰窗口，WGC 可能返回黑色矩形，不能把它当作独立内容层。
        if (wcscmp(name, L"SysShadow") == 0)
            return TRUE;
        const bool desktop = window == GetShellWindow() || wcscmp(name, L"Progman") == 0
                             || wcscmp(name, L"WorkerW") == 0;
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        DWORD affinity = 0;
        GetWindowDisplayAffinity(window, &affinity);
        // 工具窗口仍参与遮挡；是否能作为 WGC 源另行检查，避免误删其上方普通窗口的有效像素。
        const bool excluded = affinity != 0
                              || NativeWindows::isGlassWindow(reinterpret_cast<WId>(window))
                              || isTransientCaptureWindow(window);
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

bool NativeWindows::isCaptureCandidate(WId id)
{
#ifdef Q_OS_WIN
    const HWND window = reinterpret_cast<HWND>(id);
    if (!window || !IsWindow(window) || !IsWindowVisible(window) || IsIconic(window)
        || GetAncestor(window, GA_ROOT) != window)
        return false;
    if (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD)
        return false;
    const LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    // WGC 拒绝未带 APPWINDOW 的工具窗口，并可能在 CreateForWindow 内部抛出异常。
    if ((extendedStyle & WS_EX_TOOLWINDOW) && !(extendedStyle & WS_EX_APPWINDOW))
        return false;
    DWORD affinity = 0;
    if (GetWindowDisplayAffinity(window, &affinity) && affinity != 0)
        return false;
    // WGC 可能对短暂存在的弹出窗口抛出内部异常，创建捕获项前先排除这些句柄。
    return !isTransientCaptureWindow(window);
#else
    Q_UNUSED(id);
    return false;
#endif
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

        // 无边框应用可能没有标题栏样式；非工具顶层窗口的标题或系统菜单也可标识应用。
        if (!(style & (WS_CAPTION | WS_SYSMENU)) && !(extendedStyle & WS_EX_APPWINDOW)
            && GetWindowTextLengthW(window) == 0)
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
    // 从底到顶恢复窗口并保留原有普通或最大化状态，不抢夺焦点。
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

WId NativeWindows::desktopInsertAfter(WId id)
{
#ifdef Q_OS_WIN
    // 不能插入置顶窗口之后，否则 Windows 会将卡片重新提升至置顶层。
    HWND previousNormal = HWND_NOTOPMOST;
    HWND after = HWND_BOTTOM;
    for (const Window& entry : snapshot()) {
        if (entry.id == id || entry.topmost)
            continue;
        if (entry.desktop) { after = previousNormal; break; }
        previousNormal = reinterpret_cast<HWND>(entry.id);
    }
    return reinterpret_cast<WId>(after);
#else
    Q_UNUSED(id);
    return 0;
#endif
}

bool NativeWindows::isAtDesktopLayer(WId id, const QSet<WId>& desktopWidgets)
{
#ifdef Q_OS_WIN
    const Stack& stack = cachedStack();
    const int index = indexOf(stack, id);
    if (index < 0 || stack.at(index).topmost)
        return false;
    for (int i = 0; i < index; ++i)
        // 置顶桌面覆盖层存在时跳过无法完成的层级修复，避免无限重试。
        if (stack.at(i).desktop && !stack.at(i).topmost)
            return false;
    for (int i = index + 1; i < stack.size(); ++i) {
        const Window& below = stack.at(i);
        if (below.desktop)
            break;
        if (!desktopWidgets.contains(below.id))
            return false;
    }
#else
    Q_UNUSED(id);
    Q_UNUSED(desktopWidgets);
#endif
    return true;
}

void NativeWindows::placeDesktop(WId id)
{
#ifdef Q_OS_WIN
    const HWND window = reinterpret_cast<HWND>(id);
    constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    // 直接插入桌面层，避免先取消置顶时短暂盖住普通应用。
    const HWND anchor = reinterpret_cast<HWND>(desktopInsertAfter(id));
    if (!SetWindowPos(window, anchor, 0, 0, 0, 0, flags)) {
        const DWORD error = GetLastError();
        DWORD anchorProcess = 0;
        GetWindowThreadProcessId(anchor, &anchorProcess);
        RenderDiagnostics::write(QString("window.layer.failed/0x%1").arg(id, 0, 16),
            QString("operation=desktop error=%1 anchor=0x%2 anchorPid=%3")
                .arg(error).arg(reinterpret_cast<quintptr>(anchor), 0, 16).arg(anchorProcess), 10000);
    }
    // 其他进程可能同时提升锚点窗口，需要再次检查。
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
