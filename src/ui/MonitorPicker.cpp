#include "MonitorPicker.h"

#include <cstdio>    // sprintf_s
#include <algorithm> // std::sort

// ---------------------------------------------------------------------------
// EnumDisplayMonitors callback
// ---------------------------------------------------------------------------
BOOL CALLBACK MonitorPicker::EnumProc(HMONITOR hmon, HDC, LPRECT, LPARAM lParam) {
    auto* list = reinterpret_cast<std::vector<MonitorEntry>*>(lParam);

    MONITORINFOEXA info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoA(hmon, &info)) return TRUE;

    MonitorEntry e;
    e.hmon    = hmon;
    e.rect    = info.rcMonitor;
    e.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    list->push_back(e);
    return TRUE;
}

// ---------------------------------------------------------------------------
void MonitorPicker::Refresh() {
    m_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, EnumProc,
                        reinterpret_cast<LPARAM>(&m_monitors));

    // Primary monitor first, then by horizontal position.
    std::sort(m_monitors.begin(), m_monitors.end(),
              [](const MonitorEntry& a, const MonitorEntry& b) {
                  if (a.primary != b.primary) return a.primary > b.primary;
                  return a.rect.left < b.rect.left;
              });

    // Re-label with sorted indices so "Display 1" is always the primary.
    for (int i = 0; i < static_cast<int>(m_monitors.size()); ++i) {
        auto& e = m_monitors[i];
        const int w = e.rect.right  - e.rect.left;
        const int h = e.rect.bottom - e.rect.top;
        char buf[128];
        if (e.primary)
            sprintf_s(buf, "Display %d \xe2\x80\x94 %d\xc3\x97%d  (Primary)", i + 1, w, h);
        else
            sprintf_s(buf, "Display %d \xe2\x80\x94 %d\xc3\x97%d", i + 1, w, h);
        e.label = buf;
    }
}
