#include "TrayIcon.h"
#include "../resource.h"

static std::wstring Widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

bool TrayIcon::Init(HWND hwnd, const wchar_t* tip) {
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
    wcsncpy_s(m_nid.szTip, tip, _TRUNCATE);
    m_added = Shell_NotifyIconW(NIM_ADD, &m_nid) != FALSE;
    return m_added;
}

void TrayIcon::Destroy() {
    if (m_added) {
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_added = false;
    }
}

void TrayIcon::ShowNotification(const std::wstring& title, const std::wstring& text) {
    if (!m_added) return;
    NOTIFYICONDATAW nid = m_nid;
    nid.uFlags      = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo,      text.c_str(),  _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

UINT TrayIcon::HandleNotify(HWND hwnd, LPARAM lParam, const MenuState& state) {
    if (lParam == NIN_BALLOONUSERCLICK) return CMD_NOTIFICATION;
    if (lParam != WM_RBUTTONUP && lParam != WM_LBUTTONUP) return 0;

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CMD_SETTINGS, L"Settings");
    AppendMenuW(menu, MF_STRING, CMD_SCREENSHOT, L"Save Screenshot\tCtrl+Shift+PrtScn");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (state.paused    ? MF_CHECKED : 0), CMD_PAUSE, L"Pause Overlay");
    AppendMenuW(menu, MF_STRING | (state.mouseHide ? MF_CHECKED : 0), CMD_MOUSE, L"Hide Mouse");

    if (state.presets && !state.presets->empty()) {
        const std::vector<PresetItem>& presets = *state.presets;
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        HMENU sub = CreatePopupMenu();
        for (int i = 0; i < (int)presets.size(); i++)
            AppendMenuW(sub, MF_STRING, CMD_PRESET + i, Widen(presets[i].name).c_str());
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, L"Preset");
    }

    // Updates: a download item when a newer release exists, and a submenu
    // with the current version, a manual check and the launch-check toggle.
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (!state.updateVersion.empty()) {
        const std::wstring item = L"Download Update (v" + Widen(state.updateVersion) + L")...";
        AppendMenuW(menu, MF_STRING, CMD_OPEN_UPDATE, item.c_str());
        SetMenuDefaultItem(menu, CMD_OPEN_UPDATE, FALSE);   // shown in bold
    }
    {
        HMENU sub = CreatePopupMenu();
        const std::wstring ver = L"SeeRT Overlay v" + Widen(state.version);
        AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, ver.c_str());
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        if (state.checkingUpdates)
            AppendMenuW(sub, MF_STRING | MF_GRAYED, CMD_CHECK_UPDATES, L"Checking for Updates...");
        else
            AppendMenuW(sub, MF_STRING, CMD_CHECK_UPDATES, L"Check for Updates Now");
        AppendMenuW(sub, MF_STRING | (state.updatesOnLaunch ? MF_CHECKED : 0), CMD_UPDATES_ON_LAUNCH,
                    L"Check for Updates on Launch");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, L"Updates");
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
