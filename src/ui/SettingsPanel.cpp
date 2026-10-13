#include "SettingsPanel.h"
#include "../settings/Presets.h"
#include "StyleFonts.h"
#include <imgui.h>
#include <algorithm> // std::clamp
#include <cfloat>    // FLT_MAX
#include <cctype>    // toupper / tolower
#include <cmath>
#include <cstring>   // strcmp
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

static const char* kStyleNames[] = {
    "CRT OSD", "Simple", "N64", "Camcorder", "DOS Terminal", "VCR", "64 Adventure"
};
static constexpr int kStyleCount = 7;

static const char* kSectionNames[] = {
    "CAPTURE SOURCE", "PRESETS", "GEOMETRY", "SCANLINES",
    "GLOW", "HALATION", "VIGNETTE", "FLICKER", "STATIC",
    "SHADOW MASK", "COLOUR", "CURSOR", "STREAMING", "MENU STYLE"
};
static constexpr int kSectionCount = 14;
static constexpr int kMenuSection  = 13;   // style switcher + panel size

// Show a tooltip for the most recently drawn item.
static void Tip(const char* text) {
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", text);
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
    if (ImGui::BeginCombo("Style##menu", kStyleNames[s.menuStyle])) {
        for (int i = 0; i < kStyleCount; ++i)
            if (ImGui::Selectable(kStyleNames[i], s.menuStyle == i))
                s.menuStyle = i;
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
// N64 layout — Cherry Bomb font, accordion, centered menu items
// ---------------------------------------------------------------------------
static SettingsPanel::DrawResult DrawN64(CRTSettings& s,
                                          const Monitors& monitors,
                                          HMONITOR selectedMon,
                                          const std::vector<HMONITOR>& secondaryMons,
                                          bool streamOpen,
                                          const std::vector<PresetItem>& presets,
                                          const AudioDevs& audioDevices) {
    static int s_open = -1;
    SettingsPanel::DrawResult result;

    if (StyleFonts::cherryBomb) ImGui::PushFont(StyleFonts::cherryBomb);

    ImVec2 display = ImGui::GetIO().DisplaySize;
    const float panelW = PanelWidth(560.f);
    const float maxH   = PanelHeight(display.y * 0.78f);

    ImGui::SetNextWindowPos(
        ImVec2((display.x - panelW) * 0.5f, PanelTop(display.y * 0.11f, maxH)), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(panelW, 60.f), ImVec2(panelW, maxH));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_AlwaysAutoResize;

    if (!ImGui::Begin("##N64", nullptr, flags)) {
        ImGui::End();
        if (StyleFonts::cherryBomb) ImGui::PopFont();
        return {};
    }

    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec4 kGold = { 1.00f, 0.85f, 0.10f, 1.00f };

    // Helper: draw a centered button, return true if clicked
    auto CenteredButton = [&](const char* label) -> bool {
        float bw = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.f;
        float off = (avail - bw) * 0.5f;
        if (off > 0.f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + off);
        return ImGui::Button(label);
    };

    // Big centered title
    ImGui::SetWindowFontScale(1.55f);
    {
        const char* title = "SETTINGS";
        float tw = ImGui::CalcTextSize(title).x;
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + (avail - tw) * 0.5f);
        ImGui::TextColored(kGold, "%s", title);
    }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    DrawTopPresets(presets, result);
    ImGui::Spacing();

    // Single-open accordion section list
    for (int i = 0; i < kSectionCount; ++i) {
        ImGui::PushID(i);

        char hdr[80];
        bool isOpen = (s_open == i);
        snprintf(hdr, sizeof(hdr), isOpen ? "  \u25bc  %s  " : "  \u25b6  %s  ", kSectionNames[i]);
        if (CenteredButton(hdr)) s_open = isOpen ? -1 : i;

        if (isOpen) {
            ImGui::Indent(Sc(20.f));
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.58f);
            DrawSectionContent(i, s, monitors, selectedMon, secondaryMons,
                               streamOpen, presets, audioDevices, result);
            ImGui::PopItemWidth();
            ImGui::Unindent(Sc(20.f));
            ImGui::Spacing();
        }

        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Close and Quit as centered menu items
    if (CenteredButton("  CLOSE  ")) s.menuVisible = false;
    Tip("Hide the settings panel.");
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.05f, 0.04f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.08f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.00f, 0.12f, 0.08f, 1.f));
    if (CenteredButton("  QUIT  ")) PostQuitMessage(0);
    Tip("Quit the overlay.");
    ImGui::PopStyleColor(3);
    ImGui::Spacing();

    ImGui::End();
    if (StyleFonts::cherryBomb) ImGui::PopFont();
    return result;
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

constexpr int kMainPage = -1;

// Scales a colour's RGB by k (clamped), keeping alpha.
ImU32 Shade(ImU32 c, float k) {
    auto ch = [&](int shift) {
        const int v = static_cast<int>(((c >> shift) & 0xFF) * k);
        return static_cast<ImU32>(std::clamp(v, 0, 255)) << shift;
    };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | (c & IM_COL32_A_MASK);
}

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

// "CAPTURE SOURCE" → "Capture Source".  Cherry Bomb's capital R closes up at
// button sizes and reads as an A ("STAEAMING"), so button labels use title
// case; the big page titles stay in capitals.
std::string TitleCase(const char* s) {
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
float EaseOutBack(float t) {
    t = std::clamp(t, 0.f, 1.f);
    constexpr float c1 = 1.70158f, c3 = c1 + 1.f;
    return 1.f + c3 * powf(t - 1.f, 3.f) + c1 * powf(t - 1.f, 2.f);
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

} // namespace adv64

static SettingsPanel::DrawResult DrawAdventure64(CRTSettings& s,
                                                 const Monitors& monitors,
                                                 HMONITOR selectedMon,
                                                 const std::vector<HMONITOR>& secondaryMons,
                                                 bool streamOpen,
                                                 const std::vector<PresetItem>& presets,
                                                 const AudioDevs& audioDevices) {
    using namespace adv64;
    static int   s_page      = kMainPage;
    static float s_pageTime  = 0.f;
    static int   s_lastFrame = -10;
    static float s_openTime  = 0.f;

    SettingsPanel::DrawResult result;
    const float t = static_cast<float>(ImGui::GetTime());

    // Freshly opened (not drawn last frame): pop in on the main page.
    if (ImGui::GetFrameCount() - s_lastFrame > 1) {
        s_openTime = t;
        s_page     = kMainPage;
        s_pageTime = t;
    }
    s_lastFrame = ImGui::GetFrameCount();
    auto goTo = [&](int page) { s_page = page; s_pageTime = t; };

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float  panelW  = PanelWidth(680.f);
    const float  panelH  = PanelHeight(690.f);
    const float  drop    = (1.f - EaseOutBack((t - s_openTime) / 0.45f)) * Sc(-80.f);
    ImGui::SetNextWindowPos(ImVec2((display.x - panelW) * 0.5f, (display.y - panelH) * 0.5f + drop),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Sc(36.f), Sc(26.f)));
    const bool visible = ImGui::Begin("##adv64", nullptr, kFlags);
    ImGui::PopStyleVar();
    if (!visible) { ImGui::End(); return result; }

    ImGui::PushFont(Font(), 20.f);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    Panel(dl, wp, ImVec2(wp.x + panelW, wp.y + panelH), t);

    // Title — every letter its own colour, bobbing on a wave.
    {
        const char* title = (s_page == kMainPage) ? "SETTINGS" : kSectionNames[s_page];
        const float size = Sc(50.f), gap = Sc(2.f);
        float width = 0.f;
        for (int i = 0; title[i]; ++i) {
            const char ch[2] = { title[i], 0 };
            width += TextSize(size, ch).x + gap;
        }
        float x = wp.x + (panelW - width) * 0.5f;
        for (int i = 0; title[i]; ++i) {
            const char ch[2] = { title[i], 0 };
            if (ch[0] != ' ')
                CartoonText(dl, size, ImVec2(x, wp.y + Sc(24.f + sinf(t * 3.2f + i * 0.55f) * 4.f)),
                            kPalette[i % 4], ch);
            x += TextSize(size, ch).x + gap;
        }
        ImGui::Dummy(ImVec2(0.f, Sc(70.f)));
    }

    // Page content, sliding up with a bounce whenever the page changes.
    const float kFooterH = Sc(60.f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1.f - EaseOutBack((t - s_pageTime) / 0.35f)) * Sc(40.f));
    ImGui::BeginChild("##adv64page", ImVec2(0.f, -kFooterH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    {
        const float avail = ImGui::GetContentRegionAvail().x;
        if (s_page == kMainPage) {
            // Two columns of group buttons, with room on the left of each for the hover star.
            const float kStarRoom = Sc(48.f), kGap = Sc(50.f);   // room for the hover star
            const float bw = (avail - kStarRoom - kGap - Sc(12.f)) * 0.5f;
            for (int i = 0; i < kSectionCount; ++i) {
                const std::string label = TitleCase(kSectionNames[i]);
                if (i % 2 == 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kStarRoom);
                else            ImGui::SameLine(0.f, kGap);
                ImGui::PushID(i);
                if (BubbleButton("##group", label.c_str(), ImVec2(bw, Sc(54.f)), kPalette[(i / 2 + i) % 4], Sc(22.f)))
                    goTo(i);
                ImGui::PopID();
            }
        } else if (strcmp(kSectionNames[s_page], "PRESETS") == 0) {
            // Presets as a grid of bubble buttons — like picking a course —
            // with the usual dropdown / save / manage row underneath.
            const float kStarRoom = Sc(48.f), kGap = Sc(50.f);   // room for the hover star
            const float bw = (avail - kStarRoom - kGap - Sc(12.f)) * 0.5f;
            for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
                if (i % 2 == 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kStarRoom);
                else            ImGui::SameLine(0.f, kGap);
                ImGui::PushID(i);
                if (BubbleButton("##preset", presets[i].name.c_str(), ImVec2(bw, Sc(44.f)),
                                 kPalette[(i / 2 + i) % 4], Sc(19.f))) {
                    result.applyPreset    = true;
                    result.presetSettings = presets[i].settings;
                }
                ImGui::PopID();
            }
            ImGui::Dummy(ImVec2(0.f, Sc(6.f)));
            DrawSectionContent(s_page, s, monitors, selectedMon, secondaryMons,
                               streamOpen, presets, audioDevices, result);
        } else if (s_page == kMenuSection) {
            // Styles as a grid of bubble buttons, then the panel-size sliders.
            const float kStarRoom = Sc(48.f), kGap = Sc(50.f);   // room for the hover star
            const float bw = (avail - kStarRoom - kGap - Sc(12.f)) * 0.5f;
            for (int i = 0; i < kStyleCount; ++i) {
                if (i % 2 == 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kStarRoom);
                else            ImGui::SameLine(0.f, kGap);
                ImGui::PushID(i);
                if (BubbleButton("##style", kStyleNames[i], ImVec2(bw, Sc(44.f)),
                                 kPalette[(i / 2 + i) % 4], Sc(19.f), s.menuStyle == i))
                    s.menuStyle = i;
                ImGui::PopID();
            }
            ImGui::Dummy(ImVec2(0.f, Sc(14.f)));
            ImGui::PushItemWidth(avail * 0.55f);
            DrawMenuSizeControls(s);
            ImGui::PopItemWidth();
        } else {
            ImGui::PushItemWidth(avail * 0.55f);
            DrawSectionContent(s_page, s, monitors, selectedMon, secondaryMons,
                               streamOpen, presets, audioDevices, result);
            ImGui::PopItemWidth();
        }
        ImGui::Dummy(ImVec2(0.f, Sc(8.f)));   // room for the last button's shadow
    }
    ImGui::EndChild();

    // Esc steps back out of a page, like a B press.
    if (s_page != kMainPage && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive())
        goTo(kMainPage);

    // Footer — controller-style prompts: (B) BACK on the left, (C) QUIT on the right.
    const float footerY = panelH - kFooterH - Sc(4.f);
    const float padX    = ImGui::GetStyle().WindowPadding.x;
    if (s_page != kMainPage) {
        ImGui::SetCursorPos(ImVec2(padX + Sc(8.f), footerY));
        if (Prompt("##back", "B", kGreen, "BACK", Sc(20.f))) goTo(kMainPage);
        Tip("Back to the main menu (Esc).");
    }
    ImGui::SetCursorPos(ImVec2(panelW - padX - Sc(8.f) - PromptWidth("C", "QUIT", Sc(20.f)), footerY));
    if (Prompt("##quit", "C", kYellow, "QUIT", Sc(20.f))) PostQuitMessage(0);
    Tip("Quit the overlay application entirely.");

    // Close — a round X button in the top-right corner, inside the gold rim.
    // It sits in the window padding, so widen the clip rect to reach it.
    ImGui::PushClipRect(wp, ImVec2(wp.x + panelW, wp.y + panelH), false);
    if (CloseButton("##close", ImVec2(wp.x + panelW - Sc(48.f), wp.y + Sc(48.f)), Sc(22.f)))
        s.menuVisible = false;
    Tip("Close the settings panel (Ctrl+Shift+C to re-open).");
    ImGui::PopClipRect();

    ImGui::PopFont();
    ImGui::End();
    return result;
}

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
    if (s.menuStyle < 0 || s.menuStyle >= kStyleCount) s.menuStyle = 0;
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
        case 2:  result = DrawN64       (s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices); break;
        case 3:  result = DrawCamcorder (s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices); break;
        case 4:  result = DrawDOS       (s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices); break;
        case 5:  result = DrawVCR       (s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices); break;
        case 6:  result = DrawAdventure64(s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices); break;
        default: result = DrawOSD       (s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices); break;
    }
    style = saved;
    s_sizeSliderHeld = s_sizeSliderHeldNext;
    s_cursorInfo = nullptr;
    return result;
}
