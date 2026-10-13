#include "SettingsPanel.h"
#include "../settings/Presets.h"
#include "StyleFonts.h"
#include <imgui.h>
#include <algorithm> // std::clamp
#include <cfloat>    // FLT_MAX
#include <cctype>    // toupper / tolower
#include <cmath>
#include <cstring>   // strcmp
#include <ctime>     // teletext clock
#include <string>
#include <Windows.h>

// ---------------------------------------------------------------------------
// OSD style colours
// ---------------------------------------------------------------------------
static constexpr ImVec4 kCyan    { 0.00f, 0.85f, 1.00f, 1.00f };
static constexpr ImVec4 kAmber   { 1.00f, 0.78f, 0.00f, 1.00f };
static constexpr ImVec4 kDim     { 0.45f, 0.55f, 0.75f, 1.00f };
static constexpr ImVec4 kRed     { 0.55f, 0.05f, 0.05f, 1.00f };
static constexpr ImVec4 kRedH    { 0.80f, 0.10f, 0.10f, 1.00f };
static constexpr ImVec4 kRedA    { 1.00f, 0.20f, 0.20f, 1.00f };
static constexpr ImVec4 kSelBtn  { 0.00f, 0.40f, 0.80f, 1.00f }; // primary monitor
static constexpr ImVec4 kSelBtnH { 0.05f, 0.55f, 0.95f, 1.00f };
static constexpr ImVec4 kSelBtnA { 0.10f, 0.65f, 1.00f, 1.00f };
static constexpr ImVec4 kSecBtn  { 0.00f, 0.50f, 0.30f, 1.00f }; // secondary monitor
static constexpr ImVec4 kSecBtnH { 0.05f, 0.65f, 0.40f, 1.00f };
static constexpr ImVec4 kSecBtnA { 0.10f, 0.80f, 0.50f, 1.00f };

// Menu styles in the order the pickers list them.  `id` is the number saved
// as menuStyle in settings.json, so it never changes — a retired style just
// leaves a gap (2 was the old N64 style, replaced by 64 Adventure).
struct MenuStyleInfo { int id; const char* name; };
static constexpr MenuStyleInfo kStyles[] = {
    {  0, "CRT OSD" },        {  1, "Simple" },          {  3, "Camcorder" },
    {  4, "DOS Terminal" },   {  5, "VCR" },             {  6, "64 Adventure" },
    {  7, "Sword & Sorcery" }, {  8, "Teletext" },       {  9, "Vector Monitor" },
    { 10, "Arcade Service" },
};
static constexpr int kStyleCount = static_cast<int>(sizeof(kStyles) / sizeof(kStyles[0]));

// Name of the style with this id, or nullptr if there is none.
static const char* StyleName(int id) {
    for (const MenuStyleInfo& st : kStyles)
        if (st.id == id) return st.name;
    return nullptr;
}

static const char* kSectionNames[] = {
    "CAPTURE SOURCE", "PRESETS", "GEOMETRY", "SCANLINES",
    "GLOW", "HALATION", "VIGNETTE", "FLICKER", "STATIC",
    "SHADOW MASK", "COLOUR", "CURSOR", "STREAMING", "MENU STYLE"
};
static constexpr int kSectionCount = 14;
static constexpr int kMenuSection  = 13;   // style switcher + panel size
static constexpr int kPresetsSection = 1;

// Show a tooltip for the most recently drawn item, wrapped so wide fonts
// (e.g. the arcade pixel font) don't stretch it across the screen.
static void Tip(const char* text) {
    if (!ImGui::IsItemHovered()) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// ---------------------------------------------------------------------------
// Panel size — the user's menu scale / width / height, applied on top of each
// style's own layout.  Set by SettingsPanel::Draw for the frame.
// ---------------------------------------------------------------------------
static float s_menuScale  = 1.f;
static float s_menuWidth  = 1.f;
static float s_menuHeight = 1.f;
static bool  s_sizeSliderHeld     = false;  // a size slider was being dragged last frame
static bool  s_sizeSliderHeldNext = false;

static constexpr float kScreenMargin = 20.f;

// A fixed layout length (padding, sidebar width, font size, ...) at the menu scale.
static float Sc(float v) { return v * s_menuScale; }

// A style's base panel width / height at the menu scale and stretch, kept on screen.
static float PanelWidth(float base) {
    return std::min(base * s_menuScale * s_menuWidth,
                    ImGui::GetIO().DisplaySize.x - kScreenMargin * 2.f);
}
static float PanelHeight(float base) {
    return std::min(base * s_menuScale * s_menuHeight,
                    ImGui::GetIO().DisplaySize.y - kScreenMargin * 2.f);
}

// Top edge for a panel of height h: the style's preferred spot, moved up if
// the panel would run off the bottom of the screen.
static float PanelTop(float preferred, float h) {
    return std::max(kScreenMargin,
                    std::min(preferred, ImGui::GetIO().DisplaySize.y - kScreenMargin - h));
}

using Monitors = std::vector<MonitorPicker::MonitorEntry>;

// ---------------------------------------------------------------------------
// Helper: draw the monitor list inline.
//   [*] blue  = primary overlay      click: demote (if secondaries exist)
//   [+] green = secondary overlay    click: removeMonitor
//   [ ] plain = inactive             click: addMonitor
// ---------------------------------------------------------------------------
static void DrawMonitorList(const Monitors& monitors,
                             HMONITOR selectedMon,
                             const std::vector<HMONITOR>& secondaryMons,
                             SettingsPanel::DrawResult& result) {
    if (monitors.empty()) {
        ImGui::TextColored(kDim, "  No monitors detected");
        return;
    }
    const bool hasSecondaries = !secondaryMons.empty();
    for (const auto& m : monitors) {
        const bool isPrimary   = (m.hmon == selectedMon);
        const bool isSecondary = std::find(secondaryMons.begin(), secondaryMons.end(),
                                           m.hmon) != secondaryMons.end();
        char label[160];
        if (isPrimary) {
            ImGui::PushStyleColor(ImGuiCol_Button,        kSelBtn);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSelBtnH);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kSelBtnA);
            sprintf_s(label, "[*] %s", m.label.c_str());
        } else if (isSecondary) {
            ImGui::PushStyleColor(ImGuiCol_Button,        kSecBtn);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSecBtnH);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kSecBtnA);
            sprintf_s(label, "[+] %s", m.label.c_str());
        } else {
            sprintf_s(label, "[ ] %s", m.label.c_str());
        }

        if (ImGui::Button(label, ImVec2(-1.f, 0.f))) {
            if (isPrimary) {
                if (hasSecondaries)
                    result.removeMonitor = m.hmon; // promote a secondary, vacate this monitor
            } else if (isSecondary) {
                result.removeMonitor = m.hmon;
            } else {
                result.addMonitor = m.hmon;
            }
        }
        if (isPrimary) {
            Tip(hasSecondaries
                ? "Primary overlay — the settings panel appears here. Click to demote it."
                : "Primary overlay — add at least one other monitor to be able to disable this one.");
        } else if (isSecondary) {
            Tip("Active secondary overlay — click to remove the CRT filter from this monitor.");
        } else {
            Tip("Inactive display — click to add a CRT overlay on this monitor.");
        }

        if (isPrimary || isSecondary) ImGui::PopStyleColor(3);
    }
}

// ---------------------------------------------------------------------------
// Helper: combined preset dropdown + "+" (save) and "~" (manage) buttons.
// Returns true if the manage button was clicked this frame.
// The save-new-preset modal is rendered inline here.
// ---------------------------------------------------------------------------
static bool DrawPresetsUI(const std::vector<PresetItem>& presets,
                           SettingsPanel::DrawResult& result) {
    const float avail   = ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float bh      = ImGui::GetFrameHeight(); // square button side

    // Combined dropdown — all presets in display order.
    ImGui::SetNextItemWidth(avail - bh * 2.f - spacing * 2.f);
    if (ImGui::BeginCombo("##presetcombo", "-- Select Preset --")) {
        bool shownUserHeader = false;
        for (int i = 0; i < (int)presets.size(); i++) {
            const auto& p = presets[i];
            // Insert a visual divider the first time a user preset appears.
            if (!p.isBuiltIn && !shownUserHeader) {
                ImGui::Separator();
                ImGui::TextDisabled("  Saved");
                shownUserHeader = true;
            }
            if (ImGui::Selectable(p.name.c_str())) {
                result.applyPreset    = true;
                result.presetSettings = p.settings;
            }
        }
        ImGui::EndCombo();
    }
    Tip("Select a preset to instantly apply a saved group of settings.");

    // "+" — open save-new-preset modal.
    ImGui::SameLine();
    static bool s_openSave = false;
    if (ImGui::Button("+##savepre", ImVec2(bh, 0.f)))
        s_openSave = true;
    Tip("Save the current settings as a new named preset.");

    // "~" — open manage modal.
    ImGui::SameLine();
    bool openManage = ImGui::Button("~##manage", ImVec2(bh, 0.f));
    Tip("Reorder, rename, or delete saved presets.");

    // Save-new-preset modal.
    if (s_openSave) { ImGui::OpenPopup("##savepresetdlg"); s_openSave = false; }
    ImGui::SetNextWindowSize(ImVec2(Sc(300.f), 0.f), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("##savepresetdlg", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(kCyan, "Save new preset");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextUnformatted("Name:");
        static char s_nameBuf[128] = {};
        ImGui::SetNextItemWidth(-1.f);
        const bool commit = ImGui::InputText("##newprename", s_nameBuf, sizeof(s_nameBuf),
                                              ImGuiInputTextFlags_EnterReturnsTrue);

        // Built-in names are reserved — a user preset with one would be shadowed
        // by the built-in and become unreachable.  Re-using a user preset's
        // name is allowed and overwrites it.
        const bool clashesBuiltIn = Presets::IsBuiltIn(s_nameBuf);
        const bool overwrites = std::any_of(presets.begin(), presets.end(),
            [](const PresetItem& p) { return !p.isBuiltIn && p.name == s_nameBuf; });
        if (clashesBuiltIn)
            ImGui::TextColored(kRedA, "A built-in preset already uses this name.");
        else if (overwrites)
            ImGui::TextColored(kDim, "This will overwrite your existing preset.");
        const bool canSave = s_nameBuf[0] != '\0' && !clashesBuiltIn;

        ImGui::Spacing();
        const float hw = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
        if (!canSave) ImGui::BeginDisabled();
        const bool saveClicked = ImGui::Button("Save", ImVec2(hw, 0.f));
        if (!canSave) ImGui::EndDisabled();
        const bool doSave   = (commit || saveClicked) && canSave;
        ImGui::SameLine();
        const bool doCancel = ImGui::Button("Cancel", ImVec2(-1.f, 0.f));
        if (doSave) {
            result.savePreset     = true;
            result.savePresetName = s_nameBuf;
            s_nameBuf[0]          = '\0';
            ImGui::CloseCurrentPopup();
        }
        if (doCancel) {
            s_nameBuf[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    return openManage;
}

// ---------------------------------------------------------------------------
// Helper: manage modal — reorder all, rename/delete user presets.
// ---------------------------------------------------------------------------
static void DrawManageModal(const std::vector<PresetItem>& presets,
                             SettingsPanel::DrawResult& result) {
    using ManageOp = SettingsPanel::DrawResult::ManageOp;

    static int  s_renameIdx = -1;
    static char s_renameBuf[128] = {};

    ImGui::TextColored(kDim, "Arrows: reorder all.  Rename / delete: user presets only.");
    ImGui::Spacing();
    ImGui::Separator();

    for (int i = 0; i < (int)presets.size(); i++) {
        const auto& p = presets[i];
        ImGui::PushID(i);

        // Up arrow
        const bool canUp = (i > 0);
        if (!canUp) ImGui::BeginDisabled();
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
            result.manageOp  = ManageOp::MoveUp;
            result.manageIdx = i;
            s_renameIdx = -1;
        }
        if (!canUp) ImGui::EndDisabled();
        ImGui::SameLine();

        // Down arrow
        const bool canDown = (i < (int)presets.size() - 1);
        if (!canDown) ImGui::BeginDisabled();
        if (ImGui::ArrowButton("##dn", ImGuiDir_Down)) {
            result.manageOp  = ManageOp::MoveDown;
            result.manageIdx = i;
            s_renameIdx = -1;
        }
        if (!canDown) ImGui::EndDisabled();
        ImGui::SameLine();

        if (!p.isBuiltIn && s_renameIdx == i) {
            // Inline rename field
            ImGui::SetNextItemWidth(180.f);
            const bool commit = ImGui::InputText("##rn", s_renameBuf, sizeof(s_renameBuf),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            // Names must stay unique across built-in and user presets, or the
            // renamed preset gets shadowed and can't be reached any more.
            bool nameTaken = false;
            for (int j = 0; j < (int)presets.size(); j++)
                if (j != i && presets[j].name == s_renameBuf) { nameTaken = true; break; }
            const bool canRename = s_renameBuf[0] != '\0' && !nameTaken;

            ImGui::SameLine();
            if (!canRename) ImGui::BeginDisabled();
            const bool okClicked = ImGui::SmallButton("OK");
            if (!canRename) ImGui::EndDisabled();
            if ((commit || okClicked) && canRename) {
                result.manageOp   = ManageOp::Rename;
                result.manageIdx  = i;
                result.manageName = s_renameBuf;
                s_renameIdx = -1;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel")) s_renameIdx = -1;
            if (nameTaken) {
                ImGui::SameLine();
                ImGui::TextColored(kRedA, "name taken");
            }
        } else {
            if (p.isBuiltIn)
                ImGui::TextDisabled("%s  [built-in]", p.name.c_str());
            else
                ImGui::Text("%s", p.name.c_str());

            if (!p.isBuiltIn) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Rename")) {
                    s_renameIdx = i;
                    strncpy_s(s_renameBuf, p.name.c_str(), sizeof(s_renameBuf));
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button,        kRed);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRedH);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kRedA);
                if (ImGui::SmallButton("Del")) {
                    result.manageOp  = ManageOp::Delete;
                    result.manageIdx = i;
                    s_renameIdx = -1;
                }
                ImGui::PopStyleColor(3);
            }
        }

        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(-1.f, 0.f)))
        ImGui::CloseCurrentPopup();
}

// ---------------------------------------------------------------------------
// Helper: audio device combo box.
// ---------------------------------------------------------------------------
using AudioDevs = std::vector<SettingsPanel::AudioDevEntry>;
static void DrawAudioDeviceCombo(const AudioDevs& devs,
                                  const std::string& currentId,
                                  SettingsPanel::DrawResult& result) {
    const char* currentName = "No Audio";
    for (const auto& d : devs)
        if (d.id == currentId) { currentName = d.name.c_str(); break; }

    ImGui::TextUnformatted("Audio Source:");
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::BeginCombo("##audiodev", currentName)) {
        for (const auto& d : devs) {
            bool sel = (d.id == currentId);
            if (ImGui::Selectable(d.name.c_str(), sel)) {
                result.selectAudioDevice = true;
                result.audioDeviceId     = d.id;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    Tip("Select which audio output device to capture and stream to the stream window.");
}

// ---------------------------------------------------------------------------
// Shader controls shared by every style's sections.
// ---------------------------------------------------------------------------
static const char* kMaskTypeNames[] = {
    "Aperture Grille", "Shadow Mask", "Slot Mask", "Magenta / Green"
};
static constexpr int kMaskTypeCount = 4;
static constexpr const char* kMaskTypeTip =
    "Aperture Grille — vertical RGB stripes (Trinitron-style).\n"
    "Shadow Mask — dot triads, alternate rows offset.\n"
    "Slot Mask — stripes broken into staggered slots.\n"
    "Magenta / Green — 2-pixel stripes; the sharpest choice at 1080p.";

// GEOMETRY — after Convergence.
static void DrawGeometryExtras(CRTSettings& s) {
    ImGui::SliderFloat("Corners", &s.cornerRadius, 0.0f, 0.25f, "%.3f");
    Tip("Rounds the corners of the picture like a CRT tube, with a soft anti-aliased edge.");
    ImGui::SliderFloat("Raster Bloom", &s.rasterBloom, 0.0f, 1.0f, "%.2f");
    Tip("The picture swells slightly on bright scenes (up to 5%), like a CRT whose high-voltage\n"
        "supply sags under load.  Responds smoothly over a fraction of a second.");
}

// SCANLINES — after Count.
static void DrawBeamControls(CRTSettings& s) {
    ImGui::SliderFloat("Beam Width",    &s.beamWidth,    0.1f, 0.5f, "%.2f");
    Tip("Thickness of the electron beam for dark lines — lower gives deeper gaps between lines.");
    ImGui::SliderFloat("Beam Dynamics", &s.beamDynamics, 0.0f, 1.0f, "%.2f");
    Tip("How much brighter lines widen.  Real beams spread with brightness, so scanlines\n"
        "fade in bright areas and stay crisp in dark ones.");
    ImGui::SliderFloat("Resolve",       &s.lineResolve,  0.0f, 1.0f, "%.2f");
    Tip("How much each line takes on a single colour from its band of the picture, like a\n"
        "real low-resolution signal.  0 keeps full detail under the lines; 1 is most\n"
        "authentic but softens fine detail such as small text.  Set Count to the content's\n"
        "line count (e.g. 240 for retro consoles) for the best match.");
}

// SHADOW MASK — after Scale.
static void DrawMaskCompensation(CRTSettings& s) {
    ImGui::SliderFloat("Compensation##sm", &s.maskCompensation, 0.0f, 1.0f, "%.2f");
    Tip("Brightens lit phosphors to make up for the light the mask blocks.\n"
        "1 keeps average brightness; 0 lets the mask darken the picture.");
}

// GLOW — after Strength.
static void DrawGlowExtras(CRTSettings& s) {
    ImGui::SliderFloat("Radius##g", &s.glowRadius, 1.0f, 30.0f, "%.0f px");
    Tip("How far the glow spreads around bright areas (in pixels at 1080p; scales with resolution).");
    ImGui::SliderFloat("Persistence##g", &s.persistence, 0.0f, 1.0f, "%.2f");
    Tip("Phosphor afterglow — bright things leave a short fading trail as they move\n"
        "(up to 250 ms).  Still images are unaffected.");
}

// HALATION — after Threshold.
static void DrawHalationRadius(CRTSettings& s) {
    ImGui::SliderFloat("Radius##d", &s.halationRadius, 10.0f, 150.0f, "%.0f px");
    Tip("How far halation spreads — light scattering inside the tube glass, so it is wide and soft\n"
        "(in pixels at 1080p; scales with resolution).");
}

// COLOUR — after the gamma controls.
static void DrawSignalControls(CRTSettings& s) {
    ImGui::SliderFloat("Colour Bleed", &s.chromaBleed,    0.0f, 1.0f, "%.2f");
    Tip("Composite-video colour smear.  NTSC carries colour with far less detail than\n"
        "brightness, so colours bleed sideways while edges stay readable.");
    ImGui::SliderFloat("Softness",     &s.signalSoftness, 0.0f, 1.0f, "%.2f");
    Tip("Horizontal softness of the whole signal, like a limited-bandwidth analogue input.");
}

// COLOUR — after Saturation.
static void DrawGammaControls(CRTSettings& s) {
    ImGui::SliderFloat("CRT Gamma",     &s.crtGamma,     1.8f, 3.0f, "%.2f");
    Tip("How the simulated tube turns signal into light.  Real CRTs are around 2.4;\n"
        "higher deepens shadows and midtones.");
    ImGui::SliderFloat("Display Gamma", &s.displayGamma, 1.8f, 3.0f, "%.2f");
    Tip("Gamma used to encode the result for your monitor — 2.2 for most displays.");
}

// ---------------------------------------------------------------------------
// Custom cursor image controls — shared by every style's Cursor section.
// The cursor state is set by SettingsPanel::Draw for the duration of the call
// (threading it through every themed draw function would touch all of them).
// ---------------------------------------------------------------------------
static const SettingsPanel::CursorInfo* s_cursorInfo = nullptr;

static void DrawCustomCursorControls(CRTSettings& s, SettingsPanel::DrawResult& result) {
    if (!s_cursorInfo) return;
    const auto& ci = *s_cursorInfo;

    if (ci.custom)
        ImGui::Text("Image: %s", ci.name.c_str());
    else
        ImGui::TextUnformatted("Image: built-in");

    const bool configured = !ci.name.empty();
    const float spacing   = ImGui::GetStyle().ItemSpacing.x;
    const float bw = configured
        ? (ImGui::GetContentRegionAvail().x - spacing) * 0.5f
        : ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("Choose image...##cur", ImVec2(bw, 0.f)))
        result.pickCustomCursor = true;
    Tip("Use your own cursor image — PNG, CUR, ICO, BMP, GIF, JPG or TIFF, up to 1024 x 1024.\n"
        "It is scaled to match the built-in cursor; use Size to adjust.");
    if (configured) {
        ImGui::SameLine();
        if (ImGui::Button("Use default##cur", ImVec2(-1.f, 0.f)))
            result.clearCustomCursor = true;
        Tip("Go back to the built-in cursor.");
    }

    if (!ci.error.empty())
        ImGui::TextColored(kRedA, "%s", ci.error.c_str());

    ImGui::Checkbox("Show click point##cur", &s.cursorShowHotspot);
    Tip("Draws a small dot exactly where clicks land — handy for lining up\n"
        "a custom image's tip using the Hotspot X / Y sliders.");

    if (ci.custom && ci.texW > 0 && ci.texH > 0) {
        ImGui::SliderInt("Hotspot X##cur", &s.customCursorHotX, 0, ci.texW - 1);
        Tip("Horizontal position, in image pixels, of the point that clicks.");
        ImGui::SliderInt("Hotspot Y##cur", &s.customCursorHotY, 0, ci.texH - 1);
        Tip("Vertical position, in image pixels, of the point that clicks.");
    }
}

// Menu style — the style switcher.
static void DrawStyleCombo(CRTSettings& s) {
    if (ImGui::BeginCombo("Style##menu", StyleName(s.menuStyle))) {
        for (const MenuStyleInfo& st : kStyles)
            if (ImGui::Selectable(st.name, s.menuStyle == st.id))
                s.menuStyle = st.id;
        ImGui::EndCombo();
    }
    Tip("Choose the menu style.");
}

// Menu style — panel scale / width / height.  The panel resizes when a slider
// is released, so it doesn't move under the mouse mid-drag.
static void DrawMenuSizeControls(CRTSettings& s) {
    bool held = false;
    ImGui::SliderFloat("Scale##menu",  &s.menuScale,  0.5f, 2.0f, "%.2fx");
    held |= ImGui::IsItemActive();
    Tip("Size of the whole menu — text, spacing and controls.\nApplies when you let go of the slider.");
    ImGui::SliderFloat("Width##menu",  &s.menuWidth,  0.5f, 2.0f, "%.2fx");
    held |= ImGui::IsItemActive();
    Tip("Stretch the panel wider or narrower.\nApplies when you let go of the slider.");
    ImGui::SliderFloat("Height##menu", &s.menuHeight, 0.5f, 2.0f, "%.2fx");
    held |= ImGui::IsItemActive();
    Tip("Stretch the panel taller or shorter — for menus that shrink to fit, this is\n"
        "the most they grow to.  The panel scrolls if the settings don't fit.\n"
        "Applies when you let go of the slider.");
    if (held) s_sizeSliderHeldNext = true;
    if (ImGui::Button("Reset Size##menu")) {
        s.menuScale = s.menuWidth = s.menuHeight = 1.f;
    }
    Tip("Back to the style's normal size.");
}

// ---------------------------------------------------------------------------
// Shared settings body — called by every themed draw function.
// SecFn must be callable as void(const char* label).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Always-visible preset bar — call once per Draw function, inside Begin/End.
// ---------------------------------------------------------------------------
static void DrawTopPresets(const std::vector<PresetItem>& presets,
                           SettingsPanel::DrawResult& result) {
    static bool s_openManage = false;
    if (DrawPresetsUI(presets, result)) s_openManage = true;
    if (s_openManage) { ImGui::OpenPopup("##topManage"); s_openManage = false; }
    ImGui::SetNextWindowSize(ImVec2(Sc(460.f), 0.f), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("##topManage", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        DrawManageModal(presets, result);
        ImGui::EndPopup();
    }
}

template<typename SecFn>
static void DrawSettingsBody(
    CRTSettings& s,
    const Monitors& monitors, HMONITOR selectedMon,
    const std::vector<HMONITOR>& secondaryMons,
    bool streamOpen,
    const AudioDevs& audioDevices,
    SettingsPanel::DrawResult& result,
    SecFn Sec)
{
    Sec("CAPTURE SOURCE");
    DrawMonitorList(monitors, selectedMon, secondaryMons, result);

    Sec("GEOMETRY");
    ImGui::SliderFloat("Curvature X",  &s.curvatureX,       0.0f,  0.5f,   "%.2f");
    Tip("Horizontal barrel distortion — bows the image outward along X to simulate a curved CRT tube.");
    ImGui::SliderFloat("Curvature Y",  &s.curvatureY,       0.0f,  0.5f,   "%.2f");
    Tip("Vertical barrel distortion — bows the image outward along Y.");
    ImGui::SliderFloat("Zoom",         &s.zoom,             1.0f,  2.0f,   "%.2f");
    Tip("Scales the image inward to fill the black corners produced by barrel distortion.");
    ImGui::SliderFloat("Convergence",  &s.convergence,      0.0f,  1.0f,   "%.2f");
    Tip("RGB channel misalignment — simulates lateral chromatic aberration from misaligned electron guns.");
    DrawGeometryExtras(s);

    Sec("SCANLINES");
    ImGui::SliderFloat("Intensity",     &s.scanlineIntensity, 0.0f,  1.0f,   "%.2f");
    Tip("Strength of the scanline beam profile — 0 is a flat picture, 1 is fully separated lines.");
    ImGui::SliderFloat("Count",         &s.scanlineCount,     200.f, 1200.f, "%.0f");
    Tip("Number of lines across the picture height — 240 for retro consoles, 480 for DVD-era content.\n"
        "Scanlines fade out automatically if they get too fine for your monitor to show.");
    DrawBeamControls(s);
    ImGui::SliderFloat("Hum Intensity", &s.humBarIntensity,   0.0f,  1.0f,   "%.2f");
    Tip("Strength of rolling brightness bands simulating mains-frequency (50/60 Hz) interference.");
    ImGui::SliderFloat("Hum Speed",     &s.humBarSpeed,       0.0f,  2.0f,   "%.2f");
    Tip("Speed at which the hum bars scroll upward, in screen heights per second.");
    ImGui::SliderFloat("Hum Bands",     &s.humBarFrequency,   0.5f,  20.0f,  "%.1f");
    Tip("Number of hum bands visible on screen — higher values give tighter, more frequent banding.");

    Sec("GLOW");
    ImGui::SliderFloat("Strength##g",   &s.glowStrength,      0.0f,  2.0f,   "%.2f");
    Tip("Phosphor glow — bright areas bleed softly into their surroundings.");
    DrawGlowExtras(s);

    Sec("HALATION");
    ImGui::SliderFloat("Strength##d",   &s.diffuseStrength,   0.0f,  2.0f,   "%.2f");
    Tip("Wide, soft glow from light scattering in the tube glass — only areas brighter than the threshold.");
    ImGui::SliderFloat("Threshold##d",  &s.diffuseThreshold,  0.0f,  1.0f,   "%.2f");
    Tip("Brightness cutoff — areas dimmer than this do not cause halation.");
    DrawHalationRadius(s);

    Sec("VIGNETTE");
    ImGui::SliderFloat("Strength##v",   &s.vignetteStrength,  0.0f,  1.0f,   "%.2f");
    Tip("Radial edge darkening — simulates light falloff toward the corners of the CRT screen.");

    Sec("FLICKER");
    ImGui::SliderFloat("Intensity##f",  &s.flickerIntensity,  0.0f,  1.0f,  "%.2f");
    Tip("Amplitude of whole-frame brightness oscillation, simulating phosphor refresh instability.");
    ImGui::SliderFloat("Speed (Hz)",    &s.flickerSpeed,      1.0f, 30.0f,  "%.1f");
    Tip("Rate of brightness oscillation in cycles per second.");
    ImGui::SliderFloat("Randomness",    &s.flickerRandomness, 0.0f,  1.0f,  "%.2f");
    Tip("0 = smooth sinusoidal flicker; 1 = random step noise that snaps between values.");

    Sec("STATIC");
    ImGui::SliderFloat("Intensity##st", &s.staticIntensity,   0.0f,  1.0f,  "%.2f");
    Tip("Amount of random per-pixel grain overlaid on the image — simulates analogue TV noise.");
    ImGui::SliderFloat("Speed##st",     &s.staticSpeed,       1.0f, 60.0f,  "%.0f fps");
    Tip("How many times per second the grain pattern randomises — higher is more chaotic.");

    Sec("SHADOW MASK");
    {
        int t = (int)s.shadowMaskType;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::Combo("##masktype", &t, kMaskTypeNames, kMaskTypeCount))
            s.shadowMaskType = (float)t;
        Tip(kMaskTypeTip);
    }
    ImGui::SliderFloat("Intensity##sm", &s.shadowMaskIntensity, 0.0f, 1.0f, "%.2f");
    Tip("How strongly the sub-pixel pattern modulates the image — 0 is off, 1 is full mask.");
    ImGui::SliderFloat("Scale##sm",     &s.shadowMaskScale,     1.0f, 4.0f, "%.0f px");
    Tip("Screen pixels per phosphor stripe.  1 px matches the panel's own subpixels best.");
    DrawMaskCompensation(s);

    Sec("COLOUR");
    ImGui::SliderFloat("Brightness",   &s.brightness,       0.5f,  2.0f,   "%.2f");
    Tip("Overall image brightness multiplier — 1.0 is unchanged.");
    ImGui::SliderFloat("Contrast",     &s.contrast,         0.5f,  2.0f,   "%.2f");
    Tip("Contrast strength pivoted around mid-grey — above 1.0 increases light/dark separation.");
    ImGui::SliderFloat("Saturation",   &s.saturation,       0.0f,  2.0f,   "%.2f");
    Tip("Colour saturation — 0 is greyscale, 1.0 is original, 2.0 is double-saturated.");
    DrawGammaControls(s);
    DrawSignalControls(s);
    {
        float col[3] = { s.phosphorR, s.phosphorG, s.phosphorB };
        if (ImGui::ColorEdit3("Phosphor##ph", col,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueBar)) {
            s.phosphorR = col[0]; s.phosphorG = col[1]; s.phosphorB = col[2];
        }
        Tip("Tints the image to simulate a specific phosphor type — white (P22), green (P31), amber (P43).");
    }

    Sec("CURSOR");
    ImGui::SliderFloat("Size##cur",      &s.cursorScale,     0.1f, 2.0f,  "%.2f");
    Tip("Render scale of the virtual cursor relative to its source pixel size.");
    ImGui::SliderFloat("Thickness##cur", &s.cursorThickness, 0.0f, 4.0f,  "%.1f px");
    Tip("Dilation radius in pixels — thickens cursor edges for visibility against busy content.");
    {
        float col[3] = { s.cursorR, s.cursorG, s.cursorB };
        if (ImGui::ColorEdit3("Colour##cur", col,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueBar)) {
            s.cursorR = col[0]; s.cursorG = col[1]; s.cursorB = col[2];
        }
        Tip("Colour tint applied to the cursor — white leaves it unchanged.");
    }
    ImGui::Checkbox("Shadow##cur",    &s.cursorStroke);
    Tip("Draws a dark offset shadow behind the cursor for contrast against bright backgrounds.");
    ImGui::Checkbox("Hide when closed (shake to show)", &s.cursorAutoHide);
    Tip("Hides the cursor when the settings panel is closed; shake the mouse briefly to reveal it.");
    ImGui::Checkbox("Use filled cursor", &s.cursorPlain);
    Tip("Replaces the outline-style cursor with a solid filled shape.");
    DrawCustomCursorControls(s, result);

    Sec("STREAMING");
    DrawAudioDeviceCombo(audioDevices, s.audioCaptureDeviceId, result);
    ImGui::Spacing();
    if (!streamOpen) {
        if (ImGui::Button("[ OPEN STREAM WINDOW ]", ImVec2(-1.f, 0.f)))
            result.openStream = true;
        Tip("Opens a separate window for streaming or recording software to capture.");
    } else {
        float sbtnW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("[ STREAM SOURCE ]", ImVec2(sbtnW, 0.f)))
            result.openStreamPicker = true;
        Tip("Select which window or monitor the stream output window displays.");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button,        kRed);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRedH);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kRedA);
        if (ImGui::Button("[ CLOSE STREAM ]", ImVec2(sbtnW, 0.f)))
            result.closeStream = true;
        Tip("Closes the stream output window.");
        ImGui::PopStyleColor(3);
    }

    Sec("MENU STYLE");
    DrawStyleCombo(s);
    DrawMenuSizeControls(s);
}

// ---------------------------------------------------------------------------
// CRT OSD layout — centred panel, TV-menu look
// ---------------------------------------------------------------------------
static SettingsPanel::DrawResult DrawOSD(CRTSettings& s,
                                         const Monitors& monitors,
                                         HMONITOR selectedMon,
                                         const std::vector<HMONITOR>& secondaryMons,
                                         bool streamOpen,
                                         const std::vector<PresetItem>& presets,
                                         const AudioDevs& audioDevices) {
    ImVec2 display = ImGui::GetIO().DisplaySize;
    const float panelW = PanelWidth(500.f);
    const float maxH   = PanelHeight(display.y * 0.70f);
    ImGui::SetNextWindowPos(
        ImVec2((display.x - panelW) * 0.5f, PanelTop(display.y * 0.15f, maxH)), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(panelW, 0.f), ImVec2(panelW, maxH));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_AlwaysAutoResize;

    if (!ImGui::Begin("##OSD", nullptr, flags)) { ImGui::End(); return {}; }
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.80f);

    {
        const char* title = "CRT  PICTURE  SETTINGS";
        float tw = ImGui::CalcTextSize(title).x;
        ImGui::SetCursorPosX((panelW - tw) * 0.5f - ImGui::GetStyle().WindowPadding.x);
        ImGui::TextColored(kCyan, "%s", title);
    }
    ImGui::Separator();
    ImGui::Spacing();

    SettingsPanel::DrawResult result;
    DrawTopPresets(presets, result);
    ImGui::Spacing();

    auto Sec = [](const char* label) {
        ImGui::Spacing();
        ImGui::TextColored(kAmber, "- %s", label);
        ImGui::Separator();
    };

    DrawSettingsBody(s, monitors, selectedMon, secondaryMons,
                     streamOpen, audioDevices, result, Sec);

    ImGui::Spacing();
    ImGui::Separator();
    {
        const char* hint = "Ctrl+Shift+C = settings  |  Ctrl+Shift+Q = quit";
        float hw = ImGui::CalcTextSize(hint).x;
        ImGui::SetCursorPosX((panelW - hw) * 0.5f - ImGui::GetStyle().WindowPadding.x);
        ImGui::TextColored(kDim, "%s", hint);
    }
    ImGui::Spacing();
    {
        const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("[ CLOSE ]", ImVec2(bw, 0.f))) s.menuVisible = false;
        Tip("Hide the settings panel (Ctrl+Shift+C to re-open).");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button,        kRed);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRedH);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kRedA);
        if (ImGui::Button("[ EXIT ]", ImVec2(bw, 0.f))) PostQuitMessage(0);
        Tip("Quit the overlay application entirely.");
        ImGui::PopStyleColor(3);
    }

    ImGui::PopItemWidth();
    ImGui::End();
    return result;
}

// ---------------------------------------------------------------------------
// Per-section content — shared by all navigation-style draw functions.
// ---------------------------------------------------------------------------
static void DrawSectionContent(
    int sec,
    CRTSettings& s,
    const Monitors& monitors, HMONITOR selectedMon,
    const std::vector<HMONITOR>& secondaryMons,
    bool streamOpen,
    const std::vector<PresetItem>& presets,
    const AudioDevs& audioDevices,
    SettingsPanel::DrawResult& result)
{
    switch (sec) {
    case 0:
        DrawMonitorList(monitors, selectedMon, secondaryMons, result);
        break;
    case 1: {
        static bool s_openManage = false;
        if (DrawPresetsUI(presets, result)) s_openManage = true;
        if (s_openManage) { ImGui::OpenPopup("##manageNavPopup"); s_openManage = false; }
        ImGui::SetNextWindowSize(ImVec2(Sc(460.f), 0.f), ImGuiCond_Always);
        if (ImGui::BeginPopupModal("##manageNavPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            DrawManageModal(presets, result);
            ImGui::EndPopup();
        }
        break;
    }
    case 2:
        ImGui::SliderFloat("Curvature X",  &s.curvatureX,  0.0f, 0.5f, "%.2f");
        Tip("Horizontal barrel distortion.");
        ImGui::SliderFloat("Curvature Y",  &s.curvatureY,  0.0f, 0.5f, "%.2f");
        Tip("Vertical barrel distortion.");
        ImGui::SliderFloat("Zoom",         &s.zoom,        1.0f, 2.0f, "%.2f");
        Tip("Scale image inward to fill barrel-distortion corners.");
        ImGui::SliderFloat("Convergence",  &s.convergence, 0.0f, 1.0f, "%.2f");
        Tip("RGB channel misalignment — lateral chromatic aberration.");
        DrawGeometryExtras(s);
        break;
    case 3:
        ImGui::SliderFloat("Intensity",     &s.scanlineIntensity, 0.0f,    1.0f,   "%.2f");
        Tip("Scanline beam profile strength.");
        ImGui::SliderFloat("Count",         &s.scanlineCount,     200.f, 1200.f,   "%.0f");
        Tip("Lines across the picture height (240 = retro consoles).");
        DrawBeamControls(s);
        ImGui::SliderFloat("Hum Intensity", &s.humBarIntensity,   0.0f,    1.0f,   "%.2f");
        Tip("Rolling brightness band strength.");
        ImGui::SliderFloat("Hum Speed",     &s.humBarSpeed,       0.0f,    2.0f,   "%.2f");
        Tip("Hum bar scroll speed.");
        ImGui::SliderFloat("Hum Bands",     &s.humBarFrequency,   0.5f,   20.0f,   "%.1f");
        Tip("Number of hum bands visible on screen.");
        break;
    case 4:
        ImGui::SliderFloat("Strength##g", &s.glowStrength, 0.0f, 2.0f, "%.2f");
        Tip("Phosphor glow strength.");
        DrawGlowExtras(s);
        break;
    case 5:
        ImGui::SliderFloat("Strength##d",  &s.diffuseStrength,  0.0f, 2.0f, "%.2f");
        Tip("Halation strength — wide glow around bright areas.");
        ImGui::SliderFloat("Threshold##d", &s.diffuseThreshold, 0.0f, 1.0f, "%.2f");
        Tip("Brightness cutoff for halation.");
        DrawHalationRadius(s);
        break;
    case 6:
        ImGui::SliderFloat("Strength##v", &s.vignetteStrength, 0.0f, 1.0f, "%.2f");
        Tip("Radial edge darkening toward corners.");
        break;
    case 7:
        ImGui::SliderFloat("Intensity##f",  &s.flickerIntensity,  0.0f,  1.0f, "%.2f");
        Tip("Brightness oscillation amplitude.");
        ImGui::SliderFloat("Speed (Hz)",    &s.flickerSpeed,      1.0f, 30.0f, "%.1f");
        Tip("Oscillation cycles per second.");
        ImGui::SliderFloat("Randomness",    &s.flickerRandomness, 0.0f,  1.0f, "%.2f");
        Tip("0 = smooth sine, 1 = random noise.");
        break;
    case 8:
        ImGui::SliderFloat("Intensity##st", &s.staticIntensity, 0.0f,  1.0f,  "%.2f");
        Tip("Per-pixel grain amount.");
        ImGui::SliderFloat("Speed##st",     &s.staticSpeed,     1.0f, 60.0f, "%.0f fps");
        Tip("Grain refresh rate.");
        break;
    case 9: {
        int t = (int)s.shadowMaskType;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::Combo("##masktype", &t, kMaskTypeNames, kMaskTypeCount)) s.shadowMaskType = (float)t;
        Tip(kMaskTypeTip);
        ImGui::SliderFloat("Intensity##sm", &s.shadowMaskIntensity, 0.0f, 1.0f, "%.2f");
        Tip("Sub-pixel pattern strength.");
        ImGui::SliderFloat("Scale##sm",     &s.shadowMaskScale,     1.0f, 4.0f, "%.0f px");
        Tip("Screen pixels per phosphor stripe.  1 px matches the panel's own subpixels best.");
        DrawMaskCompensation(s);
        break;
    }
    case 10: {
        ImGui::SliderFloat("Brightness", &s.brightness, 0.5f, 2.0f, "%.2f");
        Tip("Image brightness multiplier.");
        ImGui::SliderFloat("Contrast",   &s.contrast,   0.5f, 2.0f, "%.2f");
        Tip("Light/dark separation.");
        ImGui::SliderFloat("Saturation", &s.saturation, 0.0f, 2.0f, "%.2f");
        Tip("Colour saturation.");
        DrawGammaControls(s);
        DrawSignalControls(s);
        float col[3] = { s.phosphorR, s.phosphorG, s.phosphorB };
        if (ImGui::ColorEdit3("Phosphor##ph", col,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueBar)) {
            s.phosphorR = col[0]; s.phosphorG = col[1]; s.phosphorB = col[2];
        }
        Tip("Phosphor tint colour.");
        break;
    }
    case 11: {
        ImGui::SliderFloat("Size##cur",      &s.cursorScale,     0.1f, 2.0f, "%.2f");
        Tip("Cursor render scale.");
        ImGui::SliderFloat("Thickness##cur", &s.cursorThickness, 0.0f, 4.0f, "%.1f px");
        Tip("Cursor dilation radius.");
        float col[3] = { s.cursorR, s.cursorG, s.cursorB };
        if (ImGui::ColorEdit3("Colour##cur", col,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueBar)) {
            s.cursorR = col[0]; s.cursorG = col[1]; s.cursorB = col[2];
        }
        Tip("Cursor tint colour.");
        ImGui::Checkbox("Shadow##cur",    &s.cursorStroke);
        Tip("Dark offset shadow behind cursor.");
        ImGui::Checkbox("Hide when closed (shake to show)", &s.cursorAutoHide);
        Tip("Auto-hide cursor when panel is closed.");
        ImGui::Checkbox("Use filled cursor", &s.cursorPlain);
        Tip("Solid filled cursor shape.");
        DrawCustomCursorControls(s, result);
        break;
    }
    case 12:
        DrawAudioDeviceCombo(audioDevices, s.audioCaptureDeviceId, result);
        ImGui::Spacing();
        if (!streamOpen) {
            if (ImGui::Button("[ OPEN STREAM WINDOW ]", ImVec2(-1.f, 0.f)))
                result.openStream = true;
            Tip("Opens a separate window for streaming software.");
        } else {
            float sbtnW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button("[ STREAM SOURCE ]", ImVec2(sbtnW, 0.f)))
                result.openStreamPicker = true;
            Tip("Select the stream output source.");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,        kRed);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRedH);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kRedA);
            if (ImGui::Button("[ CLOSE STREAM ]", ImVec2(sbtnW, 0.f)))
                result.closeStream = true;
            Tip("Close the stream window.");
            ImGui::PopStyleColor(3);
        }
        break;
    case kMenuSection:
        DrawStyleCombo(s);
        DrawMenuSizeControls(s);
        break;
    }
}

// ---------------------------------------------------------------------------
// Camcorder layout — Chakra Petch font, full-screen header/footer bars,
//                    half-height panel, 2-panel on section click
// ---------------------------------------------------------------------------
static SettingsPanel::DrawResult DrawCamcorder(CRTSettings& s,
                                                const Monitors& monitors,
                                                HMONITOR selectedMon,
                                                const std::vector<HMONITOR>& secondaryMons,
                                                bool streamOpen,
                                                const std::vector<PresetItem>& presets,
                                                const AudioDevs& audioDevices) {
    static int s_section = -1;
    SettingsPanel::DrawResult result;

    if (StyleFonts::chakraPetch) ImGui::PushFont(StyleFonts::chakraPetch);

    ImVec2 display  = ImGui::GetIO().DisplaySize;
    const float kBarH   = Sc(50.f);
    const float kBarClr = 1.0f; // opaque bar
    const ImVec4 kBarBg = { 0.18f, 0.03f, 0.02f, kBarClr };

    ImGuiWindowFlags barFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar  | ImGuiWindowFlags_NoScrollWithMouse;

    // ---- Header bar — full screen width ----
    ImGui::SetNextWindowPos({ 0.f, 0.f }, ImGuiCond_Always);
    ImGui::SetNextWindowSize({ display.x, kBarH }, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBarBg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Sc(16.f), Sc(10.f)));
    ImGui::Begin("##cam_hdr", nullptr, barFlags);
    {
        ImGui::TextColored({ 1.f, 1.f, 1.f, 1.f }, "\u25a0 MENU");
        ImGui::SameLine();
        ImGui::TextColored({ 1.f, 0.12f, 0.06f, 1.f }, "\u25cf REC");
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // ---- Footer bar — full screen width ----
    ImGui::SetNextWindowPos({ 0.f, display.y - kBarH }, ImGuiCond_Always);
    ImGui::SetNextWindowSize({ display.x, kBarH }, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBarBg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Sc(16.f), Sc(10.f)));
    ImGui::Begin("##cam_ftr", nullptr, barFlags);
    {
        ImGui::TextColored({ 0.55f, 0.55f, 0.55f, 1.f },
                           "\u25c4\u25ba Navigate  \u25cf Select  MENU Close");
        ImGui::SameLine();
        const float btnW = Sc(80.f);
        const float sp   = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(display.x - (btnW * 2.f + sp) - ImGui::GetStyle().WindowPadding.x * 2.f);
        if (ImGui::Button("CLOSE", ImVec2(btnW, 0.f))) s.menuVisible = false;
        Tip("Hide the settings panel.");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button,        kRed);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRedH);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kRedA);
        if (ImGui::Button("QUIT", ImVec2(btnW, 0.f))) PostQuitMessage(0);
        Tip("Quit the overlay.");
        ImGui::PopStyleColor(3);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // ---- Main panel ----
    const bool   twoPanel = (s_section >= 0);
    const float  panelW   = PanelWidth(twoPanel ? 720.f : 460.f);
    const float  panelH   = std::min(PanelHeight(display.y * 0.35f),
                                     display.y - kBarH * 2.f - kScreenMargin * 2.f);
    const float  panelX   = (display.x - panelW) * 0.5f;
    const float  panelY   = kBarH + (display.y - kBarH * 2.f - panelH) * 0.5f;

    ImGui::SetNextWindowPos({ panelX, panelY }, ImGuiCond_Always);
    ImGui::SetNextWindowSize({ panelW, panelH }, ImGuiCond_Always);

    ImGuiWindowFlags panelFlags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (!ImGui::Begin("##CAM", nullptr, panelFlags)) {
        ImGui::End();
        if (StyleFonts::chakraPetch) ImGui::PopFont();
        return {};
    }

    DrawTopPresets(presets, result);
    ImGui::Spacing();

    if (!twoPanel) {
        // Single panel — numbered section list
        if (ImGui::BeginChild("##cam_list", ImVec2(0.f, 0.f), false)) {
            for (int i = 0; i < kSectionCount; ++i) {
                char label[80];
                snprintf(label, sizeof(label), "%2d.  %s", i + 1, kSectionNames[i]);
                if (ImGui::Selectable(label, false)) s_section = i;
            }
        }
        ImGui::EndChild();
    } else {
        // Two-panel — left sidebar + right sliders
        const float sp     = ImGui::GetStyle().ItemSpacing.x;
        const float padX   = ImGui::GetStyle().WindowPadding.x;
        const float leftW  = Sc(185.f);
        const float rightW = panelW - leftW - sp - padX * 2.f;

        ImGui::BeginChild("##cam_left", ImVec2(leftW, -1.f), true);
        for (int i = 0; i < kSectionCount; ++i) {
            char label[64]; snprintf(label, sizeof(label), "%s", kSectionNames[i]);
            if (ImGui::Selectable(label, s_section == i)) s_section = i;
        }
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("##cam_right", ImVec2(rightW, -1.f), false);
        ImGui::PushItemWidth(rightW * 0.78f);
        DrawSectionContent(s_section, s, monitors, selectedMon, secondaryMons,
                           streamOpen, presets, audioDevices, result);
        ImGui::PopItemWidth();
        ImGui::EndChild();
    }

    ImGui::End();
    if (StyleFonts::chakraPetch) ImGui::PopFont();
    return result;
}

// ---------------------------------------------------------------------------
// DOS Terminal layout — green-on-black, numbered list navigation menu
// ---------------------------------------------------------------------------
static SettingsPanel::DrawResult DrawDOS(CRTSettings& s,
                                          const Monitors& monitors,
                                          HMONITOR selectedMon,
                                          const std::vector<HMONITOR>& secondaryMons,
                                          bool streamOpen,
                                          const std::vector<PresetItem>& presets,
                                          const AudioDevs& audioDevices) {
    static int s_section = -1;
    SettingsPanel::DrawResult result;

    ImVec2 display = ImGui::GetIO().DisplaySize;
    const float panelW = PanelWidth(500.f);
    const float maxH   = PanelHeight(display.y * 0.42f);  // 60% of the old 70%
    ImGui::SetNextWindowPos(
        ImVec2((display.x - panelW) * 0.5f, PanelTop(display.y * 0.15f, maxH)), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, maxH), ImGuiCond_Always);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (!ImGui::Begin("##DOS", nullptr, flags)) { ImGui::End(); return {}; }

    const float footerH  = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    const ImVec4 kGreen  = { 0.00f, 1.00f, 0.00f, 1.00f };

    // Persistent DOS prompt header
    if (s_section < 0) {
        ImGui::TextColored(kGreen, "C:\\> SETTINGS.EXE");
    } else {
        char path[80];
        snprintf(path, sizeof(path), "C:\\SETTINGS\\%s", kSectionNames[s_section]);
        ImGui::TextColored(kGreen, "%s", path);
    }
    ImGui::Separator();
    ImGui::Spacing();

    if (s_section < 0) {
        // ---- Main menu ----
        DrawTopPresets(presets, result);
        ImGui::Spacing();

        if (ImGui::BeginChild("##dos_list", ImVec2(0.f, -footerH), false)) {
            for (int i = 0; i < kSectionCount; ++i) {
                char label[80];
                snprintf(label, sizeof(label), " [%2d]  %s", i + 1, kSectionNames[i]);
                ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.00f, 0.30f, 0.00f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.00f, 0.50f, 0.00f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.00f, 0.80f, 0.00f, 1.f));
                if (ImGui::Selectable(label, false)) s_section = i;
                ImGui::PopStyleColor(3);
            }
        }
        ImGui::EndChild();

        float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.00f, 0.22f, 0.00f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 0.38f, 0.00f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.00f, 0.70f, 0.00f, 1.f));
        if (ImGui::Button("[ESC] CLOSE", ImVec2(bw, 0.f))) s.menuVisible = false;
        Tip("Hide the settings panel.");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.00f, 0.12f, 0.00f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 0.22f, 0.00f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.00f, 0.50f, 0.00f, 1.f));
        if (ImGui::Button("[F10] QUIT",  ImVec2(bw, 0.f))) PostQuitMessage(0);
        Tip("Quit the overlay.");
        ImGui::PopStyleColor(6);
    } else {
        // ---- Section view — BACK is inline, right after the last slider ----
        if (ImGui::BeginChild("##dos_sec", ImVec2(0.f, 0.f), false)) {
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.80f);
            DrawSectionContent(s_section, s, monitors, selectedMon, secondaryMons,
                               streamOpen, presets, audioDevices, result);
            ImGui::PopItemWidth();
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.00f, 0.22f, 0.00f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 0.38f, 0.00f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.00f, 0.70f, 0.00f, 1.f));
            if (ImGui::Button("[ESC] BACK", ImVec2(-1.f, 0.f))) s_section = -1;
            Tip("Return to the main settings menu.");
            ImGui::PopStyleColor(3);
        }
        ImGui::EndChild();
    }

    ImGui::End();
    return result;
}

// ---------------------------------------------------------------------------
// VCR layout — near-black, white text, single-open accordion
// ---------------------------------------------------------------------------
static SettingsPanel::DrawResult DrawVCR(CRTSettings& s,
                                          const Monitors& monitors,
                                          HMONITOR selectedMon,
                                          const std::vector<HMONITOR>& secondaryMons,
                                          bool streamOpen,
                                          const std::vector<PresetItem>& presets,
                                          const AudioDevs& audioDevices) {
    static int s_open = -1;  // index of the one expanded section, -1 = all closed
    SettingsPanel::DrawResult result;

    ImVec2 display = ImGui::GetIO().DisplaySize;
    const float panelW = PanelWidth(580.f);
    const float maxH   = PanelHeight(display.y * 0.70f);
    ImGui::SetNextWindowPos(
        ImVec2((display.x - panelW) * 0.5f, PanelTop(display.y * 0.15f, maxH)), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(panelW, 80.f), ImVec2(panelW, maxH));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_AlwaysAutoResize;

    if (!ImGui::Begin("##VCR", nullptr, flags)) { ImGui::End(); return {}; }

    // Centered title — shows current open section or "MENU"
    {
        const char* title = (s_open < 0) ? "MENU" : kSectionNames[s_open];
        float tw = ImGui::CalcTextSize(title).x;
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + (avail - tw) * 0.5f);
        ImGui::Text("%s", title);
    }
    ImGui::Separator();
    ImGui::Separator();
    ImGui::Spacing();

    DrawTopPresets(presets, result);
    ImGui::Spacing();

    // Single-open accordion list
    for (int i = 0; i < kSectionCount; ++i) {
        ImGui::PushID(i);
        ImGui::Separator();

        char label[80];
        snprintf(label, sizeof(label), "  %s", kSectionNames[i]);
        bool isOpen = (s_open == i);
        if (ImGui::Selectable(label, isOpen))
            s_open = isOpen ? -1 : i;  // same item → close; different → close old, open new

        if (isOpen) {
            ImGui::Indent(Sc(14.f));
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.58f);
            DrawSectionContent(i, s, monitors, selectedMon, secondaryMons,
                               streamOpen, presets, audioDevices, result);
            ImGui::PopItemWidth();
            ImGui::Unindent(Sc(14.f));
            ImGui::Spacing();
        }

        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::Separator();
    ImGui::Spacing();

    // Footer buttons
    float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("CLOSE", ImVec2(bw, 0.f))) s.menuVisible = false;
    Tip("Hide the settings panel.");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.30f, 0.06f, 0.04f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.50f, 0.08f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.80f, 0.12f, 0.08f, 1.f));
    if (ImGui::Button("QUIT", ImVec2(bw, 0.f))) PostQuitMessage(0);
    Tip("Quit the overlay.");
    ImGui::PopStyleColor(3);
    ImGui::Spacing();

    ImGui::End();
    return result;
}

// ---------------------------------------------------------------------------
// Default layout — top-left panel, collapsing headers, standard ImGui look
// ---------------------------------------------------------------------------
static SettingsPanel::DrawResult DrawDefault(CRTSettings& s,
                                              const Monitors& monitors,
                                              HMONITOR selectedMon,
                                              const std::vector<HMONITOR>& secondaryMons,
                                              bool streamOpen,
                                              const std::vector<PresetItem>& presets,
                                              const AudioDevs& audioDevices) {
    SettingsPanel::DrawResult result;

    ImVec2 display = ImGui::GetIO().DisplaySize;
    const float panelW = PanelWidth(500.f);
    const float maxH   = PanelHeight(display.y * 0.70f);
    ImGui::SetNextWindowPos(
        ImVec2((display.x - panelW) * 0.5f, PanelTop(display.y * 0.15f, maxH)),
        ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(panelW, 0.f),
        ImVec2(panelW, maxH));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoMove          |
        ImGuiWindowFlags_NoResize        |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize;

    if (!ImGui::Begin("CRT Settings", nullptr, flags)) { ImGui::End(); return result; }
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * (80.f / 100.f));

    DrawTopPresets(presets, result);

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Capture Source", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawMonitorList(monitors, selectedMon, secondaryMons, result);
    }

    if (ImGui::CollapsingHeader("Geometry", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Curvature X", &s.curvatureX,  0.0f, 0.5f, "%.2f");
        Tip("Horizontal barrel distortion — bows the image outward along X to simulate a curved CRT tube.");
        ImGui::SliderFloat("Curvature Y", &s.curvatureY,  0.0f, 0.5f, "%.2f");
        Tip("Vertical barrel distortion — bows the image outward along Y.");
        ImGui::SliderFloat("Zoom",        &s.zoom,        1.0f, 2.0f, "%.2f");
        Tip("Scales the image inward to fill the black corners produced by barrel distortion.");
        ImGui::SliderFloat("Convergence", &s.convergence, 0.0f, 1.0f, "%.2f");
        Tip("RGB channel misalignment — simulates lateral chromatic aberration from misaligned electron guns.");
        DrawGeometryExtras(s);
    }
    if (ImGui::CollapsingHeader("Scanlines", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Intensity",     &s.scanlineIntensity, 0.0f,  1.0f,   "%.2f");
        Tip("Strength of the scanline beam profile — 0 is a flat picture, 1 is fully separated lines.");
        ImGui::SliderFloat("Count",         &s.scanlineCount,     200.f, 1200.f, "%.0f");
        Tip("Number of lines across the picture height — 240 for retro consoles, 480 for DVD-era content.\n"
            "Scanlines fade out automatically if they get too fine for your monitor to show.");
        DrawBeamControls(s);
        ImGui::SliderFloat("Hum Intensity", &s.humBarIntensity,   0.0f,  1.0f,   "%.2f");
        Tip("Strength of rolling brightness bands simulating mains-frequency (50/60 Hz) interference.");
        ImGui::SliderFloat("Hum Speed",     &s.humBarSpeed,       0.0f,  2.0f,   "%.2f");
        Tip("Speed at which the hum bars scroll upward, in screen heights per second.");
        ImGui::SliderFloat("Hum Bands",     &s.humBarFrequency,   0.5f, 20.0f,   "%.1f");
        Tip("Number of hum bands visible on screen — higher values give tighter, more frequent banding.");
    }
    if (ImGui::CollapsingHeader("Glow", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Strength##g",  &s.glowStrength,     0.0f, 2.0f, "%.2f");
        Tip("Phosphor glow — bright areas bleed softly into their surroundings.");
        DrawGlowExtras(s);
    }
    if (ImGui::CollapsingHeader("Halation", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Strength##d",  &s.diffuseStrength,  0.0f, 2.0f, "%.2f");
        Tip("Wide, soft glow from light scattering in the tube glass — only areas brighter than the threshold.");
        ImGui::SliderFloat("Threshold##d", &s.diffuseThreshold, 0.0f, 1.0f, "%.2f");
        Tip("Brightness cutoff — areas dimmer than this do not cause halation.");
        DrawHalationRadius(s);
    }
    if (ImGui::CollapsingHeader("Vignette", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Strength##v", &s.vignetteStrength, 0.0f, 1.0f, "%.2f");
        Tip("Radial edge darkening — simulates light falloff toward the corners of the CRT screen.");
    }
    if (ImGui::CollapsingHeader("Flicker", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Intensity##f", &s.flickerIntensity,  0.0f,  1.0f,  "%.2f");
        Tip("Amplitude of whole-frame brightness oscillation, simulating phosphor refresh instability.");
        ImGui::SliderFloat("Speed (Hz)",   &s.flickerSpeed,      1.0f, 30.0f,  "%.1f");
        Tip("Rate of brightness oscillation in cycles per second.");
        ImGui::SliderFloat("Randomness",   &s.flickerRandomness, 0.0f,  1.0f,  "%.2f");
        Tip("0 = smooth sinusoidal flicker; 1 = random step noise that snaps between values.");
    }
    if (ImGui::CollapsingHeader("Static", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Intensity##st", &s.staticIntensity,  0.0f,  1.0f,  "%.2f");
        Tip("Amount of random per-pixel grain overlaid on the image — simulates analogue TV noise.");
        ImGui::SliderFloat("Speed##st",     &s.staticSpeed,      1.0f, 60.0f,  "%.0f fps");
        Tip("How many times per second the grain pattern randomises — higher is more chaotic.");
    }
    if (ImGui::CollapsingHeader("Shadow Mask", ImGuiTreeNodeFlags_DefaultOpen)) {
        int t = (int)s.shadowMaskType;
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::Combo("Pattern##sm", &t, kMaskTypeNames, kMaskTypeCount))
            s.shadowMaskType = (float)t;
        Tip(kMaskTypeTip);
        ImGui::SliderFloat("Intensity##sm", &s.shadowMaskIntensity, 0.0f, 1.0f, "%.2f");
        Tip("How strongly the sub-pixel pattern modulates the image — 0 is off, 1 is full mask.");
        ImGui::SliderFloat("Scale##sm",     &s.shadowMaskScale,     1.0f, 4.0f, "%.0f px");
        Tip("Screen pixels per phosphor stripe.  1 px matches the panel's own subpixels best.");
        DrawMaskCompensation(s);
    }
    if (ImGui::CollapsingHeader("Colour", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Brightness",  &s.brightness, 0.5f, 2.0f, "%.2f");
        Tip("Overall image brightness multiplier — 1.0 is unchanged.");
        ImGui::SliderFloat("Contrast",    &s.contrast,   0.5f, 2.0f, "%.2f");
        Tip("Contrast strength pivoted around mid-grey — above 1.0 increases light/dark separation.");
        ImGui::SliderFloat("Saturation",  &s.saturation, 0.0f, 2.0f, "%.2f");
        Tip("Colour saturation — 0 is greyscale, 1.0 is original, 2.0 is double-saturated.");
        DrawGammaControls(s);
        DrawSignalControls(s);
        {
            float col[3] = { s.phosphorR, s.phosphorG, s.phosphorB };
            if (ImGui::ColorEdit3("Phosphor##ph", col,
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueBar)) {
                s.phosphorR = col[0]; s.phosphorG = col[1]; s.phosphorB = col[2];
            }
            Tip("Tints the image to simulate a specific phosphor type — white (P22), green (P31), amber (P43).");
        }
    }

    if (ImGui::CollapsingHeader("Cursor", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Size##cur",      &s.cursorScale,     0.1f, 2.0f,  "%.2f");
        Tip("Render scale of the virtual cursor relative to its source pixel size.");
        ImGui::SliderFloat("Thickness##cur", &s.cursorThickness, 0.0f, 4.0f,  "%.1f px");
        Tip("Dilation radius in pixels — thickens cursor edges for visibility against busy content.");
        {
            float col[3] = { s.cursorR, s.cursorG, s.cursorB };
            if (ImGui::ColorEdit3("Colour##cur", col,
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueBar)) {
                s.cursorR = col[0]; s.cursorG = col[1]; s.cursorB = col[2];
            }
            Tip("Colour tint applied to the cursor — white leaves it unchanged.");
        }
        ImGui::Checkbox("Shadow##cur",    &s.cursorStroke);
        Tip("Draws a dark offset shadow behind the cursor for contrast against bright backgrounds.");
        ImGui::Checkbox("Hide when closed (shake to show)", &s.cursorAutoHide);
        Tip("Hides the cursor when the settings panel is closed; shake the mouse briefly to reveal it.");
        ImGui::Checkbox("Use filled cursor", &s.cursorPlain);
        Tip("Replaces the outline-style cursor with a solid filled shape.");
        DrawCustomCursorControls(s, result);
    }

    // Streaming
    if (ImGui::CollapsingHeader("Streaming", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawAudioDeviceCombo(audioDevices, s.audioCaptureDeviceId, result);
        ImGui::Spacing();
        if (!streamOpen) {
            if (ImGui::Button("Open Stream Window", ImVec2(-1.f, 0.f)))
                result.openStream = true;
            Tip("Opens a separate window for streaming or recording software to capture.");
        } else {
            if (ImGui::Button("Select Stream Source...", ImVec2(-1.f, 0.f)))
                result.openStreamPicker = true;
            Tip("Select which window or monitor the stream output window displays.");
            ImGui::PushStyleColor(ImGuiCol_Button,        kRed);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRedH);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kRedA);
            if (ImGui::Button("Close Stream Window", ImVec2(-1.f, 0.f)))
                result.closeStream = true;
            Tip("Closes the stream output window.");
            ImGui::PopStyleColor(3);
        }
    }

    if (ImGui::CollapsingHeader("Menu Style", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawStyleCombo(s);
        DrawMenuSizeControls(s);
    }

    ImGui::Separator();
    ImGui::TextColored(kDim, "Ctrl+Shift+C = settings | Ctrl+Shift+Q = quit");
    ImGui::Separator();
    {
        const float bw = (ImGui::GetContentRegionAvail().x
                          - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Close", ImVec2(bw, 0.f)))
            s.menuVisible = false;
        Tip("Hide the settings panel (Ctrl+Shift+C to re-open).");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button,        kRed);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRedH);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kRedA);
        if (ImGui::Button("Exit App", ImVec2(bw, 0.f)))
            PostQuitMessage(0);
        Tip("Quit the overlay application entirely.");
        ImGui::PopStyleColor(3);
    }

    ImGui::PopItemWidth();
    ImGui::End();
    return result;
}

// ---------------------------------------------------------------------------
// Drawing helpers shared by the game-style menus.
// ---------------------------------------------------------------------------
// Scales a colour's RGB by k (clamped), keeping alpha.
static ImU32 Shade(ImU32 c, float k) {
    auto ch = [&](int shift) {
        const int v = static_cast<int>(((c >> shift) & 0xFF) * k);
        return static_cast<ImU32>(std::clamp(v, 0, 255)) << shift;
    };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | (c & IM_COL32_A_MASK);
}

// Replaces a colour's alpha.
static ImU32 WithAlpha(ImU32 c, float a) {
    return (c & ~IM_COL32_A_MASK) |
           (static_cast<ImU32>(std::clamp(a, 0.f, 1.f) * 255.f + 0.5f) << IM_COL32_A_SHIFT);
}

// Blends two colours (alpha included); t in 0–1.
static ImU32 Mix(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    auto ch = [&](int shift) {
        const float x = static_cast<float>((a >> shift) & 0xFF);
        const float y = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<ImU32>(x + (y - x) * t + 0.5f) << shift;
    };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | ch(IM_COL32_A_SHIFT);
}

// Top-to-bottom gradient colour at height y, with an optional middle stop.
static ImU32 VGrad(float y, float y0, float y1, ImU32 top, ImU32 bottom) {
    return Mix(top, bottom, (y - y0) / std::max(1.f, y1 - y0));
}
static ImU32 VGrad3(float y, float y0, float y1, ImU32 top, ImU32 mid, ImU32 bottom) {
    const float k = (y - y0) / std::max(1.f, y1 - y0);
    return k < 0.5f ? Mix(top, mid, k * 2.f) : Mix(mid, bottom, k * 2.f - 1.f);
}

// Recolours the vertices added to dl since vtxStart with fn(pos).  Each
// vertex keeps its own alpha, so anti-aliased edges stay soft.
template <typename Fn>
static void ShadeVerts(ImDrawList* dl, int vtxStart, Fn fn) {
    for (int i = vtxStart; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        const ImU32 c = fn(v.pos);
        const float a = ((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.f *
                        ((c     >> IM_COL32_A_SHIFT) & 0xFF) / 255.f;
        v.col = WithAlpha(c, a);
    }
}

// Deterministic hash in [0, 1).
static float Hash(float x, float y) {
    const float h = sinf(x * 127.1f + y * 311.7f) * 43758.5453f;
    return h - floorf(h);
}

// "CAPTURE SOURCE" → "Capture Source".  Cherry Bomb's capital R closes up at
// button sizes and reads as an A ("STAEAMING"), so button labels use title
// case; the big page titles stay in capitals.
static std::string TitleCase(const char* s) {
    std::string out(s);
    bool wordStart = true;
    for (char& ch : out) {
        if (ch == ' ') { wordStart = true; continue; }
        ch = static_cast<char>(wordStart ? toupper(static_cast<unsigned char>(ch))
                                         : tolower(static_cast<unsigned char>(ch)));
        wordStart = false;
    }
    return out;
}

// 0 → 1 with a small overshoot past 1 before settling — the "bouncy" ease.
static float EaseOutBack(float t) {
    t = std::clamp(t, 0.f, 1.f);
    constexpr float c1 = 1.70158f, c3 = c1 + 1.f;
    return 1.f + c3 * powf(t - 1.f, 3.f) + c1 * powf(t - 1.f, 2.f);
}

// Heavy landing: falls, then bounces to rest.
static float EaseOutBounce(float x) {
    x = std::clamp(x, 0.f, 1.f);
    constexpr float n = 7.5625f, d = 2.75f;
    if (x < 1.f / d)   return n * x * x;
    if (x < 2.f / d)   { x -= 1.5f   / d; return n * x * x + 0.75f; }
    if (x < 2.5f / d)  { x -= 2.25f  / d; return n * x * x + 0.9375f; }
    x -= 2.625f / d;   return n * x * x + 0.984375f;
}


// ---------------------------------------------------------------------------
// Paged menu — the layout shared by the game-style menus: a main page with
// every settings group as a button in a grid, a page per group, a footer with
// BACK on the left and QUIT on the right, and a close button in the top-right
// corner.  Esc steps back to the main page.  A style supplies its look and
// layout by overriding the hooks; DrawPagedMenu does the navigation.
// All lengths are at 1x — DrawPagedMenu applies the menu scale.
// ---------------------------------------------------------------------------
class PagedMenuStyle {
public:
    virtual ~PagedMenuStyle() = default;

    // Layout.
    virtual ImVec2 PanelSize()   const = 0;
    virtual ImVec2 Padding()     const = 0;   // window padding
    virtual float  TitleSpace()  const = 0;   // space above the page content
    virtual float  FooterSpace() const = 0;   // space below it
    virtual float  FooterRow()   const = 0;   // footer buttons' top, up from the panel bottom
    virtual float  FooterInset() const { return 0.f; }   // footer buttons' extra side margin
    virtual int    Columns()     const { return 2; }
    virtual float  Gutter()      const { return 0.f; }   // room left of the first column (hover pointer)
    virtual float  ColumnGap()   const { return 12.f; }
    virtual float  GroupButtonHeight() const = 0;        // main page
    virtual float  ItemButtonHeight()  const = 0;        // preset and style grids
    virtual float  SliderWidth() const { return 0.55f; } // fraction of the content width

    // Text.
    virtual ImFont*     Font()     const = 0;
    virtual float       FontSize() const = 0;
    virtual std::string PageTitle(int pageIndex) const {
        return pageIndex < 0 ? "SETTINGS" : kSectionNames[pageIndex];
    }
    virtual std::string GroupLabel(int section) const { return TitleCase(kSectionNames[section]); }

    // Animation — t is seconds since the panel opened / the page changed.
    virtual ImVec2 OpenOffset(float)  const { return ImVec2(0.f, 0.f); }
    virtual float  OpenReveal(float)  const { return 1.f; }   // visible fraction, growing from the middle
    virtual ImVec2 PageOffset(float)  const { return ImVec2(0.f, 0.f); }
    virtual float  PageReveal(float)  const { return 1.f; }   // visible fraction of the content, top down

    // Drawing.  a..b is the panel in screen space.
    virtual void   DrawPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, int page, float t) = 0;
    virtual void   DrawTitle(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* title, int page, float t) = 0;
    // A grid button — a settings group (main page), a preset or a menu style.
    // True when clicked.
    enum class Kind { Group, Preset, Style };
    virtual bool   GroupButton(const char* id, const char* label, ImVec2 size, int index,
                               bool selected, Kind kind) = 0;
    virtual bool   BackButton(const char* id) = 0;     // drawn at the cursor
    virtual float  QuitWidth() const = 0;
    virtual bool   QuitButton(const char* id) = 0;     // drawn at the cursor
    virtual void   DrawFooter(ImDrawList*, ImVec2 /*a*/, ImVec2 /*b*/, int /*page*/, float /*t*/) {}  // a..b = footer band
    virtual ImVec2 CloseCenter(ImVec2 a, ImVec2 b) const = 0;
    virtual bool   CloseButton(const char* id, ImVec2 center) = 0;

    // Navigation state.
    int   page      = -1;   // -1 = main page, else a section index
    float pageTime  = 0.f;
    float openTime  = 0.f;
    int   lastFrame = -10;
};

static SettingsPanel::DrawResult DrawPagedMenu(PagedMenuStyle& st, const char* windowId,
                                               CRTSettings& s,
                                               const Monitors& monitors,
                                               HMONITOR selectedMon,
                                               const std::vector<HMONITOR>& secondaryMons,
                                               bool streamOpen,
                                               const std::vector<PresetItem>& presets,
                                               const AudioDevs& audioDevices) {
    SettingsPanel::DrawResult result;
    const float t = static_cast<float>(ImGui::GetTime());

    // Freshly opened (not drawn last frame): start on the main page.
    if (ImGui::GetFrameCount() - st.lastFrame > 1) {
        st.openTime = t;
        st.page     = -1;
        st.pageTime = t;
    }
    st.lastFrame = ImGui::GetFrameCount();
    auto goTo = [&](int page) { st.page = page; st.pageTime = t; };

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float  panelW  = PanelWidth(st.PanelSize().x);
    const float  panelH  = PanelHeight(st.PanelSize().y);
    const ImVec2 open    = st.OpenOffset(t - st.openTime);
    ImGui::SetNextWindowPos(ImVec2((display.x - panelW) * 0.5f + open.x, (display.y - panelH) * 0.5f + open.y),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Sc(st.Padding().x), Sc(st.Padding().y)));
    const bool visible = ImGui::Begin(windowId, nullptr, kFlags);
    ImGui::PopStyleVar();
    if (!visible) { ImGui::End(); return result; }

    ImGui::PushFont(st.Font(), st.FontSize());
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 pa = ImGui::GetWindowPos();
    const ImVec2 pb(pa.x + panelW, pa.y + panelH);

    // Opening reveal: only a band through the middle shows, growing to full height.
    const float reveal  = st.OpenReveal(t - st.openTime);
    ImVec2      clipMin = pa, clipMax = pb;   // the visible part of the panel
    if (reveal < 1.f) {
        const float cy = (pa.y + pb.y) * 0.5f;
        const float hh = std::max(Sc(1.f), panelH * 0.5f * reveal);
        clipMin = ImVec2(pa.x, cy - hh);
        clipMax = ImVec2(pb.x, cy + hh);
        ImGui::PushClipRect(clipMin, clipMax, false);
    }

    st.DrawPanel(dl, pa, pb, st.page, t);
    st.DrawTitle(dl, pa, pb, st.PageTitle(st.page).c_str(), st.page, t);
    ImGui::Dummy(ImVec2(0.f, Sc(st.TitleSpace())));

    // Page content.
    const ImVec2 po = st.PageOffset(t - st.pageTime);
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + po.x, ImGui::GetCursorPosY() + po.y));
    ImGui::BeginChild("##page", ImVec2(0.f, -Sc(st.FooterSpace())), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    {
        const float pageReveal = st.PageReveal(t - st.pageTime);
        if (pageReveal < 1.f) {
            const ImVec2 cp = ImGui::GetWindowPos(), cs = ImGui::GetWindowSize();
            ImGui::PushClipRect(cp, ImVec2(cp.x + cs.x, cp.y + cs.y * pageReveal), true);
        }

        const float avail  = ImGui::GetContentRegionAvail().x;
        const int   cols   = st.Columns();
        const float gutter = Sc(st.Gutter()), gap = Sc(st.ColumnGap());
        const float bw     = (avail - gutter - gap * (cols - 1) - Sc(12.f)) / cols;
        auto gridCell = [&](int i) {
            if (i % cols == 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gutter);
            else               ImGui::SameLine(0.f, gap);
        };
        const ImVec2 itemSize(bw, Sc(st.ItemButtonHeight()));

        if (st.page < 0) {
            for (int i = 0; i < kSectionCount; ++i) {
                gridCell(i);
                ImGui::PushID(i);
                if (st.GroupButton("##group", st.GroupLabel(i).c_str(), ImVec2(bw, Sc(st.GroupButtonHeight())),
                                   i, false, PagedMenuStyle::Kind::Group))
                    goTo(i);
                ImGui::PopID();
            }
        } else if (st.page == kPresetsSection) {
            // Presets as a grid of buttons, with the usual dropdown / save / manage row underneath.
            for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
                gridCell(i);
                ImGui::PushID(i);
                if (st.GroupButton("##preset", presets[i].name.c_str(), itemSize, i, false,
                                   PagedMenuStyle::Kind::Preset)) {
                    result.applyPreset    = true;
                    result.presetSettings = presets[i].settings;
                }
                ImGui::PopID();
            }
            ImGui::Dummy(ImVec2(0.f, Sc(6.f)));
            DrawSectionContent(st.page, s, monitors, selectedMon, secondaryMons,
                               streamOpen, presets, audioDevices, result);
        } else if (st.page == kMenuSection) {
            // Styles as a grid of buttons, then the panel-size sliders.
            for (int i = 0; i < kStyleCount; ++i) {
                gridCell(i);
                ImGui::PushID(i);
                if (st.GroupButton("##style", kStyles[i].name, itemSize, i, s.menuStyle == kStyles[i].id,
                                   PagedMenuStyle::Kind::Style))
                    s.menuStyle = kStyles[i].id;
                ImGui::PopID();
            }
            ImGui::Dummy(ImVec2(0.f, Sc(14.f)));
            ImGui::PushItemWidth(avail * st.SliderWidth());
            DrawMenuSizeControls(s);
            ImGui::PopItemWidth();
        } else {
            ImGui::PushItemWidth(avail * st.SliderWidth());
            DrawSectionContent(st.page, s, monitors, selectedMon, secondaryMons,
                               streamOpen, presets, audioDevices, result);
            ImGui::PopItemWidth();
        }
        ImGui::Dummy(ImVec2(0.f, Sc(8.f)));   // room for the last button's shadow

        if (pageReveal < 1.f) ImGui::PopClipRect();
    }
    ImGui::EndChild();

    if (st.page >= 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive())
        goTo(-1);

    // Footer — BACK on the left (not on the main page), QUIT on the right.
    const float footerY = panelH - Sc(st.FooterRow());
    const float padX    = ImGui::GetCursorStartPos().x;   // this window's padding
    const float inset   = Sc(st.FooterInset());
    st.DrawFooter(dl, ImVec2(pa.x, pa.y + footerY), pb, st.page, t);
    if (st.page >= 0) {
        ImGui::SetCursorPos(ImVec2(padX + inset, footerY));
        if (st.BackButton("##back")) goTo(-1);
        Tip("Back to the main menu (Esc).");
    }
    ImGui::SetCursorPos(ImVec2(panelW - padX - inset - st.QuitWidth(), footerY));
    if (st.QuitButton("##quit")) PostQuitMessage(0);
    Tip("Quit the overlay application entirely.");

    // Close button.  It may sit in the window padding, so widen the clip rect to reach it.
    ImGui::PushClipRect(clipMin, clipMax, false);
    if (st.CloseButton("##close", st.CloseCenter(pa, pb))) s.menuVisible = false;
    Tip("Close the settings panel (Ctrl+Shift+C to re-open).");
    ImGui::PopClipRect();

    if (reveal < 1.f) ImGui::PopClipRect();
    ImGui::PopFont();
    ImGui::End();
    return result;
}

// ---------------------------------------------------------------------------
// 64 Adventure layout — a bubbly late-90s console-game menu.
//   • Rainbow title in the classic four-colour console palette, each letter
//     bobbing on a wave, with a chunky cartoon outline.
//   • Main page: every settings group as a glossy colour-block button that
//     wobbles when hovered (with a spinning star) and squishes when pressed.
//   • Clicking a group opens a page with that group's sliders; the footer has
//     controller-style prompts — (B) BACK, (START) CLOSE, (C) QUIT.
//   • The panel and each page pop in with an overshooting ease.
// ---------------------------------------------------------------------------
namespace adv64 {

constexpr ImU32 kRed    = IM_COL32(232,  44,  52, 255);
constexpr ImU32 kYellow = IM_COL32(255, 196,  24, 255);
constexpr ImU32 kGreen  = IM_COL32( 34, 176,  76, 255);
constexpr ImU32 kBlue   = IM_COL32( 44,  96, 224, 255);
constexpr ImU32 kInk    = IM_COL32( 22,  14,  44, 255);  // outlines
constexpr ImU32 kWhite  = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kPalette[4] = { kRed, kYellow, kGreen, kBlue };

ImFont* Font() { return StyleFonts::cherryBomb ? StyleFonts::cherryBomb : ImGui::GetFont(); }

ImVec2 TextSize(float size, const char* text) {
    return Font()->CalcTextSizeA(size, FLT_MAX, 0.f, text);
}

// Chunky cartoon text: soft drop shadow, ink outline, then the fill.
void CartoonText(ImDrawList* dl, float size, ImVec2 pos, ImU32 fill, const char* text) {
    ImFont* f = Font();
    // Thinner outline on small text so letter counters (e.g. the gap in R)
    // don't fill in.
    const float o = size < Sc(30.f) ? std::max(Sc(1.2f), size * 0.055f) : size * 0.07f;
    dl->AddText(f, size, ImVec2(pos.x + o * 1.5f, pos.y + o * 1.8f), IM_COL32(0, 0, 0, 120), text);
    for (int i = 0; i < 8; ++i) {
        const float a = i * 0.7853982f;
        dl->AddText(f, size, ImVec2(pos.x + cosf(a) * o, pos.y + sinf(a) * o), kInk, text);
    }
    dl->AddText(f, size, pos, fill, text);
}

// Five-point star (a triangle fan, since a star is concave) with an ink rim.
void Star(ImDrawList* dl, ImVec2 c, float r, float rot, ImU32 fill) {
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        const float rr = (i % 2 == 0) ? r : r * 0.45f;
        const float a  = rot + i * 0.6283185f - 1.5707963f;
        pts[i] = ImVec2(c.x + cosf(a) * rr, c.y + sinf(a) * rr);
    }
    for (int i = 0; i < 10; ++i)
        dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], fill);
    dl->AddPolyline(pts, 10, kInk, ImDrawFlags_Closed, Sc(2.f));
}

// Glossy pill button.  Returns true when clicked.
bool BubbleButton(const char* id, const char* label, ImVec2 size, ImU32 color,
                  float fontSize, bool selected = false) {
    const ImVec2 pos     = ImGui::GetCursorScreenPos();
    const bool   clicked = ImGui::InvisibleButton(id, size);
    const bool   hovered = ImGui::IsItemHovered();
    const bool   held    = ImGui::IsItemActive();
    const float  t       = static_cast<float>(ImGui::GetTime());

    float scale = 1.f;
    if (held)         scale = 0.95f;                          // squish
    else if (hovered) scale = 1.05f + 0.02f * sinf(t * 9.f);  // wobble

    const ImVec2 c    (pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    const ImVec2 half (size.x * 0.5f * scale, size.y * 0.5f * scale);
    const ImVec2 a    (c.x - half.x, c.y - half.y);
    const ImVec2 b    (c.x + half.x, c.y + half.y);
    const float  r    = half.y;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(a.x + Sc(2.f), a.y + Sc(6.f)), ImVec2(b.x + Sc(2.f), b.y + Sc(6.f)),
                      IM_COL32(0, 0, 0, 110), r);                                 // drop shadow
    dl->AddRectFilled(a, b, Shade(color, 0.70f), r);                              // darker lower lip
    dl->AddRectFilled(a, ImVec2(b.x, b.y - half.y * 0.22f),
                      (hovered || selected) ? Shade(color, 1.15f) : color, r);    // face
    dl->AddRectFilled(ImVec2(a.x + r * 0.6f, a.y + half.y * 0.18f),
                      ImVec2(b.x - r * 0.6f, a.y + half.y * 0.80f),
                      IM_COL32(255, 255, 255, 70), r);                            // gloss
    dl->AddRect(a, b, kInk, r, 0, Sc(3.f));

    const float  fs = fontSize * scale;
    const ImVec2 ts = TextSize(fs, label);
    CartoonText(dl, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.55f), kWhite, label);

    if (hovered || selected)
        Star(dl, ImVec2(a.x - size.y * 0.42f, c.y), size.y * 0.36f, t * 3.f, kYellow);
    return clicked;
}

// Controller-style prompt: a coloured button glyph (round for one letter, a
// pill for words like START) followed by a label.
float PromptGlyphWidth(const char* glyph, float fontSize) {
    const float d = fontSize * 1.6f;
    return std::max(d, TextSize(fontSize * (glyph[1] ? 0.70f : 1.0f), glyph).x + d * 0.6f);
}

float PromptWidth(const char* glyph, const char* label, float fontSize) {
    return PromptGlyphWidth(glyph, fontSize) + Sc(10.f) + TextSize(fontSize, label).x;
}

// Draws a prompt at the cursor.  Returns true when clicked.
bool Prompt(const char* id, const char* glyph, ImU32 color, const char* label, float fontSize) {
    const float  d   = fontSize * 1.6f;
    const float  gfs = fontSize * (glyph[1] ? 0.70f : 1.0f);
    const ImVec2 gs  = TextSize(gfs, glyph);
    const float  gw  = PromptGlyphWidth(glyph, fontSize);
    const ImVec2 ls  = TextSize(fontSize, label);
    const ImVec2 size(PromptWidth(glyph, label, fontSize), d);

    const ImVec2 pos     = ImGui::GetCursorScreenPos();
    const bool   clicked = ImGui::InvisibleButton(id, size);
    const bool   hovered = ImGui::IsItemHovered();
    const float  bob     = hovered ? Sc(-2.f + sinf(static_cast<float>(ImGui::GetTime()) * 10.f) * 1.5f) : 0.f;

    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 a(pos.x, pos.y + bob);
    const ImVec2 b(pos.x + gw, pos.y + d + bob);
    dl->AddRectFilled(ImVec2(a.x + Sc(1.f), a.y + Sc(4.f)), ImVec2(b.x + Sc(1.f), b.y + Sc(4.f)),
                      IM_COL32(0, 0, 0, 110), d * 0.5f);
    dl->AddRectFilled(a, b, hovered ? Shade(color, 1.15f) : color, d * 0.5f);
    dl->AddRectFilled(ImVec2(a.x + d * 0.22f, a.y + d * 0.12f),
                      ImVec2(b.x - d * 0.22f, a.y + d * 0.46f), IM_COL32(255, 255, 255, 70), d * 0.5f);
    dl->AddRect(a, b, kInk, d * 0.5f, 0, Sc(2.5f));
    CartoonText(dl, gfs, ImVec2((a.x + b.x - gs.x) * 0.5f, (a.y + b.y - gs.y) * 0.5f), kWhite, glyph);
    CartoonText(dl, fontSize, ImVec2(b.x + Sc(10.f), (a.y + b.y - ls.y) * 0.5f),
                hovered ? kYellow : kWhite, label);
    return clicked;
}

// Round red close button with a chunky white X, centred on c.  Wobbles and
// tilts when hovered, squishes when pressed.  Returns true when clicked.
bool CloseButton(const char* id, ImVec2 c, float r) {
    ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
    const bool  clicked = ImGui::InvisibleButton(id, ImVec2(r * 2.f, r * 2.f));
    const bool  hovered = ImGui::IsItemHovered();
    const bool  held    = ImGui::IsItemActive();
    const float t       = static_cast<float>(ImGui::GetTime());

    float scale = 1.f, tilt = 0.f;
    if (held)         scale = 0.9f;
    else if (hovered) { scale = 1.08f + 0.03f * sinf(t * 9.f); tilt = 0.25f * sinf(t * 6.f); }
    const float rr = r * scale;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(ImVec2(c.x + Sc(2.f), c.y + Sc(5.f)), rr, IM_COL32(0, 0, 0, 110));  // drop shadow
    dl->AddCircleFilled(c, rr, Shade(kRed, 0.70f));                                          // darker lower lip
    dl->AddCircleFilled(ImVec2(c.x, c.y - rr * 0.08f), rr * 0.92f,
                        hovered ? Shade(kRed, 1.15f) : kRed);                                // face
    dl->AddCircleFilled(ImVec2(c.x - rr * 0.18f, c.y - rr * 0.42f), rr * 0.36f,
                        IM_COL32(255, 255, 255, 70));                                        // gloss
    dl->AddCircle(c, rr, kInk, 0, Sc(3.f));

    // The X: an ink stroke under a white one, rotated by the hover tilt.
    for (int pass = 0; pass < 2; ++pass) {
        const float  arm = pass == 0 ? rr * 0.50f : rr * 0.42f;
        const float  w   = pass == 0 ? rr * 0.36f : rr * 0.20f;
        const ImU32  col = pass == 0 ? kInk : kWhite;
        for (int k = 0; k < 2; ++k) {
            const float  a = tilt + 0.7853982f + k * 1.5707963f;
            const ImVec2 d(cosf(a) * arm, sinf(a) * arm);
            dl->AddLine(ImVec2(c.x - d.x, c.y - d.y), ImVec2(c.x + d.x, c.y + d.y), col, w);
        }
    }
    return clicked;
}

// Royal-blue panel with a gold rim, ink outline, and a slowly scrolling
// faint checker pattern.
void Panel(ImDrawList* dl, ImVec2 a, ImVec2 b, float t) {
    const float r = Sc(30.f), sh = Sc(8.f), rim = Sc(4.f), in = Sc(12.f), ink = Sc(2.f);
    dl->AddRectFilled(ImVec2(a.x + sh, a.y + sh * 1.5f), ImVec2(b.x + sh, b.y + sh * 1.5f), IM_COL32(0, 0, 0, 120), r);
    dl->AddRectFilled(a, b, kInk, r);
    dl->AddRectFilled(ImVec2(a.x + rim, a.y + rim), ImVec2(b.x - rim, b.y - rim), kYellow, r - ink);
    const ImVec2 ca(a.x + in, a.y + in), cb(b.x - in, b.y - in);
    dl->AddRectFilled(ImVec2(ca.x - ink, ca.y - ink), ImVec2(cb.x + ink, cb.y + ink), kInk, r - Sc(8.f));
    dl->AddRectFilled(ca, cb, IM_COL32(30, 38, 128, 255), r - Sc(10.f));

    // Checker, clipped just inside the rounded corners.
    const float cell   = Sc(32.f);
    const float scroll = fmodf(t * Sc(12.f), cell * 2.f);
    dl->PushClipRect(ImVec2(ca.x + Sc(9.f), ca.y + Sc(9.f)), ImVec2(cb.x - Sc(9.f), cb.y - Sc(9.f)), true);
    for (float y = ca.y - cell * 2.f + scroll; y < cb.y; y += cell) {
        for (float x = ca.x - cell * 2.f + scroll; x < cb.x; x += cell) {
            const int ix = static_cast<int>(floorf((x - ca.x + cell * 2.f - scroll) / cell + 0.5f));
            const int iy = static_cast<int>(floorf((y - ca.y + cell * 2.f - scroll) / cell + 0.5f));
            if ((ix + iy) % 2 == 0)
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cell, y + cell), IM_COL32(255, 255, 255, 14));
        }
    }
    dl->PopClipRect();

    // Soft highlight across the top, like a plastic sheen.
    dl->AddRectFilled(ImVec2(ca.x + Sc(20.f), ca.y + Sc(6.f)), ImVec2(cb.x - Sc(20.f), ca.y + Sc(60.f)),
                      IM_COL32(255, 255, 255, 18), Sc(20.f));
}

// The 64 Adventure look for the shared paged menu.
class Style : public PagedMenuStyle {
public:
    ImVec2  PanelSize()   const override { return ImVec2(680.f, 690.f); }
    ImVec2  Padding()     const override { return ImVec2(36.f, 26.f); }
    float   TitleSpace()  const override { return 70.f; }
    float   FooterSpace() const override { return 60.f; }
    float   FooterRow()   const override { return 64.f; }
    float   FooterInset() const override { return 8.f; }
    float   Gutter()      const override { return 48.f; }   // room for the hover star
    float   ColumnGap()   const override { return 50.f; }
    float   GroupButtonHeight() const override { return 54.f; }
    float   ItemButtonHeight()  const override { return 44.f; }
    ImFont* Font()        const override { return adv64::Font(); }
    float   FontSize()    const override { return 20.f; }

    // The panel drops in and each page slides up, both with a bouncy overshoot.
    ImVec2 OpenOffset(float t) const override { return ImVec2(0.f, (1.f - EaseOutBack(t / 0.45f)) * Sc(-80.f)); }
    ImVec2 PageOffset(float t) const override { return ImVec2(0.f, (1.f - EaseOutBack(t / 0.35f)) * Sc(40.f)); }

    void DrawPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, int, float t) override { Panel(dl, a, b, t); }

    // Every letter its own colour, bobbing on a wave.
    void DrawTitle(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* title, int, float t) override {
        const float size = Sc(50.f), gap = Sc(2.f);
        float width = 0.f;
        for (int i = 0; title[i]; ++i) {
            const char ch[2] = { title[i], 0 };
            width += TextSize(size, ch).x + gap;
        }
        float x = a.x + (b.x - a.x - width) * 0.5f;
        for (int i = 0; title[i]; ++i) {
            const char ch[2] = { title[i], 0 };
            if (ch[0] != ' ')
                CartoonText(dl, size, ImVec2(x, a.y + Sc(24.f + sinf(t * 3.2f + i * 0.55f) * 4.f)),
                            kPalette[i % 4], ch);
            x += TextSize(size, ch).x + gap;
        }
    }

    bool GroupButton(const char* id, const char* label, ImVec2 size, int index, bool selected, Kind kind) override {
        return BubbleButton(id, label, size, kPalette[(index / 2 + index) % 4],
                            Sc(kind == Kind::Group ? 22.f : 19.f), selected);
    }
    bool   BackButton(const char* id) override { return Prompt(id, "B", kGreen, "BACK", Sc(20.f)); }
    float  QuitWidth() const override { return PromptWidth("C", "QUIT", Sc(20.f)); }
    bool   QuitButton(const char* id) override { return Prompt(id, "C", kYellow, "QUIT", Sc(20.f)); }
    ImVec2 CloseCenter(ImVec2 a, ImVec2 b) const override { return ImVec2(b.x - Sc(48.f), a.y + Sc(48.f)); }
    bool   CloseButton(const char* id, ImVec2 c) override { return adv64::CloseButton(id, c, Sc(22.f)); }
};

} // namespace adv64

// ---------------------------------------------------------------------------
// Sword & Sorcery layout — an 80s barbarian-fantasy dungeon menu.
//   • A stone wall in a riveted bronze frame, lit by two flickering torches
//     that cast warm light and drift embers.
//   • Heavy-metal title in molten gold, with a glint sweeping across it.
//   • Main page: every settings group as a chamfered stone tablet with
//     engraved lettering; hovering one wreathes it in purple sorcery and
//     points a sword at it.  Clicking opens that group's page.
//   • The panel slams down like a dungeon gate and pages rumble in.
//   • A skull keeps watch between BACK and QUIT; close is an iron seal on
//     the top-right corner.
// ---------------------------------------------------------------------------
namespace sorcery {

constexpr ImU32 kBronze   = IM_COL32(168, 112,  46, 255);
constexpr ImU32 kBronzeHi = IM_COL32(236, 184, 100, 255);
constexpr ImU32 kBronzeLo = IM_COL32( 70,  40,  14, 255);
constexpr ImU32 kGoldHi   = IM_COL32(255, 246, 190, 255);
constexpr ImU32 kGold     = IM_COL32(240, 180,  60, 255);
constexpr ImU32 kGoldLo   = IM_COL32(130,  62,  12, 255);
constexpr ImU32 kStone    = IM_COL32( 92,  88,  98, 255);
constexpr ImU32 kMortar   = IM_COL32( 22,  18,  26, 255);
constexpr ImU32 kEngrave  = IM_COL32(224, 216, 196, 255);  // pale carved lettering
constexpr ImU32 kShadow   = IM_COL32( 10,   6,  12, 255);
constexpr ImU32 kSorcery  = IM_COL32(170,  80, 255, 255);  // purple magic glow
constexpr ImU32 kBlood    = IM_COL32(150,  22,  26, 255);
constexpr ImU32 kEyeGlow  = IM_COL32(120, 255,  90, 255);  // skull eyes
constexpr ImU32 kFire[4]  = { IM_COL32(200,  40,  10, 255), IM_COL32(255, 120,  20, 255),
                              IM_COL32(255, 200,  60, 255), IM_COL32(255, 245, 200, 255) };

ImFont* TitleFont() { return StyleFonts::metalMania ? StyleFonts::metalMania : ImGui::GetFont(); }
ImFont* BodyFont()  { return StyleFonts::cinzel     ? StyleFonts::cinzel     : ImGui::GetFont(); }

// Torch-light flicker in roughly [0, 1]: a few unrelated sines.
float Flicker(float t, float seed) {
    return 0.5f + 0.25f * sinf(t * 7.3f + seed) + 0.15f * sinf(t * 13.1f + seed * 2.1f)
                + 0.10f * sinf(t * 23.7f + seed * 3.7f);
}

// A rectangle with its corners cut off, clockwise from the top-left corner's
// top edge.
void Chamfer(ImVec2 a, ImVec2 b, float c, ImVec2 out[8]) {
    out[0] = ImVec2(a.x + c, a.y);  out[1] = ImVec2(b.x - c, a.y);
    out[2] = ImVec2(b.x, a.y + c);  out[3] = ImVec2(b.x, b.y - c);
    out[4] = ImVec2(b.x - c, b.y);  out[5] = ImVec2(a.x + c, b.y);
    out[6] = ImVec2(a.x, b.y - c);  out[7] = ImVec2(a.x, a.y + c);
}

// Domed bronze rivet.
void Stud(ImDrawList* dl, ImVec2 c, float r) {
    dl->AddCircleFilled(c, r + Sc(1.f), kShadow);
    dl->AddCircleFilled(c, r, kBronzeLo);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.15f, c.y - r * 0.15f), r * 0.78f, kBronze);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.32f, c.y - r * 0.32f), r * 0.30f, kBronzeHi);
}

// Engraved lettering: a dark cut above-left, a light catch below-right, then
// the fill as a top-to-bottom gradient.
void CarvedText(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, const char* text,
                ImU32 top, ImU32 bottom) {
    const float o = std::max(Sc(1.f), size * 0.05f);
    dl->AddText(f, size, ImVec2(pos.x - o, pos.y - o), IM_COL32(0, 0, 0, 210), text);
    dl->AddText(f, size, ImVec2(pos.x + o, pos.y + o), IM_COL32(255, 236, 200, 60), text);
    const int v0 = dl->VtxBuffer.Size;
    dl->AddText(f, size, pos, IM_COL32_WHITE, text);
    ShadeVerts(dl, v0, [&](ImVec2 p) { return VGrad(p.y, pos.y + size * 0.15f, pos.y + size, top, bottom); });
}

// Heavy-metal title in molten gold: drop shadow, dark outline, a three-stop
// gold gradient, and a diagonal glint that sweeps across every few seconds.
void MoltenTitle(ImDrawList* dl, float size, ImVec2 pos, const char* text, float t) {
    ImFont*      f  = TitleFont();
    const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.f, text);
    const float  o  = std::max(Sc(1.5f), size * 0.045f);

    dl->AddText(f, size, ImVec2(pos.x + o * 1.5f, pos.y + o * 2.2f), IM_COL32(0, 0, 0, 170), text);
    for (int i = 0; i < 8; ++i) {
        const float a = i * 0.7853982f;
        dl->AddText(f, size, ImVec2(pos.x + cosf(a) * o, pos.y + sinf(a) * o), IM_COL32(40, 18, 4, 255), text);
    }
    const int v0 = dl->VtxBuffer.Size;
    dl->AddText(f, size, pos, IM_COL32_WHITE, text);

    const float sweep = fmodf(t, 3.5f) / 1.2f;   // glint crosses in 1.2 s, then rests
    const float gx    = pos.x - Sc(60.f) + sweep * (ts.x + Sc(120.f));
    const float band  = Sc(26.f);
    ShadeVerts(dl, v0, [&](ImVec2 p) {
        const ImU32 base = VGrad3(p.y, pos.y + size * 0.2f, pos.y + size * 0.95f, kGoldHi, kGold, kGoldLo);
        const float d    = fabsf(p.x + (p.y - pos.y) * 0.4f - gx);
        return Mix(base, IM_COL32_WHITE, std::max(0.f, 1.f - d / band) * 0.85f);
    });
}

// Riveted bronze frame filling a..b.
void Frame(ImDrawList* dl, ImVec2 a, ImVec2 b, float thickness) {
    const float r = Sc(6.f);
    dl->AddRectFilled(ImVec2(a.x + Sc(10.f), a.y + Sc(14.f)), ImVec2(b.x + Sc(10.f), b.y + Sc(14.f)),
                      IM_COL32(0, 0, 0, 140), r);
    dl->AddRectFilled(a, b, kShadow, r);
    const int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(ImVec2(a.x + Sc(2.f), a.y + Sc(2.f)), ImVec2(b.x - Sc(2.f), b.y - Sc(2.f)), IM_COL32_WHITE, r);
    ShadeVerts(dl, v0, [&](ImVec2 p) { return VGrad3(p.y, a.y, b.y, kBronzeHi, kBronze, kBronzeLo); });
    // Light bevel on the outer top edge, inner shadow where it meets the wall.
    dl->AddLine(ImVec2(a.x + r, a.y + Sc(3.f)), ImVec2(b.x - r, a.y + Sc(3.f)), IM_COL32(255, 230, 170, 120), Sc(1.5f));
    dl->AddRect(ImVec2(a.x + thickness - Sc(2.f), a.y + thickness - Sc(2.f)),
                ImVec2(b.x - thickness + Sc(2.f), b.y - thickness + Sc(2.f)), kShadow, 0.f, 0, Sc(3.f));

    // Rivets along every edge, and bigger ones on the corners.
    const float mid = thickness * 0.5f;
    const int   nx  = std::max(2, static_cast<int>((b.x - a.x) / Sc(64.f)));
    const int   ny  = std::max(2, static_cast<int>((b.y - a.y) / Sc(64.f)));
    for (int i = 1; i < nx; ++i) {
        const float x = a.x + (b.x - a.x) * i / nx;
        Stud(dl, ImVec2(x, a.y + mid), Sc(3.2f));
        Stud(dl, ImVec2(x, b.y - mid), Sc(3.2f));
    }
    for (int i = 1; i < ny; ++i) {
        const float y = a.y + (b.y - a.y) * i / ny;
        Stud(dl, ImVec2(a.x + mid, y), Sc(3.2f));
        Stud(dl, ImVec2(b.x - mid, y), Sc(3.2f));
    }
    Stud(dl, ImVec2(a.x + mid, a.y + mid), Sc(5.5f));
    Stud(dl, ImVec2(b.x - mid, a.y + mid), Sc(5.5f));
    Stud(dl, ImVec2(a.x + mid, b.y - mid), Sc(5.5f));
    Stud(dl, ImVec2(b.x - mid, b.y - mid), Sc(5.5f));
}

// Staggered stone blocks with mortar joints, each block a slightly different
// shade, then darkened toward the edges.
void Wall(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    dl->AddRectFilled(a, b, kMortar);
    const float bh = Sc(34.f), bw = Sc(74.f), gap = Sc(3.f);
    dl->PushClipRect(a, b, true);
    int row = 0;
    for (float y = a.y; y < b.y; y += bh, ++row) {
        const float off = (row % 2) ? bw * 0.5f : 0.f;
        int col = 0;
        for (float x = a.x - off; x < b.x; x += bw, ++col) {
            const ImU32  c  = Mix(IM_COL32(50, 46, 56, 255), IM_COL32(80, 74, 86, 255),
                                  Hash(static_cast<float>(col), static_cast<float>(row)));
            const ImVec2 p0(x + gap * 0.5f, y + gap * 0.5f), p1(x + bw - gap * 0.5f, y + bh - gap * 0.5f);
            dl->AddRectFilled(p0, p1, c, Sc(3.f));
            dl->AddLine(ImVec2(p0.x + Sc(3.f), p0.y + Sc(1.f)), ImVec2(p1.x - Sc(3.f), p0.y + Sc(1.f)),
                        IM_COL32(255, 255, 255, 16), Sc(1.f));
            dl->AddLine(ImVec2(p0.x + Sc(3.f), p1.y - Sc(1.f)), ImVec2(p1.x - Sc(3.f), p1.y - Sc(1.f)),
                        IM_COL32(0, 0, 0, 70), Sc(1.f));
        }
    }
    // Vignette — the corners of the dungeon fall into darkness.
    const float  v = Sc(90.f);
    const ImU32  d = IM_COL32(0, 0, 0, 170), z = IM_COL32(0, 0, 0, 0);
    dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + v), d, d, z, z);
    dl->AddRectFilledMultiColor(ImVec2(a.x, b.y - v), b, z, z, d, d);
    dl->AddRectFilledMultiColor(a, ImVec2(a.x + v, b.y), d, z, z, d);
    dl->AddRectFilledMultiColor(ImVec2(b.x - v, a.y), b, z, d, d, z);
    dl->PopClipRect();
}

// Flame layer: a teardrop with a rounded base centred on c and its tip h
// above, leaning by sway.  Points run clockwise, as convex fills expect.
void Teardrop(ImDrawList* dl, ImVec2 c, float w, float h, float sway, ImU32 col) {
    ImVec2 pts[14];
    pts[0] = ImVec2(c.x + sway, c.y - h);
    for (int i = 0; i <= 12; ++i) {
        const float a = i / 12.f * 3.14159265f;
        pts[1 + i] = ImVec2(c.x + cosf(a) * w, c.y + sinf(a) * w * 0.8f);
    }
    dl->AddConvexPolyFilled(pts, 14, col);
}

// Wall torch whose flame sits on `root`: warm light on the stones, a bronze
// cup on a wooden haft, a four-layer flickering flame and rising embers.
void Torch(ImDrawList* dl, ImVec2 root, float t, float seed) {
    const float fl = Flicker(t, seed);
    for (int i = 0; i < 7; ++i)
        dl->AddCircleFilled(ImVec2(root.x, root.y - Sc(14.f)), Sc(150.f - i * 18.f) * (0.92f + 0.12f * fl),
                            IM_COL32(255, 140, 40, 8), 48);

    // Haft, iron bracket, cup.
    dl->AddRectFilled(ImVec2(root.x - Sc(5.f), root.y + Sc(5.f)), ImVec2(root.x + Sc(5.f), root.y + Sc(36.f)), kShadow, Sc(2.f));
    dl->AddRectFilled(ImVec2(root.x - Sc(4.f), root.y + Sc(6.f)), ImVec2(root.x + Sc(4.f), root.y + Sc(35.f)),
                      IM_COL32(96, 60, 30, 255), Sc(2.f));
    dl->AddRectFilled(ImVec2(root.x - Sc(9.f), root.y + Sc(19.f)), ImVec2(root.x + Sc(9.f), root.y + Sc(25.f)),
                      IM_COL32(56, 54, 62, 255), Sc(1.f));
    const ImVec2 cup[4] = { ImVec2(root.x - Sc(13.f), root.y - Sc(2.f)), ImVec2(root.x + Sc(13.f), root.y - Sc(2.f)),
                            ImVec2(root.x + Sc(7.f),  root.y + Sc(10.f)), ImVec2(root.x - Sc(7.f),  root.y + Sc(10.f)) };
    const int v0 = dl->VtxBuffer.Size;
    dl->AddConvexPolyFilled(cup, 4, IM_COL32_WHITE);
    ShadeVerts(dl, v0, [&](ImVec2 p) { return VGrad(p.y, root.y - Sc(2.f), root.y + Sc(10.f), kBronzeHi, kBronzeLo); });
    dl->AddPolyline(cup, 4, kShadow, ImDrawFlags_Closed, Sc(1.5f));

    // Flame, outer to inner.
    for (int k = 0; k < 4; ++k) {
        const float w     = Sc(12.f - k * 2.6f) * (0.9f + 0.15f * fl);
        const float h     = Sc(34.f - k * 6.f) * (0.85f + 0.3f * Flicker(t * 1.3f, seed + k));
        const float sway  = Sc(4.f) * sinf(t * 5.f + seed + k * 0.6f);
        Teardrop(dl, ImVec2(root.x, root.y - Sc(4.f) - Sc(1.5f) * k), w, h, sway, kFire[k]);
    }

    // Embers drifting up and fading.
    for (int i = 0; i < 6; ++i) {
        const float  ph = fmodf(t * 0.55f + i * 0.173f + seed, 1.f);
        const ImVec2 p(root.x + sinf(t * 2.f + i * 1.7f + seed) * Sc(10.f) * ph,
                       root.y - Sc(30.f) - ph * Sc(90.f));
        dl->AddCircleFilled(p, Sc(1.8f - ph), WithAlpha(kFire[2], (1.f - ph) * 0.9f));
    }
}

// A sword pointing right with its tip at `tip`; len is roughly tip-to-pommel.
void Sword(ImDrawList* dl, ImVec2 tip, float len) {
    const float  bw     = len * 0.07f;   // blade half-width
    const float  guardX = tip.x - len * 0.62f;
    const ImVec2 blade[5] = { tip, ImVec2(tip.x - len * 0.1f, tip.y + bw), ImVec2(guardX, tip.y + bw),
                              ImVec2(guardX, tip.y - bw), ImVec2(tip.x - len * 0.1f, tip.y - bw) };
    int v0 = dl->VtxBuffer.Size;
    dl->AddConvexPolyFilled(blade, 5, IM_COL32_WHITE);
    ShadeVerts(dl, v0, [&](ImVec2 p) {
        return VGrad(p.y, tip.y - bw, tip.y + bw, IM_COL32(236, 238, 246, 255), IM_COL32(118, 122, 140, 255));
    });
    dl->AddLine(ImVec2(guardX, tip.y), ImVec2(tip.x - len * 0.14f, tip.y), IM_COL32(90, 94, 110, 255), Sc(1.f));
    dl->AddPolyline(blade, 5, kShadow, ImDrawFlags_Closed, Sc(1.5f));

    // Grip with leather wraps, crossguard, pommel.
    dl->AddRectFilled(ImVec2(guardX - len * 0.27f, tip.y - len * 0.045f), ImVec2(guardX, tip.y + len * 0.045f),
                      IM_COL32(92, 40, 30, 255), Sc(1.f));
    for (int i = 1; i < 5; ++i) {
        const float x = guardX - len * 0.27f * i / 5.f;
        dl->AddLine(ImVec2(x, tip.y - len * 0.045f), ImVec2(x - len * 0.02f, tip.y + len * 0.045f), kShadow, Sc(1.f));
    }
    const ImVec2 ga(guardX - len * 0.045f, tip.y - len * 0.17f), gb(guardX + len * 0.01f, tip.y + len * 0.17f);
    v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(ga, gb, IM_COL32_WHITE, Sc(2.f));
    ShadeVerts(dl, v0, [&](ImVec2 p) { return VGrad3(p.y, ga.y, gb.y, kGoldHi, kGold, kGoldLo); });
    dl->AddRect(ga, gb, kShadow, Sc(2.f), 0, Sc(1.2f));
    const ImVec2 pommel(guardX - len * 0.31f, tip.y);
    dl->AddCircleFilled(pommel, len * 0.065f + Sc(1.f), kShadow);
    dl->AddCircleFilled(pommel, len * 0.065f, kGold);
    dl->AddCircleFilled(ImVec2(pommel.x - len * 0.015f, pommel.y - len * 0.02f), len * 0.025f, kGoldHi);
}

// Chamfered stone tablet with engraved lettering and bronze rim.  Hovering it
// wreathes it in purple sorcery (gold when selected) and, with `pointer`,
// points a sword at it from the left; pressing sinks it.  True when clicked.
bool Tablet(const char* id, const char* label, ImVec2 size, float fontSize,
            bool selected = false, ImU32 tint = kStone, bool pointer = true) {
    const ImVec2 pos     = ImGui::GetCursorScreenPos();
    const bool   clicked = ImGui::InvisibleButton(id, size);
    const bool   hovered = ImGui::IsItemHovered();
    const bool   held    = ImGui::IsItemActive();
    const ImGuiID iid    = ImGui::GetItemID();
    const float  t       = static_cast<float>(ImGui::GetTime());

    const float  push = held ? Sc(2.f) : 0.f;
    const ImVec2 a(pos.x, pos.y + push), b(pos.x + size.x, pos.y + size.y + push);
    const float  c = std::min(size.y * 0.28f, Sc(12.f));
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    ImVec2       pts[8];

    // Aura — stacked, growing translucent tablets.
    if (hovered || selected) {
        const float pulse = 0.75f + 0.25f * sinf(t * 5.f);
        const ImU32 glow  = hovered ? kSorcery : kGold;
        for (int i = 6; i >= 1; --i) {
            const float g = Sc(2.2f) * i;
            Chamfer(ImVec2(a.x - g, a.y - g), ImVec2(b.x + g, b.y + g), c + g * 0.6f, pts);
            dl->AddConvexPolyFilled(pts, 8, WithAlpha(glow, 0.07f * pulse));
        }
    }
    if (!held) {
        Chamfer(ImVec2(a.x + Sc(3.f), a.y + Sc(5.f)), ImVec2(b.x + Sc(3.f), b.y + Sc(5.f)), c, pts);
        dl->AddConvexPolyFilled(pts, 8, IM_COL32(0, 0, 0, 140));
    }

    // Face, lit from above.
    Chamfer(a, b, c, pts);
    const ImU32 base = held ? Shade(tint, 0.8f) : hovered ? Shade(tint, 1.18f) : tint;
    const int   v0   = dl->VtxBuffer.Size;
    dl->AddConvexPolyFilled(pts, 8, IM_COL32_WHITE);
    ShadeVerts(dl, v0, [&](ImVec2 p) {
        return VGrad(p.y, a.y, b.y, Shade(base, 1.3f), Shade(base, 0.68f));
    });

    // Chiselled bevel: light along the top and left, dark along the bottom and right.
    {
        ImVec2 in[8];
        Chamfer(ImVec2(a.x + Sc(3.f), a.y + Sc(3.f)), ImVec2(b.x - Sc(3.f), b.y - Sc(3.f)),
                std::max(0.f, c - Sc(1.5f)), in);
        const ImVec2 lit[5]  = { in[6], in[7], in[0], in[1], in[2] };
        const ImVec2 dark[5] = { in[2], in[3], in[4], in[5], in[6] };
        dl->AddPolyline(lit,  5, IM_COL32(255, 248, 230, 60), 0, Sc(1.5f));
        dl->AddPolyline(dark, 5, IM_COL32(0, 0, 0, 90),       0, Sc(1.5f));
    }

    // A crack, different on every tablet.
    {
        const float  h0 = (iid % 997) / 997.f, h1 = (iid % 991) / 991.f;
        const float  cx = a.x + size.x * (0.12f + 0.76f * h0);
        const ImVec2 crack[4] = { ImVec2(cx, a.y + Sc(1.f)),
                                  ImVec2(cx + Sc(4.f) * (h1 - 0.5f) * 2.f, a.y + size.y * 0.18f),
                                  ImVec2(cx - Sc(3.f), a.y + size.y * 0.30f),
                                  ImVec2(cx + Sc(2.f), a.y + size.y * (0.38f + 0.15f * h1)) };
        dl->AddPolyline(crack, 4, IM_COL32(0, 0, 0, 110), 0, Sc(1.2f));
    }

    dl->AddPolyline(pts, 8, selected ? kGold : kBronze, ImDrawFlags_Closed, Sc(2.5f));
    const float my = (a.y + b.y) * 0.5f;
    Stud(dl, ImVec2(a.x + Sc(9.f), my), Sc(2.8f));
    Stud(dl, ImVec2(b.x - Sc(9.f), my), Sc(2.8f));

    ImFont*      f  = BodyFont();
    const ImVec2 ts = f->CalcTextSizeA(fontSize, FLT_MAX, 0.f, label);
    const ImVec2 tp((a.x + b.x - ts.x) * 0.5f, my - ts.y * 0.52f);
    if (hovered || selected) CarvedText(dl, f, fontSize, tp, label, kGoldHi, kGold);
    else                     CarvedText(dl, f, fontSize, tp, label, kEngrave, IM_COL32(170, 160, 140, 255));

    if (hovered && pointer)
        Sword(dl, ImVec2(a.x - Sc(6.f) - Sc(4.f) * (0.5f + 0.5f * sinf(t * 8.f)), my), Sc(58.f));
    return clicked;
}

// Bone skull with glowing green eyes, centred on c; u is its height scale.
void Skull(ImDrawList* dl, ImVec2 c, float u, float t) {
    const ImU32 bone = IM_COL32(226, 214, 186, 255), boneDark = IM_COL32(140, 126, 100, 255);
    // Jaw, cranium, cheekbones.
    dl->AddRectFilled(ImVec2(c.x - u * 0.30f, c.y + u * 0.02f), ImVec2(c.x + u * 0.30f, c.y + u * 0.50f), kShadow, u * 0.12f);
    dl->AddRectFilled(ImVec2(c.x - u * 0.28f, c.y + u * 0.02f), ImVec2(c.x + u * 0.28f, c.y + u * 0.47f), boneDark, u * 0.11f);
    dl->AddCircleFilled(ImVec2(c.x, c.y - u * 0.12f), u * 0.47f, kShadow, 32);
    dl->AddCircleFilled(ImVec2(c.x, c.y - u * 0.12f), u * 0.45f, boneDark, 32);
    dl->AddCircleFilled(ImVec2(c.x - u * 0.04f, c.y - u * 0.17f), u * 0.40f, bone, 32);
    dl->AddRectFilled(ImVec2(c.x - u * 0.26f, c.y + u * 0.02f), ImVec2(c.x + u * 0.22f, c.y + u * 0.30f), bone, u * 0.08f);

    // Eye sockets with a slow, pulsing glow.
    const float g = 0.6f + 0.4f * sinf(t * 2.2f);
    for (int side = -1; side <= 1; side += 2) {
        const ImVec2 e(c.x + side * u * 0.18f, c.y - u * 0.06f);
        dl->AddCircleFilled(e, u * 0.14f, kShadow, 20);
        for (int i = 3; i >= 1; --i)
            dl->AddCircleFilled(e, u * 0.05f * (1.f + i * 0.6f), WithAlpha(kEyeGlow, 0.12f * g), 16);
        dl->AddCircleFilled(e, u * 0.05f, WithAlpha(kEyeGlow, 0.55f + 0.45f * g), 12);
    }
    // Nose and teeth.
    dl->AddTriangleFilled(ImVec2(c.x, c.y + u * 0.08f), ImVec2(c.x - u * 0.06f, c.y + u * 0.20f),
                          ImVec2(c.x + u * 0.06f, c.y + u * 0.20f), kShadow);
    dl->AddLine(ImVec2(c.x - u * 0.20f, c.y + u * 0.36f), ImVec2(c.x + u * 0.20f, c.y + u * 0.36f), kShadow, Sc(1.2f));
    for (int i = -2; i <= 2; ++i)
        dl->AddLine(ImVec2(c.x + i * u * 0.08f, c.y + u * 0.28f), ImVec2(c.x + i * u * 0.08f, c.y + u * 0.44f),
                    kShadow, Sc(1.2f));
}

// Iron seal riveted to a corner: bronze rim, dark iron face, carved red X.
// Glows blood-red when hovered.  True when clicked.
bool CloseSeal(const char* id, ImVec2 c, float r) {
    ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(r * 2.f, r * 2.f));
    const bool hovered = ImGui::IsItemHovered();
    const bool held    = ImGui::IsItemActive();
    const float rr     = r * (held ? 0.92f : hovered ? 1.06f : 1.f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered)
        for (int i = 5; i >= 1; --i)
            dl->AddCircleFilled(c, rr + Sc(2.5f) * i, WithAlpha(IM_COL32(220, 30, 30, 255), 0.08f), 32);
    dl->AddCircleFilled(ImVec2(c.x + Sc(2.f), c.y + Sc(4.f)), rr, IM_COL32(0, 0, 0, 140), 32);
    dl->AddCircleFilled(c, rr, kShadow, 32);
    const int v0 = dl->VtxBuffer.Size;
    dl->AddCircleFilled(c, rr - Sc(1.5f), IM_COL32_WHITE, 32);
    ShadeVerts(dl, v0, [&](ImVec2 p) { return VGrad(p.y, c.y - rr, c.y + rr, kBronzeHi, kBronzeLo); });
    dl->AddCircleFilled(c, rr * 0.70f, kShadow, 32);
    dl->AddCircleFilled(c, rr * 0.66f, IM_COL32(48, 44, 54, 255), 32);
    for (int i = 0; i < 6; ++i) {
        const float a = i * 1.0471976f + 0.5235988f;
        Stud(dl, ImVec2(c.x + cosf(a) * rr * 0.84f, c.y + sinf(a) * rr * 0.84f), Sc(1.6f));
    }
    const ImU32 xCol = hovered ? IM_COL32(255, 70, 60, 255) : IM_COL32(190, 30, 30, 255);
    for (int pass = 0; pass < 2; ++pass) {
        const float arm = rr * (pass == 0 ? 0.40f : 0.34f);
        const float w   = rr * (pass == 0 ? 0.24f : 0.14f);
        for (int k = 0; k < 2; ++k) {
            const float  a = 0.7853982f + k * 1.5707963f;
            const ImVec2 d(cosf(a) * arm, sinf(a) * arm);
            dl->AddLine(ImVec2(c.x - d.x, c.y - d.y), ImVec2(c.x + d.x, c.y + d.y), pass == 0 ? kShadow : xCol, w);
        }
    }
    return clicked;
}

// The Sword & Sorcery look for the shared paged menu.
class Style : public PagedMenuStyle {
public:
    ImVec2  PanelSize()   const override { return ImVec2(700.f, 700.f); }
    ImVec2  Padding()     const override { return ImVec2(42.f, 28.f); }
    float   TitleSpace()  const override { return 82.f; }
    float   FooterSpace() const override { return 64.f; }
    float   FooterRow()   const override { return 66.f; }
    float   Gutter()      const override { return 70.f; }   // room for the hover sword
    float   ColumnGap()   const override { return 74.f; }
    float   GroupButtonHeight() const override { return 50.f; }
    float   ItemButtonHeight()  const override { return 42.f; }
    ImFont* Font()        const override { return BodyFont(); }
    float   FontSize()    const override { return 17.f; }

    // The panel slams down like a dungeon gate; pages rumble in.
    ImVec2 OpenOffset(float t) const override { return ImVec2(0.f, (1.f - EaseOutBounce(t / 0.6f)) * Sc(-140.f)); }
    ImVec2 PageOffset(float t) const override {
        return ImVec2(t < 0.35f ? sinf(t * 70.f) * Sc(5.f) * (1.f - t / 0.35f) : 0.f, 0.f);
    }

    void DrawPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, int, float t) override {
        const float th = Sc(18.f);   // frame thickness
        Frame(dl, a, b, th);
        Wall(dl, ImVec2(a.x + th, a.y + th), ImVec2(b.x - th, b.y - th));
        Torch(dl, ImVec2(a.x + Sc(80.f), a.y + Sc(72.f)), t, 0.f);
        Torch(dl, ImVec2(b.x - Sc(80.f), a.y + Sc(72.f)), t, 2.7f);
    }

    // Shrunk if needed to fit between the torches.
    void DrawTitle(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* title, int, float t) override {
        const float full = Sc(56.f);
        const float maxW = (b.x - a.x) - Sc(220.f);
        float size = full;
        float w    = TitleFont()->CalcTextSizeA(size, FLT_MAX, 0.f, title).x;
        if (w > maxW) { size *= maxW / w; w = maxW; }
        MoltenTitle(dl, size, ImVec2(a.x + (b.x - a.x - w) * 0.5f, a.y + Sc(28.f) + (full - size) * 0.5f), title, t);
    }

    bool GroupButton(const char* id, const char* label, ImVec2 size, int, bool selected, Kind kind) override {
        return Tablet(id, label, size, Sc(kind == Kind::Group ? 19.f : 17.f), selected);
    }
    bool  BackButton(const char* id) override { return Tablet(id, "Back", FooterButton(), Sc(18.f), false, kStone, false); }
    float QuitWidth() const override { return FooterButton().x; }
    bool  QuitButton(const char* id) override {
        return Tablet(id, "Quit", FooterButton(), Sc(18.f), false, IM_COL32(112, 40, 42, 255), false);
    }
    // A skull keeps watch between BACK and QUIT.
    void DrawFooter(ImDrawList* dl, ImVec2 a, ImVec2 b, int, float t) override {
        Skull(dl, ImVec2((a.x + b.x) * 0.5f, a.y + Sc(18.f)), Sc(44.f), t);
    }
    ImVec2 CloseCenter(ImVec2 a, ImVec2 b) const override { return ImVec2(b.x - Sc(36.f), a.y + Sc(36.f)); }
    bool   CloseButton(const char* id, ImVec2 c) override { return CloseSeal(id, c, Sc(22.f)); }

private:
    static ImVec2 FooterButton() { return ImVec2(Sc(132.f), Sc(40.f)); }
};

} // namespace sorcery

// ---------------------------------------------------------------------------
// Teletext layout — a Ceefax-style teletext page.
//   • The page is a 40 × 25 grid of character cells in the eight teletext
//     colours, set in Bedstead (a recreation of the SAA5050 teletext chip).
//   • Header row: page number, service name, date and a ticking clock.  On a
//     page change the header's page counter rolls while the page "loads",
//     then the page appears.
//   • A double-height title banner with mosaic-graphics trim, coloured per
//     page; the index lists every group with dot leaders to its page number.
//   • The fastext row along the bottom: red Index (back) and cyan Quit.
// ---------------------------------------------------------------------------
namespace teletext {

constexpr ImU32 kBlack   = IM_COL32(  0,   0,   0, 255);
constexpr ImU32 kRed     = IM_COL32(255,   0,   0, 255);
constexpr ImU32 kGreen   = IM_COL32(  0, 255,   0, 255);
constexpr ImU32 kYellow  = IM_COL32(255, 255,   0, 255);
constexpr ImU32 kBlue    = IM_COL32(  0,   0, 255, 255);
constexpr ImU32 kMagenta = IM_COL32(255,   0, 255, 255);
constexpr ImU32 kCyan    = IM_COL32(  0, 255, 255, 255);
constexpr ImU32 kWhite   = IM_COL32(255, 255, 255, 255);

constexpr int   kCols = 40, kRows = 25;
constexpr float kSearchTime = 0.3f;   // seconds a page "loads" before it appears

ImFont* Font() { return StyleFonts::bedstead ? StyleFonts::bedstead : ImGui::GetFont(); }

// Text laid out in character cells from the top-left corner pos, each
// character exactly cell.x wide and cell.y tall (two rows for double
// height): drawn at the font size that gives the cell width, then its
// vertices stretched to the cell height.
void CellText(ImDrawList* dl, ImVec2 pos, ImVec2 cell, ImU32 col, const char* text) {
    ImFont*     f       = Font();
    const float advance = f->CalcTextSizeA(100.f, FLT_MAX, 0.f, "M").x / 100.f;   // per px of size
    const float size    = cell.x / advance;
    const int   v0      = dl->VtxBuffer.Size;
    dl->AddText(f, size, pos, col, text);
    const float k = cell.y / size;
    for (int i = v0; i < dl->VtxBuffer.Size; ++i)
        dl->VtxBuffer[i].pos.y = pos.y + (dl->VtxBuffer[i].pos.y - pos.y) * k;
}

// A mosaic character: a 2 × 3 grid of blocks, bit 0 top-left … bit 5
// bottom-right.
void Mosaic(ImDrawList* dl, ImVec2 p, ImVec2 cell, int bits, ImU32 col) {
    const float bw = cell.x * 0.5f, bh = cell.y / 3.f;
    for (int i = 0; i < 6; ++i)
        if (bits & (1 << i)) {
            const float x = p.x + (i % 2) * bw, y = p.y + (i / 2) * bh;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + bw, y + bh), col);
        }
}

// The teletext look for the shared paged menu.
class Style : public PagedMenuStyle {
public:
    ImVec2  PanelSize()   const override { return ImVec2(720.f, 600.f); }   // 40 × 25 cells of 18 × 24
    ImVec2  Padding()     const override { return ImVec2(36.f, 0.f); }      // content starts two columns in
    float   TitleSpace()  const override { return 92.f; }    // rows 0–3, less the item spacing
    float   FooterSpace() const override { return 50.f; }
    float   FooterRow()   const override { return 24.f; }    // the fastext row (row 24)
    int     Columns()     const override { return 1; }
    float   GroupButtonHeight() const override { return 20.f; }   // + 4 item spacing = one row
    float   ItemButtonHeight()  const override { return 20.f; }
    float   SliderWidth() const override { return 0.5f; }
    ImFont* Font()        const override { return teletext::Font(); }
    float   FontSize()    const override { return 18.f; }

    // Once the page is found it paints in, top to bottom, in a blink.
    float PageReveal(float t) const override { return std::clamp((t - kSearchTime) / 0.12f, 0.f, 1.f); }

    void DrawPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, int, float) override {
        cell_ = ImVec2((b.x - a.x) / kCols, (b.y - a.y) / kRows);
        dl->AddRectFilled(a, b, kBlack);
        dl->AddRect(a, b, IM_COL32(70, 70, 70, 255));
    }

    void DrawTitle(ImDrawList* dl, ImVec2 a, ImVec2, const char* title, int pageIndex, float t) override {
        const float since = t - pageTime;
        const bool  found = since >= kSearchTime;
        const int   num   = PageNumber(pageIndex);

        // Header: requested page, service name with the (rolling) page
        // counter, date, clock.
        const time_t now = time(nullptr);
        tm lt{};
        localtime_s(&lt, &now);
        char date[32], buf[64];
        strftime(date, sizeof(date), "%a %d %b", &lt);
        snprintf(buf, sizeof(buf), "P%03d", num);
        CellText(dl, At(a, 1, 0), cell_, kWhite, buf);
        snprintf(buf, sizeof(buf), "SeeRT %03d %s", found ? num : 100 + static_cast<int>(since * 230.f) % 100, date);
        CellText(dl, At(a, 6, 0), cell_, kWhite, buf);
        snprintf(buf, sizeof(buf), "%02d:%02d/%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
        CellText(dl, At(a, 27, 0), cell_, kYellow, buf);
        if (!found) return;

        // Title banner, rows 1–2: mosaic staircases either side of a
        // double-height title, coloured per page.
        static constexpr ImU32 kBanner[3] = { kBlue, kRed, kMagenta };
        static constexpr ImU32 kTrim[3]   = { kCyan, kYellow, kCyan };
        static constexpr ImU32 kTitle[3]  = { kYellow, kWhite, kWhite };
        const int k = (pageIndex + 1) % 3;
        dl->AddRectFilled(At(a, 0, 1), At(a, kCols, 3), kBanner[k]);
        static constexpr int kStairTop[6]    = { 0x00, 0x00, 0x30, 0x3C, 0x3F, 0x3F };
        static constexpr int kStairBottom[6] = { 0x30, 0x3C, 0x3F, 0x3F, 0x3F, 0x3F };
        for (int c = 0; c < 6; ++c) {
            Mosaic(dl, At(a, 1 + c, 1), cell_, kStairTop[c],    kTrim[k]);
            Mosaic(dl, At(a, 1 + c, 2), cell_, kStairBottom[c], kTrim[k]);
            Mosaic(dl, At(a, 38 - c, 1), cell_, MirrorMosaic(kStairTop[c]),    kTrim[k]);
            Mosaic(dl, At(a, 38 - c, 2), cell_, MirrorMosaic(kStairBottom[c]), kTrim[k]);
        }
        const int len = static_cast<int>(strlen(title));
        CellText(dl, At(a, (kCols - len) / 2, 1), ImVec2(cell_.x, cell_.y * 2.f), kTitle[k], title);
    }

    // An index line: name, dot leaders, page number.  Hovering highlights the row.
    bool GroupButton(const char* id, const char* label, ImVec2 size, int index, bool selected, Kind kind) override {
        const ImVec2 pos     = ImGui::GetCursorScreenPos();
        const bool   clicked = ImGui::InvisibleButton(id, size);
        const bool   hovered = ImGui::IsItemHovered();
        ImDrawList*  dl      = ImGui::GetWindowDrawList();

        const float top  = pos.y - (cell_.y - size.y) * 0.5f;   // the whole character row
        const int   cols = static_cast<int>(size.x / cell_.x);
        const int   num  = (kind == Kind::Group ? 101 : kind == Kind::Preset ? 201 : 301) + index;
        if (hovered) dl->AddRectFilled(ImVec2(pos.x, top), ImVec2(pos.x + cols * cell_.x, top + cell_.y), kBlue);

        std::string name = label;
        if (static_cast<int>(name.size()) > cols - 6) name.resize(cols - 6);
        CellText(dl, ImVec2(pos.x, top), cell_, selected ? kGreen : hovered ? kYellow : kWhite, name.c_str());
        const int dots = cols - static_cast<int>(name.size()) - 5;
        if (dots > 0)
            CellText(dl, ImVec2(pos.x + (name.size() + 1) * cell_.x, top), cell_, kCyan, std::string(dots, '.').c_str());
        char n[8];
        snprintf(n, sizeof(n), "%d", num);
        CellText(dl, ImVec2(pos.x + (cols - 3) * cell_.x, top), cell_, hovered ? kWhite : kYellow, n);
        return clicked;
    }

    bool  BackButton(const char* id) override { return Fastext(id, "Index", kRed); }
    float QuitWidth() const override { return cell_.x * kFastextCells; }
    bool  QuitButton(const char* id) override { return Fastext(id, "Quit", kCyan); }

    void DrawFooter(ImDrawList* dl, ImVec2 a, ImVec2, int pageIndex, float) override {
        const char* hint = pageIndex < 0 ? "Pick a page" : "Esc for index";
        CellText(dl, ImVec2(a.x + (kCols - static_cast<int>(strlen(hint))) / 2 * cell_.x, a.y), cell_, kGreen, hint);
    }

    // A red three-cell box at the end of the header row.
    ImVec2 CloseCenter(ImVec2 a, ImVec2 b) const override { return ImVec2(b.x - cell_.x * 1.5f, a.y + cell_.y * 0.5f); }
    bool   CloseButton(const char* id, ImVec2 c) override {
        const ImVec2 p(c.x - cell_.x * 1.5f, c.y - cell_.y * 0.5f);
        ImGui::SetCursorScreenPos(p);
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(cell_.x * 3.f, cell_.y));
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + cell_.x * 3.f, p.y + cell_.y), hovered ? kYellow : kRed);
        CellText(dl, ImVec2(p.x + cell_.x, p.y), cell_, hovered ? kBlack : kWhite, "X");
        return clicked;
    }

private:
    static constexpr int kFastextCells = 8;

    static int PageNumber(int pageIndex) { return pageIndex < 0 ? 100 : 101 + pageIndex; }

    // Top-left of the cell at (col, row).
    ImVec2 At(ImVec2 a, int col, int row) const { return ImVec2(a.x + col * cell_.x, a.y + row * cell_.y); }

    // Swaps the left and right blocks of a mosaic character.
    static int MirrorMosaic(int bits) {
        int out = 0;
        for (int i = 0; i < 6; ++i)
            if (bits & (1 << i)) out |= 1 << (i ^ 1);
        return out;
    }

    // A fastext link: coloured text, inverted when hovered.
    bool Fastext(const char* id, const char* label, ImU32 col) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 size(cell_.x * kFastextCells, cell_.y);
        const bool   clicked = ImGui::InvisibleButton(id, size);
        const bool   hovered = ImGui::IsItemHovered();
        ImDrawList*  dl      = ImGui::GetWindowDrawList();
        if (hovered) dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), col);
        CellText(dl, pos, cell_, hovered ? kBlack : col, label);
        return clicked;
    }

    ImVec2 cell_ = ImVec2(18.f, 24.f);   // set from the panel each frame
};

} // namespace teletext

// ---------------------------------------------------------------------------
// Vector Monitor layout — an Asteroids / Tempest-style vector display.
//   • Everything is drawn as glowing phosphor beams on black, with a stroke
//     font built from line segments; no filled shapes.
//   • A twinkling starfield with slowly tumbling wireframe asteroids behind.
//   • Hovering a button points the little ship at it, thruster flickering.
//   • The panel switches on like a CRT — a bright line opening to full
//     height — and the footer keeps score: 150 points per page visited.
// ---------------------------------------------------------------------------
namespace vecmon {

constexpr ImU32 kBeam = IM_COL32(170, 230, 255, 255);   // phosphor blue-white
constexpr ImU32 kHot  = IM_COL32(255, 236, 120, 255);   // hover
constexpr ImU32 kGo   = IM_COL32(110, 255, 150, 255);   // selected / back
constexpr ImU32 kStop = IM_COL32(255,  96,  96, 255);   // quit / close
constexpr ImU32 kDim  = IM_COL32( 80, 130, 175, 255);

ImFont* Font() { return StyleFonts::shareTech ? StyleFonts::shareTech : ImGui::GetFont(); }

// Stroke font: each glyph is a set of strokes through points on a 4 × 6 grid,
// written "xy xy …"; "|" lifts the pen.  Lowercase draws as uppercase.
const char* Glyph(char c) {
    switch (toupper(static_cast<unsigned char>(c))) {
    case 'A': return "06 02 20 42 46|03 43";
    case 'B': return "06 00 30 41 42 33 03|33 44 45 36 06";
    case 'C': return "40 00 06 46";
    case 'D': return "06 00 20 42 44 26 06";
    case 'E': return "40 00 06 46|03 33";
    case 'F': return "40 00 06|03 33";
    case 'G': return "30 00 06 46 43 23";
    case 'H': return "00 06|40 46|03 43";
    case 'I': return "00 40|20 26|06 46";
    case 'J': return "40 44 26 16 04";
    case 'K': return "00 06|40 03 46";
    case 'L': return "00 06 46";
    case 'M': return "06 00 23 40 46";
    case 'N': return "06 00 46 40";
    case 'O': return "00 40 46 06 00";
    case 'P': return "06 00 40 43 03";
    case 'Q': return "00 40 44 26 06 00|24 46";
    case 'R': return "06 00 40 43 03|13 46";
    case 'S': return "40 00 03 43 46 06";
    case 'T': return "00 40|20 26";
    case 'U': return "00 06 46 40";
    case 'V': return "00 26 40";
    case 'W': return "00 06 24 46 40";
    case 'X': return "00 46|40 06";
    case 'Y': return "00 23 40|23 26";
    case 'Z': return "00 40 06 46";
    case '0': return "00 40 46 06 00|06 40";
    case '1': return "11 20 26|06 46";
    case '2': return "00 40 43 03 06 46";
    case '3': return "00 40 46 06|03 43";
    case '4': return "00 03 43|40 46";
    case '5': return "40 00 03 43 46 06";
    case '6': return "00 06 46 43 03";
    case '7': return "00 40 46";
    case '8': return "00 40 46 06 00|03 43";
    case '9': return "43 03 00 40 46";
    case '-': return "13 33";
    case '+': return "13 33|22 24";
    case '.': return "26";
    case ',': return "25 16";
    case ':': return "22|25";
    case '\'': return "20 21";
    case '/': return "06 40";
    case '(': return "30 12 14 36";
    case ')': return "10 32 34 16";
    case '<': return "30 03 36";
    case '>': return "10 43 16";
    case '&': return "46 02 11 21 32 05 16 26 44";
    case '!': return "20 24|26";
    case ' ': return "";
    default:  return "00 40 42 23 24|26";   // '?'
    }
}

// A glowing beam through pts (a dot for a single point): a wide faint halo,
// a softer glow and a near-white core.  Drawn a segment at a time — thick
// polylines grow long miter spikes at sharp corners — so the overlap makes
// each vertex a touch brighter, like a real beam dwelling there.
void Beam(ImDrawList* dl, const ImVec2* pts, int n, ImU32 col, float w, bool closed = false, float intensity = 1.f) {
    const ImU32 core = WithAlpha(Mix(col, IM_COL32_WHITE, 0.45f), intensity);
    if (n == 1) {
        dl->AddCircleFilled(pts[0], w * 2.4f, WithAlpha(col, 0.16f * intensity), 8);
        dl->AddCircleFilled(pts[0], w * 0.9f, core, 8);
        return;
    }
    const int    segs     = closed ? n : n - 1;
    const float  width[3] = { w * 5.f, w * 2.4f, w };
    const ImU32  cols[3]  = { WithAlpha(col, 0.09f * intensity), WithAlpha(col, 0.26f * intensity), core };
    for (int pass = 0; pass < 3; ++pass)
        for (int i = 0; i < segs; ++i)
            dl->AddLine(pts[i], pts[(i + 1) % n], cols[pass], width[pass]);
}

float TextWidth(float h, const char* text) {
    const size_t n = strlen(text);
    return n ? (n * 6.f - 2.f) * h / 6.f : 0.f;
}

// Stroke-font text, h tall, with its top-left at pos.
void Text(ImDrawList* dl, ImVec2 pos, float h, ImU32 col, const char* text, float intensity = 1.f) {
    const float u = h / 6.f, w = std::max(1.f, h * 0.075f);
    for (int i = 0; text[i]; ++i) {
        const float ox = pos.x + i * 6.f * u;
        ImVec2 pts[16];
        int    n = 0;
        for (const char* p = Glyph(text[i]); ; ) {
            if (*p == '|' || *p == '\0') {
                if (n) Beam(dl, pts, n, col, w, false, intensity);
                n = 0;
                if (*p == '\0') break;
                ++p;
            } else if (*p == ' ') {
                ++p;
            } else {
                if (n < 16) pts[n++] = ImVec2(ox + (p[0] - '0') * u, pos.y + (p[1] - '0') * u);
                p += 2;
            }
        }
    }
}

// The player's ship, nose at `nose`, pointing right; thruster flickering.
void Ship(ImDrawList* dl, ImVec2 nose, float s, ImU32 col, float t) {
    const ImVec2 hull[4] = { nose, ImVec2(nose.x - 2.f * s, nose.y - 0.9f * s),
                             ImVec2(nose.x - 1.5f * s, nose.y), ImVec2(nose.x - 2.f * s, nose.y + 0.9f * s) };
    Beam(dl, hull, 4, col, Sc(1.3f), true);
    if (fmodf(t * 17.f, 1.f) < 0.65f) {
        const ImVec2 flame[3] = { ImVec2(nose.x - 1.65f * s, nose.y - 0.4f * s),
                                  ImVec2(nose.x - (2.5f + 0.4f * Hash(floorf(t * 30.f), 1.f)) * s, nose.y),
                                  ImVec2(nose.x - 1.65f * s, nose.y + 0.4f * s) };
        Beam(dl, flame, 3, kHot, Sc(1.1f));
    }
}

// Rectangle with cut corners (clockwise), the outline of every button.
void Bevel(ImVec2 a, ImVec2 b, float c, ImVec2 out[8]) {
    out[0] = ImVec2(a.x + c, a.y);  out[1] = ImVec2(b.x - c, a.y);
    out[2] = ImVec2(b.x, a.y + c);  out[3] = ImVec2(b.x, b.y - c);
    out[4] = ImVec2(b.x - c, b.y);  out[5] = ImVec2(a.x + c, b.y);
    out[6] = ImVec2(a.x, b.y - c);  out[7] = ImVec2(a.x, a.y + c);
}

// Outlined button with a stroke-font label (shrunk to fit).  True when clicked.
bool Button(const char* id, const char* label, ImVec2 size, float textH, ImU32 col, bool selected = false) {
    const ImVec2 pos     = ImGui::GetCursorScreenPos();
    const bool   clicked = ImGui::InvisibleButton(id, size);
    const bool   hovered = ImGui::IsItemHovered();
    const bool   held    = ImGui::IsItemActive();
    ImDrawList*  dl      = ImGui::GetWindowDrawList();

    const ImU32 c = selected ? kGo : hovered ? kHot : col;
    ImVec2 pts[8];
    Bevel(pos, ImVec2(pos.x + size.x, pos.y + size.y), std::min(Sc(8.f), size.y * 0.3f), pts);
    dl->AddConvexPolyFilled(pts, 8, IM_COL32(0, 3, 8, 235));   // no beam inside: rocks pass behind
    if (hovered) dl->AddConvexPolyFilled(pts, 8, WithAlpha(c, 0.07f));
    Beam(dl, pts, 8, c, Sc(held ? 1.8f : 1.2f), true, hovered || selected ? 1.f : 0.75f);

    float       h = textH;
    const float room = size.x - Sc(20.f);
    float       w = TextWidth(h, label);
    if (w > room) { h *= room / w; w = room; }
    Text(dl, ImVec2(pos.x + (size.x - w) * 0.5f, pos.y + (size.y - h) * 0.5f), h, c, label,
         hovered || selected ? 1.f : 0.8f);
    return clicked;
}

// The vector-display look for the shared paged menu.
class Style : public PagedMenuStyle {
public:
    ImVec2  PanelSize()   const override { return ImVec2(700.f, 680.f); }
    ImVec2  Padding()     const override { return ImVec2(40.f, 26.f); }
    float   TitleSpace()  const override { return 80.f; }
    float   FooterSpace() const override { return 62.f; }
    float   FooterRow()   const override { return 58.f; }
    float   Gutter()      const override { return 40.f; }   // room for the ship
    float   ColumnGap()   const override { return 52.f; }
    float   GroupButtonHeight() const override { return 44.f; }
    float   ItemButtonHeight()  const override { return 38.f; }
    ImFont* Font()        const override { return vecmon::Font(); }
    float   FontSize()    const override { return 18.f; }
    std::string GroupLabel(int section) const override { return kSectionNames[section]; }

    // Switches on like a CRT: a bright line opening to full height.
    float OpenReveal(float t) const override { return std::clamp(t / 0.22f, 0.f, 1.f); }

    void DrawPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, int, float t) override {
        dl->AddRectFilled(a, b, IM_COL32(0, 3, 8, 240));
        dl->PushClipRect(a, b, true);

        // Starfield.
        for (int i = 0; i < 70; ++i) {
            const ImVec2 p(a.x + Hash(static_cast<float>(i), 1.f) * (b.x - a.x),
                           a.y + Hash(static_cast<float>(i), 2.f) * (b.y - a.y));
            const float twinkle = 0.25f + 0.75f * (0.5f + 0.5f * sinf(t * (1.f + Hash(static_cast<float>(i), 3.f) * 3.f) + i));
            dl->AddCircleFilled(p, Sc(0.9f), WithAlpha(kBeam, 0.35f * twinkle), 4);
        }
        // Tumbling asteroids, drifting and wrapping around the panel.
        for (int r = 0; r < 4; ++r) {
            const float fr     = static_cast<float>(r);
            const float radius = Sc(18.f + 22.f * Hash(fr, 4.f));
            const float spanX  = (b.x - a.x) + radius * 2.f, spanY = (b.y - a.y) + radius * 2.f;
            float x = fmodf(Hash(fr, 7.f) * spanX + (Hash(fr, 5.f) - 0.5f) * Sc(30.f) * t, spanX);
            float y = fmodf(Hash(fr, 8.f) * spanY + (Hash(fr, 6.f) - 0.5f) * Sc(30.f) * t, spanY);
            if (x < 0.f) x += spanX;
            if (y < 0.f) y += spanY;
            const ImVec2 c(a.x - radius + x, a.y - radius + y);
            const float  rot = t * (Hash(fr, 9.f) - 0.5f) * 1.2f;
            ImVec2 pts[10];
            for (int k = 0; k < 10; ++k) {
                const float ang = rot + k * 0.6283185f;
                const float rr  = radius * (0.72f + 0.28f * Hash(fr * 13.f + k, 10.f));
                pts[k] = ImVec2(c.x + cosf(ang) * rr, c.y + sinf(ang) * rr);
            }
            Beam(dl, pts, 10, kDim, Sc(1.f), true, 0.5f);
        }
        dl->PopClipRect();

        // Frame: a dim rectangle with bright corner brackets.
        const ImVec2 i0(a.x + Sc(6.f), a.y + Sc(6.f)), i1(b.x - Sc(6.f), b.y - Sc(6.f));
        const ImVec2 box[4] = { i0, ImVec2(i1.x, i0.y), i1, ImVec2(i0.x, i1.y) };
        Beam(dl, box, 4, kDim, Sc(1.f), true, 0.7f);
        const float L = Sc(28.f);
        const ImVec2 corners[4][3] = {
            { ImVec2(i0.x, i0.y + L), i0, ImVec2(i0.x + L, i0.y) },
            { ImVec2(i1.x - L, i0.y), ImVec2(i1.x, i0.y), ImVec2(i1.x, i0.y + L) },
            { ImVec2(i1.x, i1.y - L), i1, ImVec2(i1.x - L, i1.y) },
            { ImVec2(i0.x + L, i1.y), ImVec2(i0.x, i1.y), ImVec2(i0.x, i1.y - L) } };
        for (const auto& c : corners) Beam(dl, c, 3, kBeam, Sc(1.6f));
    }

    // Big stroke-font title with a faint beam flicker, underlined.
    void DrawTitle(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* title, int, float t) override {
        float       h    = Sc(34.f);
        const float maxW = (b.x - a.x) - Sc(160.f);
        float       w    = TextWidth(h, title);
        if (w > maxW) { h *= maxW / w; w = maxW; }
        const float x = a.x + (b.x - a.x - w) * 0.5f, y = a.y + Sc(30.f);
        Text(dl, ImVec2(x, y), h, kBeam, title, 0.9f + 0.1f * sinf(t * 37.f) * sinf(t * 13.f));
        const ImVec2 line[2] = { ImVec2(x - Sc(24.f), a.y + Sc(78.f)), ImVec2(x + w + Sc(24.f), a.y + Sc(78.f)) };
        Beam(dl, line, 2, kDim, Sc(1.f), false, 0.8f);
    }

    bool GroupButton(const char* id, const char* label, ImVec2 size, int, bool selected, Kind kind) override {
        const ImVec2 pos     = ImGui::GetCursorScreenPos();
        const bool   clicked = Button(id, label, size, Sc(kind == Kind::Group ? 14.f : 12.f), kBeam, selected);
        if (ImGui::IsItemHovered())
            Ship(ImGui::GetWindowDrawList(), ImVec2(pos.x - Sc(8.f), pos.y + size.y * 0.5f), Sc(9.f), kHot,
                 static_cast<float>(ImGui::GetTime()));
        return clicked;
    }

    bool  BackButton(const char* id) override { return Button(id, "< BACK", FooterButton(), Sc(13.f), kGo); }
    float QuitWidth() const override { return FooterButton().x; }
    bool  QuitButton(const char* id) override { return Button(id, "QUIT", FooterButton(), Sc(13.f), kStop); }

    // Score (150 per page visited) and three reserve ships.
    void DrawFooter(ImDrawList* dl, ImVec2 a, ImVec2 b, int pageIndex, float) override {
        if (pageIndex != lastPage_) {
            if (pageIndex >= 0) score_ += 150;
            lastPage_ = pageIndex;
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "SCORE %06d", score_);
        const float h = Sc(12.f), w = TextWidth(h, buf);
        const float cx = (a.x + b.x) * 0.5f;
        Text(dl, ImVec2(cx - w * 0.5f, a.y + Sc(4.f)), h, kBeam, buf, 0.85f);
        for (int i = 0; i < 3; ++i) {
            const float  s = Sc(5.f);
            const ImVec2 n(cx - Sc(16.f) + i * Sc(16.f), a.y + Sc(24.f));
            const ImVec2 up[4] = { n, ImVec2(n.x + 0.9f * s, n.y + 2.f * s), ImVec2(n.x, n.y + 1.5f * s),
                                   ImVec2(n.x - 0.9f * s, n.y + 2.f * s) };
            Beam(dl, up, 4, kBeam, Sc(1.f), true, 0.8f);
        }
    }

    ImVec2 CloseCenter(ImVec2 a, ImVec2 b) const override { return ImVec2(b.x - Sc(32.f), a.y + Sc(32.f)); }
    bool   CloseButton(const char* id, ImVec2 c) override {
        const float r = Sc(13.f);
        ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(r * 2.f, r * 2.f));
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 col = hovered ? kHot : kStop;
        const ImVec2 box[4] = { ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y - r), ImVec2(c.x + r, c.y + r),
                                ImVec2(c.x - r, c.y + r) };
        dl->AddRectFilled(box[0], box[2], IM_COL32(0, 3, 8, 235));
        if (hovered) dl->AddRectFilled(box[0], box[2], WithAlpha(col, 0.08f));
        Beam(dl, box, 4, col, Sc(1.2f), true);
        const float x = r * 0.5f;
        const ImVec2 d1[2] = { ImVec2(c.x - x, c.y - x), ImVec2(c.x + x, c.y + x) };
        const ImVec2 d2[2] = { ImVec2(c.x + x, c.y - x), ImVec2(c.x - x, c.y + x) };
        Beam(dl, d1, 2, col, Sc(1.4f));
        Beam(dl, d2, 2, col, Sc(1.4f));
        return clicked;
    }

private:
    static ImVec2 FooterButton() { return ImVec2(Sc(120.f), Sc(34.f)); }

    int score_    = 0;
    int lastPage_ = -1;
};

} // namespace vecmon

// ---------------------------------------------------------------------------
// Arcade Service layout — a coin-op cabinet's hidden test-mode screen.
//   • Black screen over the monitor-convergence crosshatch, with colour-bar
//     strips top and bottom, in a chunky pixel font.
//   • One numbered list, like an operator menu; the hovered item turns
//     yellow with a blinking arrow, the active choice is marked ON.
//   • Pages print in line by line; the footer blinks INSERT COIN.
// ---------------------------------------------------------------------------
namespace arcade {

constexpr ImU32 kWhite   = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kYellow  = IM_COL32(255, 255,   0, 255);
constexpr ImU32 kRed     = IM_COL32(255,  48,  48, 255);
constexpr ImU32 kCyan    = IM_COL32(  0, 220, 255, 255);
constexpr ImU32 kGreen   = IM_COL32( 48, 255,  72, 255);
constexpr ImU32 kBlue    = IM_COL32( 40,  40, 255, 255);
constexpr ImU32 kMagenta = IM_COL32(255,   0, 255, 255);
constexpr ImU32 kBlack   = IM_COL32(  0,   0,   0, 255);

ImFont* Font() { return StyleFonts::pressStart ? StyleFonts::pressStart : ImGui::GetFont(); }

float TextW(float size, const char* text) { return Font()->CalcTextSizeA(size, FLT_MAX, 0.f, text).x; }

// Pixel text with a one-pixel darker shadow, the classic arcade drop.
void PixelText(ImDrawList* dl, float size, ImVec2 pos, ImU32 col, const char* text) {
    const float o = std::max(1.f, size / 8.f);
    dl->AddText(Font(), size, ImVec2(pos.x + o, pos.y + o), Shade(col, 0.35f), text);
    dl->AddText(Font(), size, pos, col, text);
}

// True for the "on" part of each blink period.
bool Blink(float t, float period, float duty) { return fmodf(t, period) < period * duty; }

// A pixel arrow pointing right with its tip at `tip`; px is one pixel.
void Arrow(ImDrawList* dl, ImVec2 tip, float px, ImU32 col) {
    for (int c = 0; c < 4; ++c) {
        const float h = (7 - 2 * c) * px * 0.5f;
        dl->AddRectFilled(ImVec2(tip.x - (4 - c) * px, tip.y - h), ImVec2(tip.x - (3 - c) * px, tip.y + h), col);
    }
}

// A strip of the seven test colour bars across a..b.
void ColourBars(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    static constexpr ImU32 kBars[7] = { kWhite, kYellow, kCyan, kGreen, kMagenta, kRed, kBlue };
    const float w = (b.x - a.x) / 7.f;
    for (int i = 0; i < 7; ++i)
        dl->AddRectFilled(ImVec2(a.x + i * w, a.y), ImVec2(a.x + (i + 1) * w, b.y), kBars[i]);
}

std::string Upper(const char* s) {
    std::string out(s);
    for (char& ch : out) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
    return out;
}

// Text-only button: turns yellow and underlines when hovered.  True when clicked.
bool TextButton(const char* id, const char* label, ImU32 col) {
    const float  fs      = Sc(16.f);
    const ImVec2 pos     = ImGui::GetCursorScreenPos();
    const ImVec2 size(TextW(fs, label) + Sc(8.f), Sc(22.f));
    const bool   clicked = ImGui::InvisibleButton(id, size);
    const bool   hovered = ImGui::IsItemHovered();
    ImDrawList*  dl      = ImGui::GetWindowDrawList();
    const ImU32  c       = hovered ? kYellow : col;
    PixelText(dl, fs, ImVec2(pos.x + Sc(4.f), pos.y + (size.y - fs) * 0.5f), c, label);
    if (hovered)
        dl->AddRectFilled(ImVec2(pos.x + Sc(4.f), pos.y + size.y), ImVec2(pos.x + size.x - Sc(4.f), pos.y + size.y + Sc(2.f)), c);
    return clicked;
}

// The arcade test-mode look for the shared paged menu.
class Style : public PagedMenuStyle {
public:
    ImVec2  PanelSize()   const override { return ImVec2(680.f, 720.f); }
    ImVec2  Padding()     const override { return ImVec2(36.f, 24.f); }
    float   TitleSpace()  const override { return 88.f; }
    float   FooterSpace() const override { return 84.f; }
    float   FooterRow()   const override { return 72.f; }
    int     Columns()     const override { return 1; }
    float   Gutter()      const override { return 40.f; }   // room for the arrow
    float   GroupButtonHeight() const override { return 22.f; }
    float   ItemButtonHeight()  const override { return 22.f; }
    float   SliderWidth() const override { return 0.5f; }
    ImFont* Font()        const override { return arcade::Font(); }
    float   FontSize()    const override { return 16.f; }

    std::string PageTitle(int pageIndex) const override {
        return pageIndex < 0 ? "SERVICE MODE" : kSectionNames[pageIndex];
    }
    std::string GroupLabel(int section) const override {
        char buf[64];
        snprintf(buf, sizeof(buf), "%02d %s", section + 1, kSectionNames[section]);
        return buf;
    }

    // Each page prints in a line at a time.
    float PageReveal(float t) const override { return std::min(1.f, floorf(t / 0.025f) / 12.f); }

    // Black screen over the convergence crosshatch, colour bars top and bottom.
    void DrawPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, int, float) override {
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 248));
        const float step = Sc(40.f);
        const float x0 = a.x + fmodf(b.x - a.x, step) * 0.5f, y0 = a.y + fmodf(b.y - a.y, step) * 0.5f;
        for (float x = x0; x <= b.x; x += step) dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), IM_COL32(255, 255, 255, 22));
        for (float y = y0; y <= b.y; y += step) dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(255, 255, 255, 22));
        for (float x = x0; x <= b.x; x += step)
            for (float y = y0; y <= b.y; y += step)
                dl->AddRectFilled(ImVec2(x - Sc(1.f), y - Sc(1.f)), ImVec2(x + Sc(1.f), y + Sc(1.f)), IM_COL32(255, 255, 255, 90));
        const float in = Sc(4.f);
        dl->AddRect(ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, b.y - in), kWhite, 0.f, 0, Sc(2.f));
        ColourBars(dl, ImVec2(a.x + Sc(10.f), a.y + Sc(10.f)), ImVec2(b.x - Sc(10.f), a.y + Sc(16.f)));
        ColourBars(dl, ImVec2(a.x + Sc(10.f), b.y - Sc(16.f)), ImVec2(b.x - Sc(10.f), b.y - Sc(10.f)));
    }

    void DrawTitle(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* title, int, float) override {
        float       fs   = Sc(24.f);
        const float maxW = (b.x - a.x) - Sc(140.f);
        float       w    = TextW(fs, title);
        if (w > maxW) { fs *= maxW / w; w = maxW; }
        PixelText(dl, fs, ImVec2(a.x + (b.x - a.x - w) * 0.5f, a.y + Sc(34.f)), kYellow, title);
        const char* sub = "SEERT CRT OVERLAY";
        const float ss  = Sc(8.f);
        PixelText(dl, ss, ImVec2(a.x + (b.x - a.x - TextW(ss, sub)) * 0.5f, a.y + Sc(70.f)), kCyan, sub);
    }

    bool GroupButton(const char* id, const char* label, ImVec2 size, int index, bool selected, Kind kind) override {
        const ImVec2 pos     = ImGui::GetCursorScreenPos();
        const bool   clicked = ImGui::InvisibleButton(id, size);
        const bool   hovered = ImGui::IsItemHovered();
        ImDrawList*  dl      = ImGui::GetWindowDrawList();
        const float  t       = static_cast<float>(ImGui::GetTime());

        std::string text = label;
        if (kind != Kind::Group) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%02d ", index + 1);
            text = buf + Upper(label);
        }
        const float fs   = Sc(16.f);
        const float room = size.x - (selected ? TextW(fs, " ON") : 0.f);
        while (text.size() > 4 && TextW(fs, text.c_str()) > room) text.pop_back();

        const ImU32 col = selected ? kGreen : hovered ? kYellow : kWhite;
        const float ty  = pos.y + (size.y - fs) * 0.5f;
        PixelText(dl, fs, ImVec2(pos.x, ty), col, text.c_str());
        if (selected) PixelText(dl, fs, ImVec2(pos.x + size.x - TextW(fs, "ON"), ty), kGreen, "ON");
        if (hovered && Blink(t, 0.5f, 0.65f))
            Arrow(dl, ImVec2(pos.x - Sc(10.f), pos.y + size.y * 0.5f), Sc(2.f), kYellow);
        return clicked;
    }

    bool  BackButton(const char* id) override { return TextButton(id, "< BACK", kWhite); }
    float QuitWidth() const override { return TextW(Sc(16.f), "QUIT") + Sc(8.f); }
    bool  QuitButton(const char* id) override { return TextButton(id, "QUIT", kRed); }

    void DrawFooter(ImDrawList* dl, ImVec2 a, ImVec2 b, int, float t) override {
        const float cx = (a.x + b.x) * 0.5f;
        if (Blink(t, 1.f, 0.6f)) {
            const float fs = Sc(16.f);
            PixelText(dl, fs, ImVec2(cx - TextW(fs, "INSERT COIN") * 0.5f, a.y + Sc(3.f)), kYellow, "INSERT COIN");
        }
        const float ss = Sc(8.f);
        PixelText(dl, ss, ImVec2(cx - TextW(ss, "CREDIT 00") * 0.5f, a.y + Sc(32.f)), kWhite, "CREDIT 00");
    }

    ImVec2 CloseCenter(ImVec2 a, ImVec2 b) const override { return ImVec2(b.x - Sc(34.f), a.y + Sc(44.f)); }
    bool   CloseButton(const char* id, ImVec2 c) override {
        const float r = Sc(13.f);
        ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(r * 2.f, r * 2.f));
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), hovered ? kYellow : kRed);
        const float fs = Sc(16.f);
        dl->AddText(arcade::Font(), fs, ImVec2(c.x - TextW(fs, "X") * 0.5f + Sc(1.f), c.y - fs * 0.5f + Sc(1.f)),
                    hovered ? kBlack : kWhite, "X");
        return clicked;
    }
};

} // namespace arcade

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
SettingsPanel::DrawResult SettingsPanel::Draw(CRTSettings& s,
                                               const Monitors& monitors,
                                               HMONITOR selectedMon,
                                               const std::vector<HMONITOR>& secondaryMons,
                                               bool streamOpen,
                                               const std::vector<PresetItem>& presets,
                                               const std::vector<AudioDevEntry>& audioDevices,
                                               const CursorInfo& cursor) {
    if (!s.menuVisible) return {};
    if (!StyleName(s.menuStyle)) s.menuStyle = 0;   // unknown or retired id
    s_cursorInfo = &cursor;

    // Menu size.  While a size slider is being dragged keep last frame's
    // size, so the slider doesn't move out from under the mouse.
    s.menuScale  = std::clamp(s.menuScale,  0.5f, 2.f);
    s.menuWidth  = std::clamp(s.menuWidth,  0.5f, 2.f);
    s.menuHeight = std::clamp(s.menuHeight, 0.5f, 2.f);
    if (!s_sizeSliderHeld) {
        s_menuScale  = s.menuScale;
        s_menuWidth  = s.menuWidth;
        s_menuHeight = s.menuHeight;
    }
    s_sizeSliderHeldNext = false;

    // Scale the style (padding, rounding, fonts) for the panel only; the
    // toggle button and other overlay windows keep their normal size.
    ImGuiStyle&      style = ImGui::GetStyle();
    const ImGuiStyle saved = style;
    style.ScaleAllSizes(s_menuScale);
    style.FontScaleMain *= s_menuScale;

    DrawResult result;
    switch (s.menuStyle) {
        case 1:  result = DrawDefault   (s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices); break;
        case 3:  result = DrawCamcorder (s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices); break;
        case 4:  result = DrawDOS       (s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices); break;
        case 5:  result = DrawVCR       (s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices); break;
        case 6: {
            static adv64::Style adv64Style;
            result = DrawPagedMenu(adv64Style, "##adv64", s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices);
            break;
        }
        case 7: {
            static sorcery::Style sorceryStyle;
            result = DrawPagedMenu(sorceryStyle, "##sorcery", s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices);
            break;
        }
        case 8: {
            static teletext::Style teletextStyle;
            result = DrawPagedMenu(teletextStyle, "##teletext", s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices);
            break;
        }
        case 9: {
            static vecmon::Style vectorStyle;
            result = DrawPagedMenu(vectorStyle, "##vector", s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices);
            break;
        }
        case 10: {
            static arcade::Style arcadeStyle;
            result = DrawPagedMenu(arcadeStyle, "##arcade", s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices);
            break;
        }
        default: result = DrawOSD       (s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices); break;
    }
    style = saved;
    s_sizeSliderHeld = s_sizeSliderHeldNext;
    s_cursorInfo = nullptr;
    return result;
}
