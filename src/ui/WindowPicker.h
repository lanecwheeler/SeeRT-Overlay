#pragma once
#include <Windows.h>
#include <string>
#include <vector>

class WindowPicker {
public:
    // Rebuild the window list (monitors are fixed at Open() time).
    void Refresh();

    // Draw the ImGui modal.  Call every frame while IsOpen().
    // Returns true on the frame the user commits a selection (OK pressed).
    bool Draw();

    bool  IsOpen()      const { return m_open; }
    void  Open(std::vector<std::string>& hiddenSources);
    void  Close();

    // Valid only after Draw() returns true.  Exactly one will be non-null.
    HWND     GetSelected()        const { return m_selected; }
    HMONITOR GetSelectedMonitor() const { return m_selectedMonitor; }

private:
    struct WindowEntry {
        HWND        hwnd  = nullptr;
        std::string title;
    };
    struct MonitorEntry {
        HMONITOR    hmon    = nullptr;
        std::string label;
        RECT        rc      = {};
        bool        primary = false;
    };

    static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lParam);
    static BOOL CALLBACK MonitorEnumProc(HMONITOR hmon, HDC, LPRECT, LPARAM lParam);

    static std::string BaseTitle(const std::string& title);

    std::vector<MonitorEntry>  m_monitors;
    std::vector<WindowEntry>   m_windows;

    // Flat index: 0..monitors-1 are monitors, monitors..total-1 are windows.
    int      m_listIndex       = 0;
    HWND     m_selected        = nullptr;
    HMONITOR m_selectedMonitor = nullptr;
    bool     m_open            = false;

    std::vector<std::string>* m_hiddenSources = nullptr;
};
