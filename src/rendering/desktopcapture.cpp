#include "desktopcapture.h"
#include "nativewindows.h"
#include "renderdiagnostics.h"
#include <QPainter>
#include <cstring>

#include <QGuiApplication>
#include <QCache>
#include <QHash>
#include <QPointer>
#include <QRunnable>
#include <QScreen>
#include <QThreadPool>
#include <QTimer>
#include <QVector>
#include <QtMath>
#include <chrono>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <mutex>

#ifdef Q_OS_WIN
#include <windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowsapp.lib")
#endif

namespace {
void logCaptureFailure(const char* stage, quintptr target = 0) {
    QString details = QString("target=0x%1").arg(target, 0, 16);
#ifdef Q_OS_WIN
    // 在异常处理块中记录捕获失败的 HRESULT，便于排查回退至壁纸的原因。
    details += QString(" HRESULT=0x%1").arg(quint32(winrt::to_hresult()), 8, 16, QLatin1Char('0'));
#endif
    RenderDiagnostics::write(QString("capture.failure/%1").arg(stage), details, 1000);
}
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
    // 捕获线程复用 DIB 与内存 DC，只向 Qt 复制所需的缩小图像。
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
    // 保留数量受限的 DIB 缓存，避免不同尺寸的卡片轮流捕获时反复分配。
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
    QRect targetBounds;
    quint64 topology = 1469598103934665603ull;
};

QRect projectToCapture(const QRect& nativeRect, const CapturePart& part)
{
    const QRect overlap = nativeRect.intersected(part.source);
    if (overlap.isEmpty())
        return {};
    const qreal sx = part.destination.width() / qreal(part.source.width());
    const qreal sy = part.destination.height() / qreal(part.source.height());
    const int left = qFloor((overlap.left() - part.source.left()) * sx);
    const int top = qFloor((overlap.top() - part.source.top()) * sy);
    const int right = qCeil((overlap.right() + 1 - part.source.left()) * sx);
    const int bottom = qCeil((overlap.bottom() + 1 - part.source.top()) * sy);
    return QRect(part.destination.topLeft() + QPoint(left, top),
                 QSize(right - left, bottom - top));
}

struct TargetReconstruction {
    QRegion target;
    QRegion valid;
};

#ifdef Q_OS_WIN
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
namespace wdx11 = winrt::Windows::Graphics::DirectX::Direct3D11;
using Microsoft::WRL::ComPtr;

bool graphicsCaptureReady()
{
    struct Apartment {
        bool ready = false;
        bool owned = false;
        Apartment() {
            // 界面线程可能已经初始化为 STA；直接检查 HRESULT，避免用异常处理正常的单元模式差异。
            const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            owned = SUCCEEDED(result); // 返回 S_FALSE 也必须配对调用 CoUninitialize。
            ready = owned || result == RPC_E_CHANGED_MODE;
        }
        ~Apartment() { if (owned) CoUninitialize(); }
    };
    static thread_local Apartment apartment;
    static thread_local const bool supported = [] {
        try { return apartment.ready && wgc::GraphicsCaptureSession::IsSupported(); }
        catch (...) { return false; }
    }();
    return supported;
}

// 每个下层窗口复用一个 WGC 捕获流，读取最新完成帧，避免反复创建设备和会话。
class WindowCaptureStream final {
    struct CapturedFrame {
        explicit CapturedFrame(wgc::Direct3D11CaptureFrame value) : frame(std::move(value)) {}
        ~CapturedFrame() { try { if (frame) frame.Close(); } catch (...) {} }
        wgc::Direct3D11CaptureFrame frame{nullptr};
    };
    // 撤销事件后仍可能收到已派发回调；委托只持有邮箱，不持有捕获流或 D3D 资源。
    struct FrameState {
        std::mutex mutex;
        std::condition_variable arrived;
        bool stopping = false;
        bool recreating = false;
        winrt::Windows::Graphics::SizeInt32 contentSize{};
        std::shared_ptr<CapturedFrame> latest;
    };
public:
    explicit WindowCaptureStream(HWND window) : m_window(window)
    {
        try { initialize(); }
        catch (...) { logCaptureFailure("initialize", reinterpret_cast<quintptr>(window)); close(); }
    }

    ~WindowCaptureStream()
    {
        close();
    }

    void close() noexcept
    {
        m_valid = false;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->stopping = true;
            m_state->latest.reset();
        }
        m_state->arrived.notify_all();
        // 设备丢失时仍要分别尝试释放每项资源。
        try { if (m_item && m_closedToken.value) m_item.Closed(m_closedToken); } catch (...) {}
        try { if (m_pool && m_token.value) m_pool.FrameArrived(m_token); } catch (...) {}
        try { if (m_session) m_session.Close(); } catch (...) {}
        try { if (m_pool) m_pool.Close(); } catch (...) {}
        m_closedToken = {};
        m_token = {};
    }

    bool valid() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_valid && !m_state->stopping && validWindow();
    }

    std::chrono::steady_clock::time_point lastUsed() const { return m_lastUsed; }

    struct Image {
        QImage pixels;
        QRect desktopBounds;
    };

    Image latestImage(const QRect& windowBounds, const QRect& requestedBounds,
                      const QSize& outputSize)
    {
        m_lastUsed = std::chrono::steady_clock::now();
        if (!valid())
            return {};
        try {
            ensureSize();
            std::shared_ptr<CapturedFrame> frame;
            {
                std::unique_lock<std::mutex> lock(m_state->mutex);
                if (!m_state->latest) {
                    // 仅首次读取等待 WGC 启动，后续请求直接使用最新到达帧。
                    m_state->arrived.wait_for(lock, std::chrono::milliseconds(50),
                        [this]() { return bool(m_state->latest) || m_state->stopping; });
                }
                if (m_state->stopping || !m_state->latest)
                    return {};
                frame = m_state->latest;
            }
            const qint64 timestamp = frame->frame.SystemRelativeTime().count();
            if (!m_lastImage.pixels.isNull() && m_lastTimestamp == timestamp
                && m_lastWindowBounds == windowBounds && m_lastRequestedBounds == requestedBounds
                && m_lastOutputSize == outputSize)
                return m_lastImage;
            Image image = copyFrame(frame->frame, windowBounds, requestedBounds, outputSize);
            if (!image.pixels.isNull()) {
                m_lastTimestamp = timestamp;
                m_lastWindowBounds = windowBounds;
                m_lastRequestedBounds = requestedBounds;
                m_lastOutputSize = outputSize;
                m_lastImage = image;
            }
            return image;
        } catch (...) {
            logCaptureFailure("copy-frame", reinterpret_cast<quintptr>(m_window));
            m_valid = false;
            return {};
        }
    }

private:
    void initialize()
    {
        if (!graphicsCaptureReady() || !validWindow())
            return;

        D3D_FEATURE_LEVEL featureLevel{};
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        winrt::check_hresult(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &m_d3dDevice,
            &featureLevel, &m_d3dContext));
        ComPtr<IDXGIDevice> dxgiDevice;
        winrt::check_hresult(m_d3dDevice.As(&dxgiDevice));
        if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                        m_d2dFactory.GetAddressOf()))
            && SUCCEEDED(m_d2dFactory->CreateDevice(dxgiDevice.Get(), &m_d2dDevice))) {
            m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                              &m_d2dContext);
        }
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
            dxgiDevice.Get(), inspectable.put()));
        m_device = inspectable.as<wdx11::IDirect3DDevice>();

        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem,
                                                      IGraphicsCaptureItemInterop>();
        // 设备初始化期间弹出窗口可能已消失，创建捕获项前再次验证句柄。
        if (!validWindow())
            return;
        const HRESULT itemResult = interop->CreateForWindow(
            m_window, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(m_item));
        if (FAILED(itemResult)) {
            m_item = nullptr;
            wchar_t windowClass[128]{};
            GetClassNameW(m_window, windowClass, int(std::size(windowClass)));
            qWarning("Windows Graphics Capture cannot create an item for HWND %p class=%ls (HRESULT 0x%08lx)",
                     static_cast<void*>(m_window), windowClass, static_cast<unsigned long>(itemResult));
            return;
        }
        m_size = m_item.Size();
        if (m_size.Width <= 0 || m_size.Height <= 0)
            return;
        m_pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            m_device, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, m_size);
        m_session = m_pool.CreateCaptureSession(m_item);
        m_state->contentSize = m_size;
        m_closedToken = m_item.Closed([state = m_state](auto const&, auto const&) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->stopping = true;
                state->latest.reset();
            }
            state->arrived.notify_all();
        });
        try { m_session.IsCursorCaptureEnabled(false); } catch (...) {}
        try { m_session.IsBorderRequired(false); } catch (...) {}
        // 支持时关闭系统捕获节流，以适配高刷新率显示器。
        if (auto cadence = m_session.try_as<wgc::IGraphicsCaptureSession5>())
            cadence.MinUpdateInterval(std::chrono::milliseconds(0));
        m_token = m_pool.FrameArrived([state = m_state](auto const& sender, auto const&) {
            try {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->stopping || state->recreating)
                    return;
                // 排空队列，只保留最新帧，避免繁忙窗口的画面持续滞后。
                while (auto frame = sender.TryGetNextFrame()) {
                    state->contentSize = frame.ContentSize();
                    state->latest = std::make_shared<CapturedFrame>(std::move(frame));
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->stopping = true;
                state->arrived.notify_all();
                return;
            }
            state->arrived.notify_all();
        });
        m_session.StartCapture();
        m_valid = true;
    }

    bool validWindow() const
    {
        return NativeWindows::isCaptureCandidate(reinterpret_cast<WId>(m_window));
    }

    void ensureSize()
    {
        winrt::Windows::Graphics::SizeInt32 size{};
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            size = m_state->contentSize;
            if (size.Width <= 0 || size.Height <= 0
                || (size.Width == m_size.Width && size.Height == m_size.Height))
                return;
            m_state->recreating = true;
            m_state->latest.reset();
        }
        m_staging.Reset();
        // 重建帧池可能等待帧到达，此处不能持有邮箱锁。
        m_pool.Recreate(m_device, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                        3, size);
        m_size = size;
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->recreating = false;
    }

    Image copyFrame(const wgc::Direct3D11CaptureFrame& frame,
                    const QRect& windowBounds, const QRect& requestedBounds,
                    const QSize& outputSize)
    {
        const auto contentSize = frame.ContentSize();
        auto access = frame.Surface().as<
            ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> texture;
        winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), &texture));
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        int effectiveWidth = qMin(int(description.Width), contentSize.Width);
        int effectiveHeight = qMin(int(description.Height), contentSize.Height);
        if (effectiveWidth <= 0 || effectiveHeight <= 0 || windowBounds.isEmpty())
            return {};
        const DPI_AWARENESS awareness = GetAwarenessFromDpiAwarenessContext(
            GetWindowDpiAwarenessContext(m_window));
        const UINT dpi = GetDpiForWindow(m_window);
        if (awareness != DPI_AWARENESS_PER_MONITOR_AWARE && dpi > 96) {
            effectiveWidth = qMax(1, qRound(effectiveWidth * 96.0 / dpi));
            effectiveHeight = qMax(1, qRound(effectiveHeight * 96.0 / dpi));
        }

        const QRect desktopCrop = requestedBounds.intersected(windowBounds);
        if (desktopCrop.isEmpty())
            return {};
        const qreal sx = effectiveWidth / qreal(windowBounds.width());
        const qreal sy = effectiveHeight / qreal(windowBounds.height());
        const int left = qBound(0, qFloor((desktopCrop.left() - windowBounds.left()) * sx),
                                effectiveWidth - 1);
        const int top = qBound(0, qFloor((desktopCrop.top() - windowBounds.top()) * sy),
                               effectiveHeight - 1);
        const int right = qBound(left + 1,
            qCeil((desktopCrop.right() + 1 - windowBounds.left()) * sx), effectiveWidth);
        const int bottom = qBound(top + 1,
            qCeil((desktopCrop.bottom() + 1 - windowBounds.top()) * sy), effectiveHeight);
        const UINT cropWidth = UINT(right - left);
        const UINT cropHeight = UINT(bottom - top);
        UINT readWidth = cropWidth;
        UINT readHeight = cropHeight;
        bool scaledOnGpu = false;
        if (outputSize.isValid()
            && outputSize != QSize(int(cropWidth), int(cropHeight))
            && m_d2dContext) {
            D3D11_TEXTURE2D_DESC scaledDescription = description;
            scaledDescription.Width = UINT(outputSize.width());
            scaledDescription.Height = UINT(outputSize.height());
            scaledDescription.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            scaledDescription.CPUAccessFlags = 0;
            scaledDescription.Usage = D3D11_USAGE_DEFAULT;
            scaledDescription.MiscFlags = 0;
            scaledDescription.ArraySize = 1;
            scaledDescription.MipLevels = 1;
            if (!m_scaled || m_scaledWidth != scaledDescription.Width
                || m_scaledHeight != scaledDescription.Height) {
                m_scaled.Reset();
                if (SUCCEEDED(m_d3dDevice->CreateTexture2D(
                        &scaledDescription, nullptr, &m_scaled))) {
                    m_scaledWidth = scaledDescription.Width;
                    m_scaledHeight = scaledDescription.Height;
                }
            }
            ComPtr<IDXGISurface> sourceSurface;
            ComPtr<IDXGISurface> targetSurface;
            ComPtr<ID2D1Bitmap1> sourceBitmap;
            ComPtr<ID2D1Bitmap1> targetBitmap;
            const D2D1_PIXEL_FORMAT pixelFormat = D2D1::PixelFormat(
                description.Format, D2D1_ALPHA_MODE_IGNORE);
            if (m_scaled
                && SUCCEEDED(texture.As(&sourceSurface))
                && SUCCEEDED(m_scaled.As(&targetSurface))
                && SUCCEEDED(m_d2dContext->CreateBitmapFromDxgiSurface(
                    sourceSurface.Get(),
                    D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, pixelFormat),
                    &sourceBitmap))
                && SUCCEEDED(m_d2dContext->CreateBitmapFromDxgiSurface(
                    targetSurface.Get(),
                    D2D1::BitmapProperties1(
                        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                        pixelFormat), &targetBitmap))) {
                m_d2dContext->SetTarget(targetBitmap.Get());
                m_d2dContext->BeginDraw();
                m_d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
                const D2D1_RECT_F destinationRect = D2D1::RectF(
                    0, 0, FLOAT(outputSize.width()), FLOAT(outputSize.height()));
                const D2D1_RECT_F sourceRect = D2D1::RectF(
                    FLOAT(left), FLOAT(top), FLOAT(right), FLOAT(bottom));
                m_d2dContext->DrawBitmap(
                    sourceBitmap.Get(), &destinationRect, 1.0f,
                    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &sourceRect, nullptr);
                const HRESULT drawResult = m_d2dContext->EndDraw();
                m_d2dContext->SetTarget(nullptr);
                if (SUCCEEDED(drawResult)) {
                    scaledOnGpu = true;
                    readWidth = UINT(outputSize.width());
                    readHeight = UINT(outputSize.height());
                }
            }
        }
        if (!m_staging || readWidth != m_stagingWidth || readHeight != m_stagingHeight) {
            D3D11_TEXTURE2D_DESC stagingDescription = description;
            stagingDescription.Width = readWidth;
            stagingDescription.Height = readHeight;
            stagingDescription.BindFlags = 0;
            stagingDescription.MiscFlags = 0;
            stagingDescription.Usage = D3D11_USAGE_STAGING;
            stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stagingDescription.ArraySize = 1;
            stagingDescription.MipLevels = 1;
            m_staging.Reset();
            winrt::check_hresult(m_d3dDevice->CreateTexture2D(
                &stagingDescription, nullptr, &m_staging));
            m_stagingWidth = readWidth;
            m_stagingHeight = readHeight;
        }
        if (scaledOnGpu) {
            m_d3dContext->CopyResource(m_staging.Get(), m_scaled.Get());
        } else {
            const D3D11_BOX sourceBox{UINT(left), UINT(top), 0,
                                      UINT(right), UINT(bottom), 1};
            m_d3dContext->CopySubresourceRegion(
                m_staging.Get(), 0, 0, 0, 0, texture.Get(), 0, &sourceBox);
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(m_d3dContext->Map(
            m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        const QImage mappedImage(static_cast<const uchar*>(mapped.pData), int(readWidth),
                                 int(readHeight), qsizetype(mapped.RowPitch),
                                 QImage::Format_ARGB32);
        QImage result;
        if (outputSize.isValid() && outputSize != mappedImage.size())
            result = mappedImage.scaled(outputSize, Qt::IgnoreAspectRatio,
                                        Qt::SmoothTransformation);
        else
            result = mappedImage.copy();
        m_d3dContext->Unmap(m_staging.Get(), 0);
        // Windows 上 BGRA8 与 QImage::Format_ARGB32 内存布局一致，可直接返回以减少复制。
        return {std::move(result), desktopCrop};
    }

    HWND m_window = nullptr;
    qint64 m_lastTimestamp = -1;
    QRect m_lastWindowBounds;
    QRect m_lastRequestedBounds;
    QSize m_lastOutputSize;
    Image m_lastImage;
    std::chrono::steady_clock::time_point m_lastUsed = std::chrono::steady_clock::now();
    std::atomic_bool m_valid = false;
    std::shared_ptr<FrameState> m_state = std::make_shared<FrameState>();
    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;
    ComPtr<ID3D11Texture2D> m_staging;
    ComPtr<ID3D11Texture2D> m_scaled;
    UINT m_stagingWidth = 0;
    UINT m_stagingHeight = 0;
    UINT m_scaledWidth = 0;
    UINT m_scaledHeight = 0;
    ComPtr<ID2D1Factory1> m_d2dFactory;
    ComPtr<ID2D1Device> m_d2dDevice;
    ComPtr<ID2D1DeviceContext> m_d2dContext;
    wdx11::IDirect3DDevice m_device{nullptr};
    wgc::GraphicsCaptureItem m_item{nullptr};
    wgc::Direct3D11CaptureFramePool m_pool{nullptr};
    wgc::GraphicsCaptureSession m_session{nullptr};
    winrt::event_token m_token{};
    winrt::event_token m_closedToken{};
    winrt::Windows::Graphics::SizeInt32 m_size{};
};

using CapturedWindowImage = WindowCaptureStream::Image;
using WindowCaptureStreams = QHash<WId, std::shared_ptr<WindowCaptureStream>>;

QCache<WId, std::chrono::steady_clock::time_point>& failedWindowCaptures()
{
    static thread_local QCache<WId, std::chrono::steady_clock::time_point> failures(64);
    return failures;
}

WindowCaptureStreams& windowCaptureStreams()
{
    // 先构造 COM 单元，确保线程局部捕获流在 COM 反初始化前销毁。
    graphicsCaptureReady();
    static thread_local WindowCaptureStreams streams;
    return streams;
}

void clearWindowCaptureStreams()
{
    windowCaptureStreams().clear();
    failedWindowCaptures().clear();
}

void pruneWindowCaptureStreams()
{
    auto& streams = windowCaptureStreams();
    const auto now = std::chrono::steady_clock::now();
    for (auto it = streams.begin(); it != streams.end(); ) {
        if (!it.value() || !it.value()->valid()
            || now - it.value()->lastUsed() > std::chrono::seconds(2))
            it = streams.erase(it);
        else
            ++it;
    }
}

CapturedWindowImage captureWindowGraphics(HWND window, const QRect& windowBounds,
                                          const QRect& requestedBounds,
                                          const QSize& outputSize)
{
    try {
        if (!NativeWindows::isCaptureCandidate(reinterpret_cast<WId>(window)) || !graphicsCaptureReady())
            return {};
        auto& streams = windowCaptureStreams();
        const WId id = reinterpret_cast<WId>(window);
        auto& failures = failedWindowCaptures();
        const auto now = std::chrono::steady_clock::now();
        if (const auto* retryAfter = failures.object(id)) {
            if (now < *retryAfter)
                return {};
            failures.remove(id);
        }
        auto stream = streams.value(id);
        if (!stream) {
            if (streams.size() >= 8) {
                auto oldest = streams.begin();
                for (auto it = streams.begin(); it != streams.end(); ++it)
                    if (it.value()->lastUsed() < oldest.value()->lastUsed())
                        oldest = it;
                streams.erase(oldest);
            }
            stream = std::make_shared<WindowCaptureStream>(window);
            if (!stream->valid()) {
                // 短暂缓存失败窗口，避免每秒重复创建不支持或正在关闭的捕获源。
                failures.insert(id, new std::chrono::steady_clock::time_point(now + std::chrono::seconds(1)));
                return {};
            }
            streams.insert(id, stream);
        }
        return stream->latestImage(windowBounds, requestedBounds, outputSize);
    } catch (...) {
        return {};
    }
}

#endif

TargetReconstruction reconstructTarget(QImage& image, const QVector<CapturePart>& parts,
                                       WId target, const NativeWindows::Stack& stack,
                                       bool fullArea = false)
{
    TargetReconstruction result;
    const int targetIndex = target ? NativeWindows::indexOf(stack, target) : -1;
    if (targetIndex < 0 || image.isNull())
        return result;
    QRect targetBounds = stack.at(targetIndex).bounds;
    if (fullArea) {
        // Moving settings windows crop at delivery time. Rebuild the entire requested
        // desktop area from windows below the target, including the overscan margin.
        targetBounds = {};
        for (const auto& part : parts) targetBounds |= part.source;
    }
    for (const CapturePart& part : parts)
        result.target += projectToCapture(targetBounds, part);
#ifdef Q_OS_WIN
    QRegion remaining = result.target;
    // 按窗口从上到下的次序选取最近的不透明像素；壁纸与玻璃窗口由调用者另行合成。
    for (int i = targetIndex + 1; i < stack.size() && !remaining.isEmpty(); ++i) {
        const NativeWindows::Window& lower = stack.at(i);
        QRegion projected;
        for (const CapturePart& part : parts)
            projected += projectToCapture(lower.bounds, part);
        if (projected.isEmpty())
            continue;
        if (lower.excluded)
            continue;
        const QRegion paintRegion = projected & remaining;
        if (paintRegion.isEmpty())
            continue;
        // 分层窗口也可能包含正常的不透明内容，不能仅凭 WS_EX_LAYERED 排除捕获源。
        QRegion captureProjection;
        int intersectingParts = 0;
        const QRect desktopCaptureBounds = lower.bounds.intersected(targetBounds);
        for (const CapturePart& part : parts) {
            const QRect projection = projectToCapture(desktopCaptureBounds, part);
            if (!projection.isEmpty()) {
                captureProjection += projection;
                ++intersectingParts;
            }
        }
        const QSize captureOutputSize = intersectingParts == 1
            ? captureProjection.boundingRect().size() : QSize();
        CapturedWindowImage captured = captureWindowGraphics(
            reinterpret_cast<HWND>(lower.id), lower.bounds,
            desktopCaptureBounds, captureOutputSize);
        // 纯黑和纯白都是有效画面，不能据此判定 WGC 失败并替换为旧帧。
        if (captured.pixels.isNull()) {
            // 先尝试两个桌面宿主，均不能捕获时才回退至壁纸文件，以兼容动态壁纸。
            if (!lower.desktop)
                remaining -= paintRegion;
            continue;
        }
        QPainter painter(&image);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (const CapturePart& part : parts) {
            const QRect source = lower.bounds.intersected(targetBounds).intersected(part.source);
            if (source.isEmpty())
                continue;
            const QRect destination = projectToCapture(source, part);
            painter.setClipRegion(paintRegion & destination);
            const qreal wx = captured.pixels.width() / qreal(captured.desktopBounds.width());
            const qreal wy = captured.pixels.height() / qreal(captured.desktopBounds.height());
            const QRect windowSource(
                qRound((source.left() - captured.desktopBounds.left()) * wx),
                qRound((source.top() - captured.desktopBounds.top()) * wy),
                qMax(1, qRound(source.width() * wx)),
                qMax(1, qRound(source.height() * wy)));
            const QRect clippedSource = windowSource.intersected(captured.pixels.rect());
            if (!clippedSource.isEmpty())
                painter.drawImage(destination, captured.pixels, clippedSource);
        }
        painter.end();
        result.valid |= paintRegion;
        remaining -= paintRegion;
    }
#else
    Q_UNUSED(parts);
#endif
    return result;
}

Plan capturePlan(const QVector<CapturePart>& parts, const QSize& size, WId target,
                 const NativeWindows::Stack& stack)
{
    const int targetIndex = target ? NativeWindows::indexOf(stack, target) : -1;
    if (target && targetIndex < 0)
        return {{}, 0};
    Plan plan{QRegion(QRect(QPoint(), size))};
    if (targetIndex >= 0)
        plan.targetBounds = stack.at(targetIndex).bounds;
    const auto hash = [&plan](quint64 value) { plan.scene = (plan.scene ^ value) * 1099511628211ull; };
    for (int i = 0; i < stack.size(); ++i) {
        const auto& window = stack.at(i);
        if (window.id == target)
            continue;
        // 先从 GDI 捕获图中剔除本进程的玻璃窗口，再单独合成下层玻璃，避免重复叠加产生反馈。
        if (window.excluded) {
            for (const auto& part : parts)
                plan.valid -= projectToCapture(window.bounds, part);
            continue;
        }
        bool intersects = false;
        for (const auto& part : parts) {
            // 阴影和圆角可能超出窗口矩形，遮挡范围需保守扩展。
            const QRect bounds = window.bounds.adjusted(-16, -16, 16, 16);
            if (!bounds.intersects(part.source))
                continue;
            intersects = true;
            // 透明桌面覆盖层不能按整块矩形视为不透明遮挡，否则会误删真正的下层窗口像素。
            if (i < targetIndex && window.opaque)
                plan.valid -= projectToCapture(bounds, part);
        }
        if (i > targetIndex && intersects) {
            // 窗口移动只改变位置，不改变捕获源身份；只跟踪与当前裁剪区域相关的窗口。
            plan.topology = (plan.topology ^ quint64(window.id)) * 1099511628211ull;
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
                                const QRect& area, qreal scale, WId target, const Plan& queued,
                                bool validateOnReturn = true, bool targetMayMove = false)
{
#ifdef Q_OS_WIN
    pruneWindowCaptureStreams();
#endif
    const NativeWindows::Stack beforeStack = NativeWindows::snapshot();
    const Plan before = capturePlan(parts, size, target, beforeStack);
    if (!queued.scene || (target ? queued.topology != before.topology : queued.scene != before.scene)
        || (targetMayMove ? queued.targetBounds.size() != before.targetBounds.size()
                          : queued.targetBounds != before.targetBounds))
        return {};
    if ((queued.valid & before.valid).isEmpty()) {
        QImage blank(size, QImage::Format_RGB32);
        blank.fill(Qt::black);
        return {std::move(blank), {}, area, before.scene, target, scale, before.targetBounds,
                false, target ? before.topology : 0, targetMayMove};
    }
    const bool targeted = target && NativeWindows::indexOf(beforeStack, target) >= 0;
    QImage image;
    if (targeted) {
        // 只重建目标矩形内可验证的下层窗口像素，剩余区域由稳定壁纸缓存补齐。
        image = QImage(size, QImage::Format_RGB32);
        image.fill(Qt::black);
    } else {
        image = grabParts(parts, size);
    }
    const TargetReconstruction reconstructed = reconstructTarget(
        image, parts, target, beforeStack, targetMayMove);
    // 异步结果在界面线程交付前验证窗口层级，避免重复枚举。
    const Plan after = validateOnReturn ? capturePlan(parts, size, target) : before;
    if ((targeted ? before.topology != after.topology : before.scene != after.scene)
        || (targetMayMove ? before.targetBounds.size() != after.targetBounds.size()
                          : before.targetBounds != after.targetBounds) || image.isNull())
        return {};
    const QRegion stable = queued.valid & before.valid & after.valid;
    const QRegion valid = targeted
        ? (reconstructed.valid & stable)
        : ((stable - reconstructed.target) | (reconstructed.valid & stable));
    // 缓存布局仍需比较几何位置，新增露出区域应由壁纸重建，不能沿用窗口残影。
    return {std::move(image), valid, area, before.scene, target, scale, before.targetBounds,
            false, targeted ? before.topology : 0, targetMayMove};
}

class CaptureWorker final : public QObject {
public:
    enum class RequestStatus { Pending, Running, Cancelled };
    using RequestState = std::atomic<RequestStatus>;
    std::shared_ptr<RequestState> prepare(QObject* receiver, WId target, bool latestOnly) {
        auto state = std::make_shared<RequestState>(RequestStatus::Pending);
        if (latestOnly) {
            const auto key = qMakePair(receiver, target);
            if (auto previous = pending.value(key).lock()) {
                auto expected = RequestStatus::Pending;
                previous->compare_exchange_strong(expected, RequestStatus::Cancelled);
            }
            pending.insert(key, state);
            if (pending.size() > 64) {
                for (auto it = pending.begin(); it != pending.end(); )
                    if (it.value().expired()) it = pending.erase(it);
                    else ++it;
            }
        }
        return state;
    }
    explicit CaptureWorker(QObject* parent) : QObject(parent) {
        // 使用单条捕获队列复用 WGC/D3D 流，避免争抢 GPU 回读带宽。
        pool.setMaxThreadCount(1);
        // 在线程退出之前通过普通任务释放捕获资源，避免驱动在系统加载器锁下销毁资源时死锁。
        pool.setExpiryTimeout(-1);
        idleCleanup.setSingleShot(true);
        idleCleanup.setTimerType(Qt::CoarseTimer);
        connect(&idleCleanup, &QTimer::timeout, this, [this]() {
            const qint64 remaining = 1500 - lastRequest.elapsed();
            if (remaining > 0) {
                idleCleanup.start(int(remaining));
                return;
            }
#ifdef Q_OS_WIN
            pool.start(QRunnable::create([]() { clearWindowCaptureStreams(); }));
#endif
        });
    }
    void noteRequest() {
        lastRequest.restart();
        if (!idleCleanup.isActive())
            idleCleanup.start(1500);
    }
    ~CaptureWorker() override {
        idleCleanup.stop();
        pool.clear();
#ifdef Q_OS_WIN
        // 在拥有缓存的工作线程中销毁 WGC 会话，再结束线程池。
        pool.start(QRunnable::create([]() { clearWindowCaptureStreams(); }));
#endif
        pool.waitForDone();
    }
    QThreadPool pool;
private:
    QHash<QPair<QObject*, WId>, std::weak_ptr<RequestState>> pending;
    QTimer idleCleanup;
    QElapsedTimer lastRequest;
};

QPointer<CaptureWorker>& captureWorkerInstance()
{
    static QPointer<CaptureWorker> worker;
    return worker;
}
}

void DesktopCapture::request(const QRect& area, qreal pixelScale, WId target, QObject* context,
                             std::function<void(Frame)> completed, int priority, bool latestOnly,
                             bool targetMayMove)
{
    auto& worker = captureWorkerInstance();
    if (!worker)
        worker = new CaptureWorker(qApp);
    CaptureWorker* owner = worker;
    owner->noteRequest();
    const auto parts = captureParts(area, pixelScale);
    const QSize size(qMax(1, qRound(area.width() * pixelScale)),
                     qMax(1, qRound(area.height() * pixelScale)));
    const auto queued = capturePlan(parts, size, target, NativeWindows::cachedStack());
    const QPointer<QObject> guard(context);
    const auto state = owner->prepare(context, target, latestOnly);
    const auto receiverDestroyed = QObject::connect(context, &QObject::destroyed, owner, [state]() {
        auto expected = CaptureWorker::RequestStatus::Pending;
        state->compare_exchange_strong(expected, CaptureWorker::RequestStatus::Cancelled);
    });
    owner->pool.start(QRunnable::create([owner, parts, size, area, pixelScale, target, queued, guard,
                                       state, receiverDestroyed, targetMayMove, completed = std::move(completed)]() {
        Frame frame;
        auto expected = CaptureWorker::RequestStatus::Pending;
        try {
            // 工作线程只处理仍有用途的排队任务，已经运行的捕获不被中途打断。
            if (state->compare_exchange_strong(expected, CaptureWorker::RequestStatus::Running))
                frame = grabFrame(parts, size, area, pixelScale, target, queued, false, targetMayMove);
        }
        catch (...) { logCaptureFailure("worker-grab", target); } // 失败时也必须释放调用方的在途请求计数。
        QMetaObject::invokeMethod(owner, [guard, receiverDestroyed, completed, frame = std::move(frame)]() mutable {
            QObject::disconnect(receiverDestroyed);
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
    Frame frame;
    try {
        frame = grabFrame(parts, size, area, pixelScale, target,
                          capturePlan(parts, size, target));
    } catch (...) {
        // 同步捕获失败返回空帧，不让设备异常越过界面事件循环。
        logCaptureFailure("synchronous-grab", target);
    }
#ifdef Q_OS_WIN
    // 同步读取不是持续捕获流，用完立即关闭；异步工作线程继续复用会话。
    try { clearWindowCaptureStreams(); } catch (...) {}
#endif
    return frame;
}

void DesktopCapture::resetWorkerStreams()
{
#ifdef Q_OS_WIN
    CaptureWorker* owner = captureWorkerInstance();
    if (!owner)
        return;
    // 保留排队的完成回调，将资源释放移出界面线程。
    owner->pool.start(QRunnable::create([]() { clearWindowCaptureStreams(); }));
#endif
}

bool DesktopCapture::validate(Frame& frame)
{
    if (frame.image.isNull() || !frame.scene)
        return false;
    const Plan current = capturePlan(captureParts(frame.area, frame.scale), frame.image.size(), frame.target);
    if ((frame.topology ? current.topology != frame.topology : current.scene != frame.scene)
        || (frame.targetMayMove ? current.targetBounds.size() != frame.targetBounds.size()
                               : current.targetBounds != frame.targetBounds)) {
        frame = {};
        return false;
    }
    frame.valid &= current.valid;
    frame.validated = true;
    return true;
}

void DesktopCapture::Cache::clear()
{
    image = {};
    area = {};
    scene = 0;
    topology = 0;
    target = 0;
    recentlyCovered = {};
    coverageClock.invalidate();
}

bool DesktopCapture::Cache::current() const
{
    if (!scene) return false;
    const Plan current = capturePlan(captureParts(area, scale), image.size(), target,
                                      NativeWindows::cachedStack());
    return current.scene && (topology ? topology == current.topology : scene == current.scene);
}

QImage DesktopCapture::Cache::merge(Frame frame, const std::function<QImage()>& fallback)
{
    // 异步回调交付前已验证窗口层级，此处复用结果以减少每帧枚举开销。
    if (!frame.validated && !validate(frame))
        return {};
    if (area != frame.area || image.size() != frame.image.size() || scene != frame.scene) {
        const QImage previous = image;
        const QRect previousArea = area;
        const bool sameScene = scene == frame.scene;
        // 完整有效的新帧会覆盖全部像素，直接共享，省去随后立即被丢弃的壁纸分配与填充。
        const bool complete = frame.valid == QRegion(frame.image.rect());
        const QImage base = complete ? frame.image : fallback();
        image = complete ? frame.image : (base.isNull() ? QImage() : base.scaled(frame.image.size(), Qt::IgnoreAspectRatio,
                                                       Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32));
        if (image.isNull()) {
            image = QImage(frame.image.size(), QImage::Format_RGB32);
            image.fill(Qt::black);
        }
        if (!complete && sameScene && !previous.isNull() && previousArea.intersects(frame.area)) {
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
    topology = frame.topology;
    scale = frame.scale;
    // 原生窗口移动后 DWM 可能滞后一帧，短暂保护新增露出区域以避免残影。
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
        // 静态桌面复用共享图像；完整新帧直接接管，避免额外分配和复制。
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
