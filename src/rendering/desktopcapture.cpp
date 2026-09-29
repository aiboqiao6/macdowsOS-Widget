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
            // A GUI thread may already be STA. That is a supported state for
            // this caller, so inspect the HRESULT instead of intentionally
            // throwing a WinRT exception that breaks the Visual Studio debugger.
            const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            owned = SUCCEEDED(result); // S_FALSE still needs CoUninitialize.
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

// Windows Graphics Capture is a streaming API. Recreating its D3D device,
// frame pool and capture session for every requested image costs at least one
// compositor frame and previously limited a live panel to roughly 4-20 fps.
// Keep one stream per lower HWND on the calling capture thread and read its
// newest completed frame without restarting the session.
class WindowCaptureStream final {
    struct CapturedFrame {
        explicit CapturedFrame(wgc::Direct3D11CaptureFrame value) : frame(std::move(value)) {}
        ~CapturedFrame() { try { if (frame) frame.Close(); } catch (...) {} }
        wgc::Direct3D11CaptureFrame frame{nullptr};
    };
    // WGC may deliver an already-dispatched event after revocation. Delegates
    // own only this mailbox, never the stream or its D3D resources.
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
        catch (...) { close(); }
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
        // Each operation must still run if another one fails on device loss.
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
                    // Only the first read waits for WGC startup. Every later
                    // request consumes the newest already-delivered frame.
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
        // Recheck after device creation: a popup can disappear while the D3D
        // device is being initialized. HRESULT handling avoids an additional
        // application throw, but cannot suppress a throw inside the Windows
        // implementation; unsupported HWNDs must be rejected before this call.
        if (!validWindow())
            return;
        const HRESULT itemResult = interop->CreateForWindow(
            m_window, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(m_item));
        if (FAILED(itemResult)) {
            m_item = nullptr;
            qWarning("Windows Graphics Capture cannot create an item for HWND %p (HRESULT 0x%08lx)",
                     static_cast<void*>(m_window), static_cast<unsigned long>(itemResult));
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
        // Recent Windows versions expose an explicit capture throttle. Ask
        // for every available compositor update, including high-Hz displays.
        if (auto cadence = m_session.try_as<wgc::IGraphicsCaptureSession5>())
            cadence.MinUpdateInterval(std::chrono::milliseconds(0));
        m_token = m_pool.FrameArrived([state = m_state](auto const& sender, auto const&) {
            try {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->stopping || state->recreating)
                    return;
                // Drain queued frames: displaying the oldest pool entry can
                // keep a busy source visibly behind even with frequent paints.
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
        // Recreate can wait for FrameArrived. Never hold its mailbox lock here.
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
        // WGC's BGRA8 memory layout already matches QImage::Format_ARGB32 on
        // Windows. Returning it directly avoids an additional full-frame
        // conversion/copy on every live sample.
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
    // Construct the apartment first so TLS destroys the streams before COM.
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
                // Avoid recreating a rejected item dozens of times per second
                // while the same unsupported or closing window remains listed.
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
                                       WId target, const NativeWindows::Stack& stack)
{
    TargetReconstruction result;
    const int targetIndex = target ? NativeWindows::indexOf(stack, target) : -1;
    if (targetIndex < 0 || image.isNull())
        return result;
    const QRect targetBounds = stack.at(targetIndex).bounds;
    for (const CapturePart& part : parts)
        result.target += projectToCapture(targetBounds, part);
#ifdef Q_OS_WIN
    QRegion remaining = result.target;
    // EnumWindows is top-to-bottom. Repaint the nearest ordinary opaque
    // window for each target pixel. Wallpaper remains invalid and is
    // supplied by the caller's stable fallback; glass windows are composited
    // separately from their last completed surfaces.
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
        // WS_EX_LAYERED is not synonymous with invisible or non-capturable.
        // Qt, Chromium and many media windows use a layered top-level HWND
        // while still presenting fully opaque changing content. Skipping those
        // windows was the direct cause of live mode falling back to wallpaper
        // forever for common sources. WGC provides the selected HWND's pixels.
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
        // WGC owns these pixels. Solid black/white are valid content, not
        // evidence of failure. Never replace them with an old frame or a GDI
        // screenshot (which may include our own glass window).
        if (captured.pixels.isNull()) {
            // WorkerW/Progman may host a live wallpaper. Try the other shell
            // host if one cannot be captured; only then use the file fallback.
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
        // GDI captures process-local glass windows even though they are
        // excluded from our logical compositor. Mark those physical pixels
        // invalid before they can enter the external backdrop cache; the
        // caller composites each lower glass surface exactly once afterwards.
        // Without this subtraction an overlapping card is present in both the
        // screenshot and its cached surface, producing recursive glare while
        // the upper card is dragged.
        if (window.excluded) {
            for (const auto& part : parts)
                plan.valid -= projectToCapture(window.bounds, part);
            continue;
        }
        bool intersects = false;
        for (const auto& part : parts) {
            // Shadows and DWM's rounded frame extend beyond GetWindowRect on
            // some themes. Be conservative even for layered/translucent covers.
            const QRect bounds = window.bounds.adjusted(-16, -16, 16, 16);
            if (!bounds.intersects(part.source))
                continue;
            intersects = true;
            // Transparent/layered shell overlays often cover the entire
            // virtual desktop while contributing no opaque pixels. Treating
            // their bounds as an occluder invalidated the complete card crop,
            // so live mode returned a black/wallpaper frame without ever
            // reaching the real lower window.
            if (i < targetIndex && window.opaque)
                plan.valid -= projectToCapture(bounds, part);
        }
        if (i > targetIndex && intersects) {
            // WGC captures each HWND independently. Moving a lower window
            // changes its layout, not its identity as a safe backdrop source.
            // Only relevant windows participate; unrelated UI elsewhere must
            // not invalidate this crop while their native windows initialize.
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
                                bool validateOnReturn = true)
{
#ifdef Q_OS_WIN
    pruneWindowCaptureStreams();
#endif
    const NativeWindows::Stack beforeStack = NativeWindows::snapshot();
    const Plan before = capturePlan(parts, size, target, beforeStack);
    if (!queued.scene || (target ? queued.topology != before.topology : queued.scene != before.scene)
        || queued.targetBounds != before.targetBounds)
        return {};
    if ((queued.valid & before.valid).isEmpty()) {
        QImage blank(size, QImage::Format_RGB32);
        blank.fill(Qt::black);
        return {std::move(blank), {}, area, before.scene, target, scale, before.targetBounds,
                false, target ? before.topology : 0};
    }
    const bool targeted = target && NativeWindows::indexOf(beforeStack, target) >= 0;
    QImage image;
    if (targeted) {
        // Reconstruct only the target rectangle from independently captured
        // lower HWNDs. Everything else stays invalid and is filled from the
        // stable wallpaper cache by the caller.
        image = QImage(size, QImage::Format_RGB32);
        image.fill(Qt::black);
    } else {
        image = grabParts(parts, size);
    }
    const TargetReconstruction reconstructed = reconstructTarget(
        image, parts, target, beforeStack);
    // Async delivery validates against a fresh native stack on the GUI thread
    // before exposing any pixels. Avoid doing that same enumeration twice.
    const Plan after = validateOnReturn ? capturePlan(parts, size, target) : before;
    if ((targeted ? before.topology != after.topology : before.scene != after.scene)
        || before.targetBounds != after.targetBounds || image.isNull())
        return {};
    const QRegion stable = queued.valid & before.valid & after.valid;
    const QRegion valid = targeted
        ? (reconstructed.valid & stable)
        : ((stable - reconstructed.target) | (reconstructed.valid & stable));
    // Geometry still identifies the cache layout, so newly uncovered areas
    // are rebuilt from wallpaper instead of retaining a moving-window ghost.
    return {std::move(image), valid, area, before.scene, target, scale, before.targetBounds,
            false, targeted ? before.topology : 0};
}

class CaptureWorker final : public QObject {
public:
    explicit CaptureWorker(QObject* parent) : QObject(parent) {
        // Keep one capture lane so live requests reuse the same WGC/D3D
        // stream and do not compete for GPU readback bandwidth.
        pool.setMaxThreadCount(1);
        // Do not destroy D3D/WGC thread-local objects during Windows thread
        // detach. Some AMD drivers then wait for another thread while holding
        // the loader lock, freezing later GUI/plugin loads. Keep the lane
        // alive and release their capture resources in ordinary work items.
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
        // Destroy WGC sessions on the worker thread that owns their cache
        // before asking QThreadPool to retire that thread.
        pool.start(QRunnable::create([]() { clearWindowCaptureStreams(); }));
#endif
        pool.waitForDone();
    }
    QThreadPool pool;
private:
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
                             std::function<void(Frame)> completed, int priority)
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
    owner->pool.start(QRunnable::create([owner, parts, size, area, pixelScale, target, queued, guard,
                                       completed = std::move(completed)]() {
        Frame frame;
        try { frame = grabFrame(parts, size, area, pixelScale, target, queued, false); }
        catch (...) {} // Always release the caller's in-flight slot on failure.
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
    Frame frame;
    try {
        frame = grabFrame(parts, size, area, pixelScale, target,
                          capturePlan(parts, size, target));
    } catch (...) {
        // Match the asynchronous path: a transient capture/device failure
        // yields an empty frame instead of escaping through the GUI event loop.
    }
#ifdef Q_OS_WIN
    // Synchronous grabs run on their caller (normally the GUI thread) and do
    // not form a live stream. Close any session created for this one-shot read
    // immediately; asynchronous worker requests retain and reuse theirs.
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
    // Preserve queued completions and keep teardown off the GUI thread.
    owner->pool.start(QRunnable::create([]() { clearWindowCaptureStreams(); }));
#endif
}

bool DesktopCapture::validate(Frame& frame)
{
    if (frame.image.isNull() || !frame.scene)
        return false;
    const Plan current = capturePlan(captureParts(frame.area, frame.scale), frame.image.size(), frame.target);
    if ((frame.topology ? current.topology != frame.topology : current.scene != frame.scene)
        || current.targetBounds != frame.targetBounds) {
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
    // Asynchronous requests are validated immediately before their callback.
    // Avoid enumerating the complete native window stack a second time in
    // that same callback; this was a significant part of every live frame.
    if (!frame.validated && !validate(frame))
        return {};
    if (area != frame.area || image.size() != frame.image.size() || scene != frame.scene) {
        const QImage previous = image;
        const QRect previousArea = area;
        const bool sameScene = scene == frame.scene;
        const QImage base = fallback();
        image = base.isNull() ? QImage() : base.scaled(frame.image.size(), Qt::IgnoreAspectRatio,
                                                       Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
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
    topology = frame.topology;
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
