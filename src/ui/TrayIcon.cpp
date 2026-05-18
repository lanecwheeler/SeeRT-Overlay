#include "TrayIcon.h"
#include "../resource.h"

bool TrayIcon::Init(HWND hwnd) {
    m_nid = {};
    m_nid.cbSize           = sizeof(m_nid);
    m_nid.hWnd             = hwnd;
    m_nid.uID              = 1;
    m_nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    m_nid.uCallbackMessage = WM_TRAY;
    m_nid.hIcon            = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP),
                   IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    if (!m_nid.hIcon)
        m_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION); // fallback
    wcscpy_s(m_nid.szTip, L"CRT Overlay");
    m_added = Shell_NotifyIconW(NIM_ADD, &m_nid) != FALSE;
    return m_added;
}

void TrayIcon::Destroy() {
    if (m_added) {
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_added = false;
    }
}

UINT TrayIcon::HandleNotify(HWND hwnd, LPARAM lParam,
                             const std::vector<PresetItem>& presets,
                             bool paused, bool mouseHide) {
    if (lParam != WM_RBUTTONUP && lParam != WM_LBUTTONUP) return 0;

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CMD_SETTINGS, L"Settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (paused    ? MF_CHECKED : 0), CMD_PAUSE, L"Pause Overlay");
    AppendMenuW(menu, MF_STRING | (mouseHide ? MF_CHECKED : 0), CMD_MOUSE, L"Hide Mouse");

    if (!presets.empty()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        HMENU sub = CreatePopupMenu();
        for (int i = 0; i < (int)presets.size(); i++) {
            int n = MultiByteToWideChar(CP_UTF8, 0, presets[i].name.c_str(), -1, nullptr, 0);
            std::wstring wname(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, presets[i].name.c_str(), -1, wname.data(), n);
            AppendMenuW(sub, MF_STRING, CMD_PRESET + i, wname.c_str());
        }
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, L"Preset");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_QUIT, L"Quit");

    // SetForegroundWindow required so the menu dismisses when the user clicks away.
    SetForegroundWindow(hwnd);
    POINT pt;
    GetCursorPos(&pt);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                              pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    return cmd;
}
