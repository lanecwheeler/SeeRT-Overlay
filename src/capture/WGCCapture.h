#pragma once
// C++/WinRT headers must come before any Windows.h inclusion that might
// define conflicting macros.  We include them here and let precompiled
// headers (or unity builds) handle the rest.
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <d3d11.h>
#include <utility>   // std::pair

namespace wgc = winrt::Windows::Graphics::Capture;

class WGCCapture {
public:
    WGCCapture() = default;
    ~WGCCapture() { Stop(); }

    // Non-copyable, movable.
    WGCCapture(const WGCCapture&)            = delete;
    WGCCapture& operator=(const WGCCapture&) = delete;

    // Initialise with an existing D3D11 device.
    // The capture system wraps this device as a WinRT IDirect3DDevice.
    bool Init(ID3D11Device* device);

    // Begin capturing the given window.  Replaces any existing capture.
    // Returns false if the window cannot be used as a capture source.
    bool StartCapture(HWND targetHwnd);

    // Begin capturing an entire monitor.  hmon must be a valid HMONITOR
    // returned by EnumDisplayMonitors or MonitorFromWindow.
    bool StartCaptureMonitor(HMONITOR hmon);

    // Poll for the next frame.  Returns an invalid frame (operator bool() ==
    // false) if no new frame is available yet.
    wgc::Direct3D11CaptureFrame TryGetFrame();

    // Extract the underlying D3D11 texture from a WGC frame.
    // The returned pointer is valid until the next call to GetFrameTexture or
    // Stop().  Do NOT Release() the pointer — WGCCapture owns the ref.
    ID3D11Texture2D* GetFrameTexture(const wgc::Direct3D11CaptureFrame& frame);

    // Stop the capture session and release WGC resources.
    void Stop();

    // Returns the pixel size of the currently captured window.
    std::pair<int, int> GetCaptureSize() const;

    bool IsCapturing() const { return m_session != nullptr; }

private:
    // D3D11 device wrapped as a WinRT IDirect3DDevice for WGC interop.
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_wrtDevice{ nullptr };

    wgc::GraphicsCaptureItem            m_item   { nullptr };
    wgc::Direct3D11CaptureFramePool     m_pool   { nullptr };
    wgc::GraphicsCaptureSession         m_session{ nullptr };

    winrt::Windows::Graphics::SizeInt32 m_captureSize{};

    // Cached texture ComPtr so the raw pointer returned by GetFrameTexture
    // remains valid across the caller's render call without the caller needing
    // to manage the COM lifetime.
    winrt::com_ptr<ID3D11Texture2D> m_lastFrameTex;
};
