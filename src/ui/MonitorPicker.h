#pragma once
#include <Windows.h>
#include <string>
#include <vector>

// MonitorPicker — enumerates connected monitors and provides an ImGui modal
// for picking one as the WGC capture source for the overlay.
class MonitorPicker {
public:
    struct MonitorEntry {
        HMONITOR    hmon;
        RECT        rect;      // virtual-screen coordinates
        bool        primary;
        std::string label;     // "Display 1 — 1920×1080 (Primary)"
    };

    void Refresh();

    const std::vector<MonitorEntry>& GetMonitors() const { return m_monitors; }
    HMONITOR GetSelected() const { return m_selected; }

private:
    static BOOL CALLBACK EnumProc(HMONITOR hmon, HDC, LPRECT, LPARAM lParam);

    std::vector<MonitorEntry> m_monitors;
    HMONITOR m_selected  = nullptr;
};
