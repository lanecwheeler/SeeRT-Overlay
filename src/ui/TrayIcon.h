#pragma once
#include <Windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include "../settings/Presets.h"

class TrayIcon {
public:
    static constexpr UINT WM_TRAY        = WM_APP + 100;
    static constexpr UINT CMD_SETTINGS   = 40000;
    static constexpr UINT CMD_PAUSE      = 40001;
    static constexpr UINT CMD_MOUSE      = 40002;
    static constexpr UINT CMD_QUIT       = 40003;
    static constexpr UINT CMD_PRESET     = 41000; // + index

    bool Init(HWND hwnd);
    void Destroy();

    // Call from WndProc when WM_TRAY arrives.  Shows context menu on right-click
    // and returns the selected command ID (0 if nothing selected).
    UINT HandleNotify(HWND hwnd, LPARAM lParam,
                      const std::vector<PresetItem>& presets,
                      bool paused, bool mouseHide);

private:
    NOTIFYICONDATAW m_nid   = {};
    bool            m_added = false;
};
