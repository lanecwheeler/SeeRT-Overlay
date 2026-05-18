#include "WGCCapture.h"

// WGC ↔ D3D11 interop headers — must be included after the WinRT headers.
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <dxgi.h>
#include <stdexcept>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "WindowsApp.lib")  // WinRT / C++/WinRT runtime

// ---------------------------------------------------------------------------
// Internal helper: wrap a D3D11 device as a WinRT IDirect3DDevice.
// This is what WGC expects when creating a frame pool.
// ---------------------------------------------------------------------------
static winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice
    CreateWinRTDevice(ID3D11Device* d3dDevice)
{
    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    winrt::check_hresult(d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())));

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(
        ::CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));

    return inspectable.as<
        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
}

// ---------------------------------------------------------------------------
bool WGCCapture::Init(ID3D11Device* device) {
    if (!device) return false;
    try {
        m_wrtDevice = CreateWinRTDevice(device);
        return true;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
bool WGCCapture::StartCapture(HWND targetHwnd) {
    Stop(); // Release any existing session first.

    if (!m_wrtDevice) return false;

    try {
        // Build a GraphicsCaptureItem from the HWND via the interop factory.
        auto factory = winrt::get_activation_factory<
            wgc::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();

        winrt::check_hresult(
            factory->CreateForWindow(
                targetHwnd,
                winrt::guid_of<wgc::GraphicsCaptureItem>(),
                winrt::put_abi(m_item)));

        m_captureSize = m_item.Size();

        // A zero-size item (minimised window, virtual-desktop) would make
        // CreateFreeThreaded throw or produce unusable pool textures.
        if (m_captureSize.Width <= 0 || m_captureSize.Height <= 0) {
            OutputDebugStringA("WGCCapture: capture item has zero size — aborting\n");
            m_item = nullptr;
            return false;
        }

        // Create the frame pool.
        // We request B8G8R8A8_UNorm because that is what D3D11 swap chains
        // typically use and avoids a format conversion step.
        m_pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            m_wrtDevice,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,              // frame buffer count
            m_captureSize);

        m_session = m_pool.CreateCaptureSession(m_item);

        // Don't bake the captured window's cursor into the frame.
        // With the fullscreen overlay, the system cursor is already visible
        // naturally; baking it would produce a double-cursor artefact.
        m_session.IsCursorCaptureEnabled(false);

        // Remove the yellow screen-recording border indicator (Windows 11+).
        // IsBorderRequired defaults to true; setting it false suppresses the
        // animated yellow highlight that WGC draws around the captured source.
        // The property exists on GraphicsCaptureSession since SDK 22000; on older
        // SDKs the try_as returns null and we skip silently.
        if (auto s2 = m_session.try_as<wgc::IGraphicsCaptureSession3>())
            s2.IsBorderRequired(false);

        // Listen for size changes so we can recreate the pool if the captured
        // window is resized.  The lambda is minimal — the App layer polls
        // TryGetFrame() and should handle SIZE_CHANGED frames gracefully.
        m_pool.FrameArrived([this](wgc::Direct3D11CaptureFramePool const& pool,
                                   winrt::Windows::Foundation::IInspectable const&) {
            // No-op: the render loop polls via TryGetFrame().
            // We register the callback simply to wake the pool's internal
            // bookkeeping; actual frame retrieval is pull-based.
            (void)pool;
        });

        m_session.StartCapture();
        return true;
    } catch (...) {
        Stop();
        return false;
    }
}

// ---------------------------------------------------------------------------
bool WGCCapture::StartCaptureMonitor(HMONITOR hmon) {
    Stop();

    if (!m_wrtDevice || !hmon) return false;

    try {
        auto factory = winrt::get_activation_factory<
            wgc::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();

        winrt::check_hresult(
            factory->CreateForMonitor(
                hmon,
                winrt::guid_of<wgc::GraphicsCaptureItem>(),
                winrt::put_abi(m_item)));

        m_captureSize = m_item.Size();

        if (m_captureSize.Width <= 0 || m_captureSize.Height <= 0) {
            OutputDebugStringA("WGCCapture: monitor capture item has zero size\n");
            m_item = nullptr;
            return false;
        }

        m_pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            m_wrtDevice,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            m_captureSize);

        m_session = m_pool.CreateCaptureSession(m_item);

        // Bake the cursor into the captured monitor frame so it is visible
        // through the CRT filter (barrel distortion, scanlines, glow, etc.).
        m_session.IsCursorCaptureEnabled(true);

        // Suppress the yellow recording-indicator border (Windows 11+).
        if (auto s2 = m_session.try_as<wgc::IGraphicsCaptureSession3>())
            s2.IsBorderRequired(false);

        m_pool.FrameArrived([this](wgc::Direct3D11CaptureFramePool const& pool,
                                   winrt::Windows::Foundation::IInspectable const&) {
            (void)pool;
        });

        m_session.StartCapture();
        return true;
    } catch (...) {
        Stop();
        return false;
    }
}

// ---------------------------------------------------------------------------
wgc::Direct3D11CaptureFrame WGCCapture::TryGetFrame() {
    if (!m_pool) return nullptr;

    try {
        auto frame = m_pool.TryGetNextFrame();
        if (!frame) return nullptr;

        // If the content size changed (e.g. window was resized), recreate
        // the frame pool to match.  Returning the old frame is still valid.
        auto newSize = frame.ContentSize();
        if (newSize.Width  != m_captureSize.Width ||
            newSize.Height != m_captureSize.Height)
        {
            m_captureSize = newSize;
            m_pool.Recreate(
                m_wrtDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2,
                m_captureSize);
        }

        return frame;
    } catch (...) {
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
ID3D11Texture2D* WGCCapture::GetFrameTexture(const wgc::Direct3D11CaptureFrame& frame) {
    if (!frame) return nullptr;

    try {
        // IDirect3DDxgiInterfaceAccess lives in the plain C++ namespace
        // Windows::Graphics::DirectX::Direct3D11 (NOT winrt::Windows::...).
        // GetDXGIInterfaceFromObject is only compiled under __cplusplus_winrt (C++/CX),
        // so we QI for the interface directly via get_unknown().
        using DxgiAccess = ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;
        winrt::com_ptr<DxgiAccess> access;
        m_lastFrameTex = nullptr;
        winrt::check_hresult(
            winrt::get_unknown(frame.Surface())->QueryInterface(
                __uuidof(DxgiAccess), access.put_void()));
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(m_lastFrameTex.put())));

        return m_lastFrameTex.get();
    } catch (...) {
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
void WGCCapture::Stop() {
    if (m_session) {
        try { m_session.Close(); } catch (...) {}
        m_session = nullptr;
    }
    if (m_pool) {
        try { m_pool.Close(); } catch (...) {}
        m_pool = nullptr;
    }
    m_item         = nullptr;
    m_lastFrameTex = nullptr;
    m_captureSize  = {};
}

// ---------------------------------------------------------------------------
std::pair<int, int> WGCCapture::GetCaptureSize() const {
    return { m_captureSize.Width, m_captureSize.Height };
}
