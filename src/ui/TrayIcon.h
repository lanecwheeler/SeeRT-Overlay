#pragma once
#include <Windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include "../settings/Presets.h"

class TrayIcon {
public:
    static constexpr UINT WM_TRAY             = WM_APP + 100;
    static constexpr UINT CMD_SETTINGS        = 40000;
    static constexpr UINT CMD_PAUSE           = 40001;
    static constexpr UINT CMD_MOUSE           = 40002;
    static constexpr UINT CMD_QUIT            = 40003;
    static constexpr UINT CMD_SCREENSHOT      = 40004;
    static constexpr UINT CMD_OPEN_UPDATE     = 40005; // open the new release's page
    static constexpr UINT CMD_CHECK_UPDATES   = 40006;
    static constexpr UINT CMD_UPDATES_ON_LAUNCH = 40007;
    static constexpr UINT CMD_NOTIFICATION    = 40008; // the user clicked our notification
    static constexpr UINT CMD_PRESET          = 41000; // + index

    // What the context menu shows.
    struct MenuState {
        const std::vector<PresetItem>* presets = nullptr;
        bool        paused          = false;
        bool        mouseHide       = false;
        std::string version;               // this build, e.g. "1.1.0"
        std::string updateVersion;         // newer release available, or empty
        bool        checkingUpdates = false;
        bool        updatesOnLaunch = true;
    };

    // tip: the tooltip shown when hovering the icon.
    bool Init(HWND hwnd, const wchar_t* tip);
    void Destroy();

    // Shows a Windows notification from the tray icon.  Clicking it sends
    // CMD_NOTIFICATION back through HandleNotify.
    void ShowNotification(const std::wstring& title, const std::wstring& text);

    // Call from WndProc when WM_TRAY arrives.  Shows the context menu on a
    // click and returns the selected command ID (0 if nothing selected).
    UINT HandleNotify(HWND hwnd, LPARAM lParam, const MenuState& state);

private:
    NOTIFYICONDATAW m_nid   = {};
    bool            m_added = false;
};
