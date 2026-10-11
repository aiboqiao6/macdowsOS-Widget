#include "ui/htmlwidgetview.h"
#include <QCoreApplication>
#include <QDir>
#include <QHideEvent>
#include <QFocusEvent>
#include <QLabel>
#include <QLibrary>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <atomic>
#include <windows.h>
#include <wrl.h>
#include "WebView2.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;

namespace {
struct CallbackLifetime {
    std::atomic_bool alive{true};
};

struct WebViewApartment {
    const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~WebViewApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

HRESULT initializeApartment() {
    // 异步创建回调与原生子窗口可能比单个视图活得更久，STA 保留到 GUI 线程退出。
    static thread_local WebViewApartment apartment;
    return apartment.result;
}
}

struct HtmlWidgetView::Impl {
    std::shared_ptr<CallbackLifetime> lifetime = std::make_shared<CallbackLifetime>();
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> web;
    QLabel* message = nullptr;
    QString document;
    QString expectedDataUri;
    QString error;
    qreal scale = 1;
    bool started = false;
    bool loaded = false;
    UINT64 navigationId = 0;
};

HtmlWidgetView::HtmlWidgetView(QWidget* parent) : QWidget(parent), m_impl(std::make_unique<Impl>()) {
    setAttribute(Qt::WA_NativeWindow);
    setFocusPolicy(Qt::StrongFocus);
    auto* layout = new QVBoxLayout(this);
    m_impl->message = new QLabel(QStringLiteral("正在加载 HTML 引擎…"), this);
    m_impl->message->setWordWrap(true);
    m_impl->message->setAlignment(Qt::AlignCenter);
    m_impl->message->setStyleSheet("color:#9fb3ce;background:#18243b;padding:16px;border-radius:12px");
    layout->addWidget(m_impl->message);
}
HtmlWidgetView::~HtmlWidgetView() {
    // QWidget 销毁原生窗口时仍会分派 COM 回调，必须先于 Impl 和 QObject 失效。
    m_impl->lifetime->alive.store(false, std::memory_order_release);
    statusChanged = {};
    if (m_impl->controller) m_impl->controller->Close();
    m_impl->web.Reset();
    m_impl->controller.Reset();
    m_impl->environment.Reset();
}
void HtmlWidgetView::fail(const QString& message) {
    m_impl->error = message;
    m_impl->message->setText(message);
    m_impl->message->show();
    const auto notify = statusChanged;
    if (notify) notify(message);
}
void HtmlWidgetView::initialize() {
    if (m_impl->started) return;
    m_impl->started = true;
    const HRESULT com = initializeApartment();
    if (FAILED(com)) { fail(QStringLiteral("HTML 引擎初始化失败（COM）。")); return; }
    static QLibrary loader(QDir(QCoreApplication::applicationDirPath()).filePath("WebView2Loader.dll"));
    loader.setLoadHints(loader.loadHints() | QLibrary::PreventUnloadHint);
    using CreateEnvironment = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    const auto create = reinterpret_cast<CreateEnvironment>(loader.resolve("CreateCoreWebView2EnvironmentWithOptions"));
    if (!create) { fail(QStringLiteral("缺少 WebView2Loader.dll，请使用完整发布目录。")); return; }
    const QString profile = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("WebView2");
    QDir().mkpath(profile);
    auto* view = this;
    const auto lifetime = m_impl->lifetime;
    const HRESULT started = create(nullptr, reinterpret_cast<LPCWSTR>(profile.utf16()), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([view, lifetime](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            if (!lifetime->alive.load(std::memory_order_acquire)) return S_OK;
            if (FAILED(result) || !environment) {
                view->fail(QStringLiteral("无法启动 HTML 引擎，请安装 Microsoft Edge WebView2 Runtime。错误：0x%1").arg(quint32(result), 8, 16, QLatin1Char('0')));
                return S_OK;
            }
            view->m_impl->environment = environment;
            const HRESULT created = environment->CreateCoreWebView2Controller(reinterpret_cast<HWND>(view->winId()),
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([view, lifetime](HRESULT status, ICoreWebView2Controller* controller) -> HRESULT {
                    if (!lifetime->alive.load(std::memory_order_acquire)) { if (controller) controller->Close(); return S_OK; }
                    if (FAILED(status) || !controller) { view->fail(QStringLiteral("无法创建 HTML 视图。")); return S_OK; }
                    auto& impl = *view->m_impl;
                    impl.controller = controller;
                    controller->get_CoreWebView2(&impl.web);
                    if (!impl.web) { view->fail(QStringLiteral("HTML 引擎不可用。")); return S_OK; }
                    ComPtr<ICoreWebView2Controller2> transparent;
                    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&transparent))))
                        transparent->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{0, 0, 0, 0});
                    ComPtr<ICoreWebView2Settings> settings;
                    impl.web->get_Settings(&settings);
                    if (settings) {
                        settings->put_IsWebMessageEnabled(FALSE);
                        settings->put_AreDevToolsEnabled(FALSE);
                        settings->put_AreDefaultContextMenusEnabled(FALSE);
                        settings->put_IsStatusBarEnabled(FALSE);
                        settings->put_IsZoomControlEnabled(FALSE);
                        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                        ComPtr<ICoreWebView2Settings3> extra;
                        if (SUCCEEDED(settings.As(&extra))) extra->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                    }
                    EventRegistrationToken token{};
                    impl.web->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                        [view, lifetime](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                            if (!lifetime->alive.load(std::memory_order_acquire)) { args->put_Cancel(TRUE); return S_OK; }
                            LPWSTR uri = nullptr;
                            args->get_Uri(&uri);
                            const QString address = QString::fromWCharArray(uri ? uri : L"");
                            // 内存页面可能被转换成数据网址，只放行宿主生成的精确页面。
                            if (address != "about:blank" && address != view->m_impl->expectedDataUri) args->put_Cancel(TRUE);
                            else args->get_NavigationId(&view->m_impl->navigationId);
                            CoTaskMemFree(uri);
                            return S_OK;
                        }).Get(), &token);
                    impl.web->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT { args->put_Handled(TRUE); return S_OK; }).Get(), &token);
                    impl.web->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT { args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY); return S_OK; }).Get(), &token);
                    ComPtr<ICoreWebView2_4> downloads;
                    if (SUCCEEDED(impl.web.As(&downloads)))
                        downloads->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>(
                            [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT { args->put_Cancel(TRUE); return S_OK; }).Get(), &token);
                    impl.web->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [view, lifetime](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                            if (!lifetime->alive.load(std::memory_order_acquire)) return S_OK;
                            UINT64 navigationId = 0; args->get_NavigationId(&navigationId);
                            if (navigationId != view->m_impl->navigationId) return S_OK;
                            BOOL success = FALSE; args->get_IsSuccess(&success);
                            if (success) {
                                view->m_impl->loaded = true;
                                view->m_impl->error.clear();
                                view->m_impl->message->hide();
                                const auto notify = view->statusChanged;
                                if (notify) notify(QStringLiteral("预览已更新"));
                            } else {
                                COREWEBVIEW2_WEB_ERROR_STATUS error{}; args->get_WebErrorStatus(&error);
                                if (error == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) return S_OK;
                                view->m_impl->loaded = false;
                                view->fail(QStringLiteral("HTML 加载失败（%1），请检查内容后重试。").arg(int(error)));
                            }
                            return S_OK;
                        }).Get(), &token);
                    impl.web->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
                        [view, lifetime](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
                            if (lifetime->alive.load(std::memory_order_acquire)) view->fail(QStringLiteral("HTML 渲染进程已停止，请关闭并重新打开此窗口。")); return S_OK;
                        }).Get(), &token);
                    view->updateBounds();
                    controller->put_ZoomFactor(impl.scale);
                    controller->put_IsVisible(view->isVisible());
                    view->setDocument(impl.document);
                    return S_OK;
                }).Get());
            if (FAILED(created) && lifetime->alive.load(std::memory_order_acquire)) view->fail(QStringLiteral("HTML 控制器创建失败。"));
            return S_OK;
        }).Get());
    if (FAILED(started) && lifetime->alive.load(std::memory_order_acquire)) fail(QStringLiteral("无法启动 WebView2，请检查 Runtime 是否已安装。"));
}
void HtmlWidgetView::setDocument(const QString& document) {
    m_impl->document = document;
    m_impl->expectedDataUri = QStringLiteral("data:text/html;charset=utf-8;base64,") + QString::fromLatin1(document.toUtf8().toBase64());
    m_impl->loaded = false;
    const auto lifetime = m_impl->lifetime;
    const auto web = m_impl->web;
    const QString source = document;
    if (web && !source.isEmpty()) {
        const HRESULT result = web->NavigateToString(reinterpret_cast<LPCWSTR>(source.utf16()));
        if (FAILED(result) && lifetime->alive.load(std::memory_order_acquire)) fail(QStringLiteral("HTML 内容过大或加载失败。"));
    }
}
void HtmlWidgetView::setContentScale(qreal scale) {
    m_impl->scale = qBound<qreal>(.25, scale, 5);
    if (m_impl->controller) m_impl->controller->put_ZoomFactor(m_impl->scale);
}
bool HtmlWidgetView::isReady() const { return bool(m_impl->web); }
bool HtmlWidgetView::hasLoaded() const { return m_impl->loaded; }
QString HtmlWidgetView::errorText() const { return m_impl->error; }
void HtmlWidgetView::evaluateScript(const QString& script, std::function<void(QString)> callback) {
    if (!callback) return;
    const auto lifetime = m_impl->lifetime;
    const auto completion = std::make_shared<std::function<void(QString)>>(std::move(callback));
    auto finish = [lifetime, completion](QString result) {
        if (!lifetime->alive.load(std::memory_order_acquire) || !*completion) return;
        auto callback = std::move(*completion);
        callback(std::move(result));
    };
    const auto web = m_impl->web;
    if (!web) { finish({}); return; }
    const HRESULT started = web->ExecuteScript(reinterpret_cast<LPCWSTR>(script.utf16()),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>([finish](HRESULT status, LPCWSTR result) -> HRESULT {
            finish(SUCCEEDED(status) && result ? QString::fromWCharArray(result) : QString()); return S_OK;
        }).Get());
    if (FAILED(started)) finish({});
}
void HtmlWidgetView::updateBounds() {
    if (!m_impl->controller) return;
    const HWND viewWindow = reinterpret_cast<HWND>(winId());
    HWND controllerParent = nullptr;
    // 工作室从独立窗口转为设置子窗口时，Qt 可能重建原生窗口句柄。
    if (SUCCEEDED(m_impl->controller->get_ParentWindow(&controllerParent)) && controllerParent != viewWindow)
        m_impl->controller->put_ParentWindow(viewWindow);
    RECT bounds{};
    GetClientRect(viewWindow, &bounds);
    m_impl->controller->put_Bounds(bounds);
    m_impl->controller->NotifyParentWindowPositionChanged();
}
void HtmlWidgetView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event); initialize();
    if (m_impl->controller) {
        ComPtr<ICoreWebView2_3> suspendable;
        if (m_impl->web && SUCCEEDED(m_impl->web.As(&suspendable))) suspendable->Resume();
        updateBounds(); m_impl->controller->put_IsVisible(TRUE);
    }
}
void HtmlWidgetView::hideEvent(QHideEvent* event) {
    if (m_impl->controller) m_impl->controller->put_IsVisible(FALSE);
    ComPtr<ICoreWebView2_3> suspendable;
    if (m_impl->web && SUCCEEDED(m_impl->web.As(&suspendable)))
        suspendable->TrySuspend(Callback<ICoreWebView2TrySuspendCompletedHandler>([](HRESULT, BOOL) -> HRESULT { return S_OK; }).Get());
    QWidget::hideEvent(event);
}
void HtmlWidgetView::resizeEvent(QResizeEvent* event) { QWidget::resizeEvent(event); updateBounds(); }
void HtmlWidgetView::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
    if (m_impl->controller) m_impl->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}
