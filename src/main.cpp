#include <Windows.h>
#include <winrt/Windows.Foundation.h>  // winrt::init_apartment

// ---------------------------------------------------------------------------
// GPU selection hints — must be in the EXE, not a DLL.
//
// Nvidia Optimus and AMD PowerXpress scan the process image for these exported
// symbols before any D3D/DXGI code runs.  Without them, a laptop with an
// iGPU+dGPU pairing may silently route the overlay to the iGPU even though
// the programmatic adapter selection (IDXGIFactory6) would prefer the dGPU.
// ---------------------------------------------------------------------------
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement                 = 1;
    __declspec(dllexport) int   AmdPowerXpressRequestHighPerformance = 1;
}

// ImGui headers must precede the forward declaration that uses IMGUI_IMPL_API.
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM

#include "App.h"
#include "resource.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                               WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// Global app instance — accessible from both WndProcs.
// ---------------------------------------------------------------------------
static App* g_app = nullptr;

// ---------------------------------------------------------------------------
// StreamWndProc — message handler for the optional stream output window.
// ---------------------------------------------------------------------------
static LRESULT CALLBACK StreamWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (g_app && wParam != SIZE_MINIMIZED)
            g_app->OnStreamResize(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_CLOSE:
        if (g_app) g_app->CloseStreamWindow();
        return 0;

    case WM_DESTROY:
        return 0; // Do NOT PostQuitMessage — this is not the main window.

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// SecondaryWndProc — per-monitor overlay windows (no ImGui, CRT-only).
// ---------------------------------------------------------------------------
static LRESULT CALLBACK SecondaryWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (g_app && wParam != SIZE_MINIMIZED)
            g_app->OnSecondaryResize(hwnd, LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_DESTROY:
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// WndProc — fullscreen overlay window.
//
// When the settings panel is closed the overlay is WS_EX_LAYERED |
// WS_EX_TRANSPARENT, so DWM never sends it mouse messages — all clicks pass
// directly to whatever window is below in Z-order (the game / browser).
//
// When the settings panel is open WS_EX_TRANSPARENT is removed, the overlay
// receives mouse events, and they are forwarded to ImGui so the user can
// interact with the settings controls.  WS_EX_NOACTIVATE keeps keyboard
// focus in the game throughout.
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {

    // Hide the real cursor while the settings panel is open — a virtual cursor
    // rendered into the intermediate RT (and thus CRT-filtered) replaces it.
    // This must come before the ImGui block so ImGui cannot override it.
    if (msg == WM_SETCURSOR && g_app && g_app->IsMenuOpen()) {
        SetCursor(nullptr);
        return TRUE;
    }

    // Hit-test routing — must run BEFORE the ImGui forwarding block so that
    // ImGui_ImplWin32_WndProcHandler never gets to consume WM_NCHITTEST.
    // (If it returned non-zero, our WndProc would return 1 = HTCLIENT to
    // Windows, making the whole overlay eat all clicks.)
    //
    //   Menu / welcome open → HTCLIENT  (full overlay receives input)
    //   Shake button only   → HTCLIENT inside button rect; HTTRANSPARENT elsewhere
    //   Otherwise           → HTTRANSPARENT (fully click-through)
    if (msg == WM_NCHITTEST && g_app) {
        if (g_app->IsMenuOpen() || g_app->IsWelcomeVisible())
            return HTCLIENT;
        if (g_app->IsShakeInteractive()) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            return g_app->IsInShakeButtonRect(pt) ? HTCLIENT : HTTRANSPARENT;
        }
        return HTTRANSPARENT;
    }

    // Forward to ImGui when the overlay is interactive (menu, welcome, or shake button).
    if (g_app && (g_app->IsMenuOpen() || g_app->IsWelcomeVisible() || g_app->IsShakeInteractive())) {
        if (ImGui::GetCurrentContext() &&
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
            return true;
    }

    switch (msg) {
    case TrayIcon::WM_TRAY:
        if (g_app) g_app->OnTrayNotify(wParam, lParam);
        return 0;

    case WM_SIZE:
        if (g_app && wParam != SIZE_MINIMIZED)
            g_app->OnResize(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    // Global hotkeys (registered with RegisterHotKey in WinMain).
    //   ID 1 — Ctrl+Shift+C : toggle settings panel
    //   ID 2 — Ctrl+Shift+Q : quit immediately
    case WM_HOTKEY:
        if (wParam == 1 && g_app) { g_app->ToggleMenu(); return 0; }
        if (wParam == 2)          { PostQuitMessage(0);  return 0; }
        return 0;

    // Safety net: apply WS_EX_TRANSPARENT to any child windows DXGI may
    // create lazily on the first Present().
    case WM_PARENTNOTIFY:
        if (LOWORD(wParam) == WM_CREATE && g_app)
            g_app->MakeChildrenTransparent();
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// RegisterWindowClasses
// ---------------------------------------------------------------------------
static void RegisterWindowClasses(HINSTANCE hInstance) {
    // Main fullscreen overlay window
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SeeRTOverlay_Output";
    RegisterClassExW(&wc);

    // Secondary per-monitor overlay windows
    WNDCLASSEXW ws2 = {};
    ws2.cbSize        = sizeof(ws2);
    ws2.style         = CS_HREDRAW | CS_VREDRAW;
    ws2.lpfnWndProc   = SecondaryWndProc;
    ws2.hInstance     = hInstance;
    ws2.lpszClassName = L"SeeRTOverlay_Secondary";
    RegisterClassExW(&ws2);

    // Optional stream output window
    WNDCLASSEXW ws = {};
    ws.cbSize        = sizeof(ws);
    ws.style         = CS_HREDRAW | CS_VREDRAW;
    ws.lpfnWndProc   = StreamWndProc;
    ws.hInstance     = hInstance;
    ws.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    ws.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    ws.lpszClassName = L"SeeRTOverlay_Stream";
    RegisterClassExW(&ws);
}

// ---------------------------------------------------------------------------
// CreateOutputWindow — fullscreen topmost overlay.
// ---------------------------------------------------------------------------
static HWND CreateOutputWindow(HINSTANCE hInstance) {
    // WS_EX_LAYERED | WS_EX_TRANSPARENT: DWM excludes layered+transparent
    // windows from hit-testing entirely, routing all clicks cross-process to
    // whatever window is below in Z-order (the game / browser).
    // WS_EX_TRANSPARENT is toggled off while the settings panel is open so
    // ImGui can receive mouse input.
    // The swap chain is attached via DirectComposition (not CreateSwapChainForHwnd)
    // so it is compatible with WS_EX_LAYERED.
    //
    // WS_EX_NOACTIVATE: overlay never steals keyboard focus.
    // WS_EX_TOPMOST:    overlay stays above game windows.
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT;
    const DWORD style   = WS_POPUP | WS_VISIBLE;

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(
        exStyle,
        L"SeeRTOverlay_Output",
        L"SeeRT Overlay",
        style,
        0, 0, screenW, screenH,
        nullptr, nullptr, hInstance, nullptr);

    return hwnd;
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int /*nCmdShow*/) {
    winrt::init_apartment();

    RegisterWindowClasses(hInstance);

    HWND hwnd = CreateOutputWindow(hInstance);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create output window.", L"SeeRT Overlay", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Set window icon from embedded resource (shown in Alt+Tab and taskbar).
    if (HICON hIcon = static_cast<HICON>(
            LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP),
                       IMAGE_ICON, 0, 0, LR_DEFAULTSIZE))) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG,   reinterpret_cast<LPARAM>(hIcon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
    }

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'C');
    RegisterHotKey(hwnd, 2, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'Q');

    App app;
    g_app = &app;

    if (!app.Init(hwnd)) {
        MessageBoxW(nullptr,
                    L"Failed to initialise Direct3D / WGC.\n"
                    L"Ensure you are running Windows 10 1803 or later.",
                    L"SeeRT Overlay", MB_OK | MB_ICONERROR);
        g_app = nullptr;
        return 1;
    }

    MSG msg = {};
    while (true) {
        // Wait until the DXGI swap chain is ready to accept the next frame.
        // With MaxFrameLatency(1) this fires once per vsync interval, replacing
        // the old busy-poll + Present(1,0) block with a proper kernel wait.
        // The alertable wait (bAlertable=TRUE) keeps the thread responsive to
        // APCs; 1000 ms timeout guards against a stalled swap chain.
        if (HANDLE h = app.GetFrameLatencyHandle())
            WaitForSingleObjectEx(h, 1000, TRUE);

        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                UnregisterHotKey(hwnd, 1);
                UnregisterHotKey(hwnd, 2);
                app.Shutdown();
                g_app = nullptr;
                return static_cast<int>(msg.wParam);
            }
        }
        app.Tick();
    }
}
