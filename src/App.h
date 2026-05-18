#pragma once
// WinRT headers must precede Windows.h to avoid macro conflicts.
#include "capture/WGCCapture.h"
#include <Windows.h>
#include <memory>
#include <string>
#include <chrono>
#include "render/D3DRenderer.h"
#include "ui/MonitorPicker.h"
#include "ui/WindowPicker.h"
#include "ui/SettingsPanel.h"
#include "ui/TrayIcon.h"
#include "settings/Settings.h"
#include "settings/Presets.h"
#include "audio/AudioLoopback.h"

// App is the top-level coordinator that owns all subsystems and drives the
// per-frame render loop.
class App {
public:
    App();
    ~App();

    App(const App&)            = delete;
    App& operator=(const App&) = delete;

    bool Init(HWND hwnd);
    void Tick();

    void SetMonitorCaptureTarget(HMONITOR hmon);

    // Add or remove a secondary overlay on an additional monitor.
    void AddMonitorOverlay(HMONITOR hmon);
    void RemoveMonitorOverlay(HMONITOR hmon);

    // Called from SecondaryWndProc on WM_SIZE.
    void OnSecondaryResize(HWND hwnd, int w, int h);

    void OnResize(int w, int h);
    void OnStreamResize(int w, int h);

    void OpenStreamWindow();
    void CloseStreamWindow();
    void SetStreamCaptureTarget(HWND target);
    void SetStreamCaptureMonitor(HMONITOR hmon);

    void Shutdown();

    // Called from WndProc when WM_TRAY arrives.
    void OnTrayNotify(WPARAM wParam, LPARAM lParam);

    // Toggle the settings panel inside the overlay.
    // Also toggles WS_EX_TRANSPARENT so the overlay is click-through when
    // the panel is closed, and receives input when it is open.
    void ToggleMenu();

    bool IsMenuOpen() const { return m_settings.menuVisible; }

    // True when the shake-popup button is visible and interactive.
    bool IsShakeInteractive() const;

    // True while the launch welcome popup is showing.
    bool IsWelcomeVisible() const { return m_welcomeVisible; }

    // Returns true if pt (window-client coords) falls within the shake button rect.
    bool IsInShakeButtonRect(POINT pt) const;

    // DXGI waitable-swap-chain handle — main loop waits on this before each
    // Tick() so the CPU sleeps until DXGI is ready for the next frame.
    HANDLE GetFrameLatencyHandle() const { return m_renderer.GetFrameLatencyHandle(); }

    // Apply WS_EX_TRANSPARENT to any DXGI-internal windows (safety net).
    void MakeChildrenTransparent();

private:
    void InitImGui(HWND hwnd);
    void BeginImGuiFrame();

    HWND m_outputHwnd = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device>        m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;

    WGCCapture   m_capture;
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame m_currentFrame{ nullptr };

    WGCCapture   m_streamCapture;
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame m_streamCurrentFrame{ nullptr };
    HWND         m_streamHwnd = nullptr;
    bool         m_streamOpen = false;
    AudioLoopback m_audioLoopback;

    D3DRenderer   m_renderer;

    MonitorPicker m_monitorPicker;
    HMONITOR      m_selectedMonitor = nullptr;

    WindowPicker  m_streamPicker;
    SettingsPanel m_settingsPanel;
    TrayIcon      m_trayIcon;

    bool m_overlayPaused = false;

    struct AudioDevEntry { std::string id; std::string name; };
    std::vector<AudioDevEntry> m_audioDevices;
    void RefreshAudioDevices();

    CRTSettings m_settings;
    std::string m_settingsPath;
    bool        m_shutdownCalled = false;

    std::vector<UserPreset>  m_userPresets;
    std::vector<std::string> m_presetOrder;
    std::vector<PresetItem>  m_orderedPresets;
    std::string              m_presetsPath;

    void RebuildOrderedPresets();

    struct SecondaryOverlay {
        HWND        hwnd = nullptr;
        HMONITOR    hmon = nullptr;
        WGCCapture  capture;
        D3DRenderer renderer;
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame currentFrame{ nullptr };

        SecondaryOverlay() = default;
        ~SecondaryOverlay() = default;
        SecondaryOverlay(const SecondaryOverlay&)            = delete;
        SecondaryOverlay& operator=(const SecondaryOverlay&) = delete;
    };
    std::vector<std::unique_ptr<SecondaryOverlay>> m_secondaryOverlays;

    // Raw window-relative cursor position (pixels, set each frame in BeginImGuiFrame).
    // Used to draw the virtual cursor in the intermediate RT so it is barrel-
    // distorted by the CRT shader and appears on the curved surface.
    float m_rawCursorX = 0.f;
    float m_rawCursorY = 0.f;

    // Custom cursor texture loaded from cursor.png at startup.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_cursorSRV;
    int m_cursorTexW = 0;
    int m_cursorTexH = 0;

    // Shake-to-show state.
    POINT m_lastMousePt      = {};   // previous frame mouse position
    float m_shakeAccum       = 0.f; // decaying speed accumulator (px/frame units)
    float m_cursorShowTimer  = 0.f; // seconds remaining to show cursor/button after shake
    float m_prevTime         = 0.f; // previous m_settings.time, for dt
    bool  m_shakeInteractive = false; // overlay made non-transparent for shake button
    bool  m_cursorHidden    = true;  // true when system cursor is blanked (on captured monitor)
    bool  m_welcomeVisible  = true;  // one-shot launch popup; never persisted
    RECT  m_welcomeRect     = {};    // actual popup bounds, updated each frame
    HWND  m_prevForeground  = nullptr; // window that had focus before settings opened

    using Clock = std::chrono::steady_clock;
    Clock::time_point m_startTime{ Clock::now() };
};
