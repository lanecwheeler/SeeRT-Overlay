#include "WindowPicker.h"

#include <imgui.h>
#include <dwmapi.h>
#include <algorithm>
#include <cctype>

#pragma comment(lib, "dwmapi.lib")

// ---------------------------------------------------------------------------
std::string WindowPicker::BaseTitle(const std::string& title) {
    auto pos = title.find(" - ");
    if (pos != std::string::npos && pos > 0)
        return title.substr(0, pos);
    return title;
}

static bool ContainsCI(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return false;
    return std::search(haystack.begin(), haystack.end(),
                       needle.begin(), needle.end(),
                       [](char a, char b) {
                           return std::tolower((unsigned char)a) ==
                                  std::tolower((unsigned char)b);
                       }) != haystack.end();
}

// ---------------------------------------------------------------------------
// Monitor enumeration callback — stores raw data; labels built after sorting.
// ---------------------------------------------------------------------------
BOOL CALLBACK WindowPicker::MonitorEnumProc(HMONITOR hmon, HDC, LPRECT, LPARAM lParam) {
    auto* self = reinterpret_cast<WindowPicker*>(lParam);
    MONITORINFOEX info = {};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(hmon, &info)) {
        MonitorEntry e;
        e.hmon    = hmon;
        e.rc      = info.rcMonitor;
        e.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        self->m_monitors.push_back(e);
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
BOOL CALLBACK WindowPicker::EnumProc(HWND hwnd, LPARAM lParam) {
    auto* self = reinterpret_cast<WindowPicker*>(lParam);

    if (!IsWindowVisible(hwnd)) return TRUE;
    if (IsIconic(hwnd))         return TRUE;

    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED,
                                        &cloaked, sizeof(cloaked))) && cloaked)
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;

    RECT rc = {};
    GetClientRect(hwnd, &rc);
    if (rc.right - rc.left == 0 || rc.bottom - rc.top == 0) return TRUE;

    wchar_t title[256] = {};
    if (GetWindowTextW(hwnd, title, 256) == 0) return TRUE;

    char utf8[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, title, -1, utf8, sizeof(utf8), nullptr, nullptr);

    WindowEntry entry;
    entry.hwnd  = hwnd;
    entry.title = utf8;
    self->m_windows.push_back(std::move(entry));
    return TRUE;
}

// ---------------------------------------------------------------------------
void WindowPicker::Refresh() {
    m_windows.clear();
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(this));

    if (m_hiddenSources && !m_hiddenSources->empty()) {
        m_windows.erase(
            std::remove_if(m_windows.begin(), m_windows.end(),
                [this](const WindowEntry& e) {
                    for (const auto& f : *m_hiddenSources)
                        if (ContainsCI(e.title, f)) return true;
                    return false;
                }),
            m_windows.end());
    }

    std::sort(m_windows.begin(), m_windows.end(),
              [](const WindowEntry& a, const WindowEntry& b) {
                  return a.title < b.title;
              });

    // Clamp index to new total (monitors never change after Open).
    const int total = (int)m_monitors.size() + (int)m_windows.size();
    if (m_listIndex >= total && total > 0)
        m_listIndex = total - 1;
}

// ---------------------------------------------------------------------------
void WindowPicker::Open(std::vector<std::string>& hiddenSources) {
    m_hiddenSources   = &hiddenSources;
    m_open            = true;
    m_selected        = nullptr;
    m_selectedMonitor = nullptr;
    m_listIndex       = 0;

    // Enumerate and sort monitors left-to-right then top-to-bottom.
    m_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(this));
    std::sort(m_monitors.begin(), m_monitors.end(),
        [](const MonitorEntry& a, const MonitorEntry& b) {
            if (a.rc.left != b.rc.left) return a.rc.left < b.rc.left;
            return a.rc.top < b.rc.top;
        });
    // Build labels after sorting so numbers are positionally stable.
    for (int i = 0; i < (int)m_monitors.size(); ++i) {
        auto& m = m_monitors[i];
        int w = m.rc.right  - m.rc.left;
        int h = m.rc.bottom - m.rc.top;
        char buf[64];
        if (m.primary)
            sprintf_s(buf, "Display %d  %d\xc3\x97%d  (primary)", i + 1, w, h);
        else
            sprintf_s(buf, "Display %d  %d\xc3\x97%d", i + 1, w, h);
        m.label = buf;
    }

    Refresh();
}

void WindowPicker::Close() { m_open = false; }

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------
bool WindowPicker::Draw() {
    if (!m_open) return false;

    bool committed   = false;
    bool needRefresh = false;

    ImGui::OpenPopup("Select Source##picker");

    const float pickerW = 520.f, pickerH = 420.f;
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(
        ImVec2((ds.x - pickerW) * 0.5f, (ds.y - pickerH) * 0.5f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(pickerW, pickerH), ImGuiCond_Always);

    if (!ImGui::BeginPopupModal("Select Source##picker", nullptr,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        return false;

    ImGui::Text("Choose the source to capture:");
    ImGui::Separator();

    const bool   hasFilters    = m_hiddenSources && !m_hiddenSources->empty();
    const float  bottomReserve = hasFilters ? 130.f : 80.f;
    const int    monCount      = (int)m_monitors.size();
    const float  hideW         = 52.f;
    const float  spacing       = ImGui::GetStyle().ItemSpacing.x;

    // Helper: commit a flat-index selection.
    auto commitIndex = [&](int idx) {
        if (idx < monCount) {
            m_selectedMonitor = m_monitors[idx].hmon;
            m_selected        = nullptr;
        } else {
            int wi = idx - monCount;
            if (wi < (int)m_windows.size()) {
                m_selected        = m_windows[wi].hwnd;
                m_selectedMonitor = nullptr;
            } else {
                return; // nothing to select
            }
        }
        committed  = true;
        m_listIndex = idx;
        ImGui::CloseCurrentPopup();
        m_open = false;
    };

    ImGui::BeginChild("##sourcelist",
                       ImVec2(0.f, pickerH - bottomReserve - 60.f), true);

    // ---- Monitors ----
    if (!m_monitors.empty()) {
        ImGui::TextDisabled("  Displays");
        for (int i = 0; i < monCount; ++i) {
            const bool sel = (m_listIndex == i);
            ImGui::PushID(i);
            if (ImGui::Selectable(m_monitors[i].label.c_str(), sel,
                                   ImGuiSelectableFlags_AllowDoubleClick)) {
                m_listIndex = i;
                if (ImGui::IsMouseDoubleClicked(0))
                    commitIndex(i);
            }
            ImGui::PopID();
        }
        ImGui::Separator();
    }

    // ---- Windows ----
    if (!m_windows.empty())
        ImGui::TextDisabled("  Windows");

    for (int i = 0; i < (int)m_windows.size(); ++i) {
        const int  flatIdx = monCount + i;
        const bool sel     = (m_listIndex == flatIdx);
        const float titleW = ImGui::GetContentRegionAvail().x - hideW - spacing;

        ImGui::PushID(flatIdx);

        if (ImGui::Selectable(m_windows[i].title.c_str(), sel,
                               ImGuiSelectableFlags_AllowDoubleClick,
                               ImVec2(titleW, 0))) {
            m_listIndex = flatIdx;
            if (ImGui::IsMouseDoubleClicked(0))
                commitIndex(flatIdx);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Hide")) {
            if (m_hiddenSources) {
                std::string f = BaseTitle(m_windows[i].title);
                if (std::find(m_hiddenSources->begin(),
                              m_hiddenSources->end(), f)
                        == m_hiddenSources->end())
                    m_hiddenSources->push_back(f);
                needRefresh = true;
            }
        }

        ImGui::PopID();
    }

    ImGui::EndChild();

    // ---- Blocked filters ----
    if (hasFilters) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Blocked filters (click Remove to restore):");
        ImGui::BeginChild("##filters", ImVec2(0.f, 60.f), false);
        int toRemove = -1;
        for (int i = 0; i < (int)m_hiddenSources->size(); ++i) {
            ImGui::PushID(i + 10000);
            ImGui::BulletText("%s", (*m_hiddenSources)[i].c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) toRemove = i;
            ImGui::PopID();
        }
        if (toRemove >= 0) {
            m_hiddenSources->erase(m_hiddenSources->begin() + toRemove);
            needRefresh = true;
        }
        ImGui::EndChild();
    }

    // ---- Button row ----
    ImGui::Separator();
    const float btnW   = 90.f;
    const float totalW = btnW * 3.f + spacing * 2.f;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalW) * 0.5f);

    if (ImGui::Button("Refresh", ImVec2(btnW, 0))) needRefresh = true;
    ImGui::SameLine();
    if (ImGui::Button("OK", ImVec2(btnW, 0)))
        commitIndex(m_listIndex);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(btnW, 0))) {
        ImGui::CloseCurrentPopup();
        m_open = false;
    }

    if (needRefresh) Refresh();

    ImGui::EndPopup();
    return committed;
}
