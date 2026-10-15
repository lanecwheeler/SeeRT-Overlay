#include "SettingsPanel.h"
#include "../settings/Presets.h"
#include "StyleFonts.h"
#include "Degauss.h"
#include "../update/UpdateCheck.h"
#include "../update/Updater.h"
#include "patchnotes.h"
#include <sstream>
#include <imgui.h>
#include <imgui_internal.h>   // BringWindowToDisplayFront (the fridge magnet)
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
// as menuStyle in settings.json, so it never changes when the list is
// reordered — and a retired style just leaves a gap (2 was the old N64 style,
// replaced by 64 Adventure).
struct MenuStyleInfo { int id; const char* name; };
static constexpr MenuStyleInfo kStyles[] = {
    {  8, "Teletext" },       {  6, "64 Adventure" },    {  7, "Sword & Sorcery" },
    {  9, "Vector Monitor" }, { 10, "Arcade Service" },  { 11, "Computer Lab" },
    { 12, "Web 1.0" },        {  0, "CRT OSD" },         {  3, "Camcorder" },
    {  5, "VCR" },            {  4, "DOS Terminal" },    {  1, "Simple" },
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
    "SHADOW MASK", "COLOUR", "CURSOR", "STREAMING", "MENU & APP"
};
static constexpr int kSectionCount = 14;
static constexpr int kMenuSection  = 13;   // menu style, panel size, version and updates
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
// Room kept right of a slider for its label: the longest slider label's
// width.  With the item width set to minus this, every slider on a page lines
// up and slider plus label fill the width.
static float SliderLabelColumn() {
    static constexpr const char* kLongest[] = {
        "Display Gamma", "Beam Dynamics", "Hum Intensity", "Compensation", "Raster Bloom", "Persistence",
    };
    float w = 0.f;
    for (const char* label : kLongest) w = std::max(w, ImGui::CalcTextSize(label).x);
    return w + ImGui::GetStyle().ItemInnerSpacing.x;
}

static float s_menuScale  = 1.f;
static float s_menuWidth  = 1.f;
static float s_menuHeight = 1.f;
static bool  s_sizeSliderHeld     = false;  // a size slider was being dragged last frame
static bool  s_sizeSliderHeldNext = false;

static constexpr float kScreenMargin = 20.f;

// Quit confirmation — every style's quit button calls RequestQuit(); the
// dialog itself is drawn once a frame by SettingsPanel::Draw (DrawQuitConfirm).
static bool s_quitRequested = false;
static bool s_saveDrawingRequested = false;   // Computer Lab's Save button
static bool s_patchNotesRequested  = false;   // open the patch notes window
static bool s_installPromptRequested = false; // ask whether to install the downloaded update
static void RequestQuit() { s_quitRequested = true; }

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

// Menu & App — the Degauss button (CRT OSD has its own in the footer).
static void DrawDegaussControls(CRTSettings& s) {
    if (ImGui::Button("Degauss##degauss")) Degauss::Start(s.degaussSound);
    Tip("Demagnetises the tube, clearing any colour blotches.\n"
        "Give it a few seconds between goes - the coil needs to cool down.");
    ImGui::SameLine();
    ImGui::Checkbox("Sound##degauss", &s.degaussSound);
    Tip("Play the degaussing coil's hum.");
}

static constexpr const char* kGitHubTip =
    "Check it out on GitHub! Feel free to report bugs and leave feedback/feature requests as well!";

// Menu & App — this build's version, the last update check's outcome, the
// update controls and, for menus without one in the footer, the GitHub link.
static void DrawAppInfo(CRTSettings& s, SettingsPanel::DrawResult& result, bool githubLink) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const UpdateCheck::Result latest   = UpdateCheck::Latest();
    const Updater::Progress   update   = Updater::Current();
    const bool                checking = UpdateCheck::Running();
    const bool                newer    = !checking && latest.status == UpdateCheck::Status::Available;
    const bool                ready    = update.status == Updater::Status::Ready && !Updater::Installed();
    const bool                fetching = update.status == Updater::Status::Downloading;

    std::string line = std::string("SeeRT Overlay v") + UpdateCheck::CurrentVersion();
    if (ready)          line += " - v" + update.version + " is ready to install";
    else if (fetching)  line += " - downloading v" + update.version + "... " +
                                std::to_string(static_cast<int>(update.fraction * 100.f)) + "%";
    else if (checking)  line += " - checking for updates...";
    else if (newer)     line += " - v" + latest.latestVersion + " is available!";
    else if (latest.status == UpdateCheck::Status::UpToDate) line += " - up to date";
    else if (latest.status == UpdateCheck::Status::Failed)   line += " - couldn't reach GitHub";
    ImGui::TextWrapped("%s", line.c_str());

    if (ready) {
        if (ImGui::Button("Restart to update##update")) result.restartToUpdate = true;
        Tip("Installs the new version and starts it again.  Your settings and presets are kept.");
    } else if (newer && !fetching) {
        if (ImGui::Button("Update now##update")) result.updateNow = true;
        Tip("Downloads the new version, installs it and restarts.");
    }
    if ((ready || newer) && !fetching) {
        ImGui::SameLine();
        if (ImGui::Button("What's new##update")) result.openUpdatePage = true;
        Tip("Opens the release page on GitHub.");
    }

    // The check button and the launch checkbox side by side.
    if (ImGui::BeginTable("##updates", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        if (checking) ImGui::BeginDisabled();
        if (ImGui::Button("Check for updates##update")) result.checkForUpdates = true;
        if (checking) ImGui::EndDisabled();
        Tip("Asks GitHub for the latest release now.");
        ImGui::TableNextColumn();
        ImGui::Checkbox("Check on launch##update", &s.checkForUpdates);
        Tip("Asks GitHub whether a newer release is out each time the app starts.");
        ImGui::EndTable();
    }
    ImGui::Checkbox("Download updates automatically##update", &s.autoUpdate);
    Tip("Downloads new versions in the background, then asks before installing them.");
    ImGui::Checkbox("Install updates without asking##update", &s.installWithoutAsking);
    Tip("Installs a downloaded update when you quit the app, without asking first.\n"
        "Your settings and presets are kept.");
    if (ImGui::Button("Patch notes##update")) s_patchNotesRequested = true;
    Tip("What's changed in each version.");

    if (githubLink) {
        // The address if it fits on one line (wide pixel fonts may not), else a short name.
        const char* link = "github.com/lanecwheeler/SeeRT-Overlay";
        if (ImGui::CalcTextSize(link).x > ImGui::GetContentRegionAvail().x) link = "SeeRT Overlay on GitHub";
        if (ImGui::TextLink(link)) result.openProjectPage = true;
        Tip(kGitHubTip);
    }
    ImGui::Spacing();
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

    Sec("MENU & APP");
    DrawStyleCombo(s);
    DrawMenuSizeControls(s);
    DrawAppInfo(s, result, true);
}

// ---------------------------------------------------------------------------
// The fridge magnet — in every style, just peeking in from the right edge of
// the screen.  Hover it and it slides out a little; drag it over the picture
// and it stains the tube with colour, the way a magnet does a real one.  The
// Degauss button (Menu & App, or CRT OSD's footer) clears it.
// ---------------------------------------------------------------------------
static constexpr float kParkAngle = 1.0472f;   // 60 degrees from poles-down: 30 degrees off level

static void DrawFridgeMagnet(const CRTSettings& s) {
    static bool   s_held  = false;
    static ImVec2 s_grab;              // the mouse's offset from the magnet's centre
    static float  s_peek  = 0.f;       // 0 tucked away .. 1 slid out
    static float  s_angle = kParkAngle;   // tilted, poles toward the screen, while parked; down while held

    ImGuiIO&     io      = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    if (display.x <= 0.f || display.y <= 0.f) return;
    const float  w   = Sc(42.f);
    const float  h   = w * 1.1f;
    const float  box = h * 1.5f;       // square hit box around the magnet at any angle
    const float  dt  = std::min(io.DeltaTime, 0.1f);

    // Parked against the right edge of the visible picture, a little below the
    // middle.  Curvature and zoom move that edge, so find the menu-space x
    // that the screen's right edge shows at this height.
    const float parkY = display.y * 0.62f;
    float edgeX = display.x;
    {
        const float cy = parkY / display.y * 2.f - 1.f;
        const float r2 = 1.f + cy * cy;
        const float sx = (1.f + s.curvatureX * r2) / std::max(s.zoom, 0.01f);
        edgeX = std::min(display.x, (sx * 0.5f + 0.5f) * display.x);
    }
    // How far the parked magnet reaches left of its centre, at its tilt.
    const float pa = std::cos(kParkAngle), pb = std::sin(kParkAngle);
    float reach = 0.f;
    for (float x : { -w * 0.5f, w * 0.5f })
        for (float y : { -h * 0.5f, h * 0.5f }) reach = std::max(reach, -(x * pa - y * pb));
    const float  visible = Sc(27.f) + s_peek * Sc(14.f);   // how much shows past the edge
    const ImVec2 park(edgeX - visible + reach, parkY);

    ImVec2 c = s_held ? ImVec2(io.MousePos.x - s_grab.x, io.MousePos.y - s_grab.y) : park;

    // An invisible window with one button over the magnet: the same input
    // path as every other control, kept in front of the menus.
    const bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    ImGui::SetNextWindowPos(ImVec2(c.x - box * 0.5f, c.y - box * 0.5f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(box, box), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    bool hovered = false;
    ImDrawList* dl = nullptr;
    if (ImGui::Begin("##fridgemagnet", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove)) {
        if (!popupOpen) ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImGui::InvisibleButton("##magnet", ImVec2(box, box));
        hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemActivated()) {
            s_held = true;
            s_grab = ImVec2(io.MousePos.x - c.x, io.MousePos.y - c.y);
        }
        if (s_held && !ImGui::IsItemActive()) s_held = false;
        if (hovered && !s_held) ImGui::SetTooltip("Keep magnets away from the TV!");
        dl = ImGui::GetWindowDrawList();
    }

    c = s_held ? ImVec2(io.MousePos.x - s_grab.x, io.MousePos.y - s_grab.y) : park;
    s_peek  += ((hovered || s_held ? 1.f : 0.f) - s_peek) * std::min(1.f, dt * 12.f);
    s_angle += ((s_held ? 0.f : kParkAngle) - s_angle) * std::min(1.f, dt * 14.f);

    // Local coordinates: centred on the magnet, poles down (+y), then turned.
    const float ca = std::cos(s_angle), sa = std::sin(s_angle);
    auto at = [&](float x, float y, ImVec2 off = ImVec2(0.f, 0.f)) {
        return ImVec2(c.x + x * ca - y * sa + off.x, c.y + x * sa + y * ca + off.y);
    };

    // The poles, between the legs' tips, magnetise the tube where they are.
    if (s_held) {
        const ImVec2 pole = at(0.f, h * 0.5f);
        Degauss::Magnetise(pole.x / display.x, pole.y / display.y, display.x / display.y, io.DeltaTime);
    }

    // A red horseshoe with steel tips — in the magnet's window, so the cursor
    // and tooltips go over it.  Unclipped: the window lags the drag by a frame.
    if (!dl) { ImGui::End(); ImGui::PopStyleVar(2); return; }
    dl->PushClipRectFullScreen();
    const float t      = w * 0.3f;               // leg thickness
    const float r      = (w - t) * 0.5f;         // arch radius, to the leg centres
    const float tipH   = t * 0.9f;
    const float archY  = -h * 0.5f + w * 0.5f;   // arch centre
    const float legEnd = h * 0.5f - tipH;
    auto horseshoe = [&](ImVec2 off, float radius, ImU32 col, float thick) {
        dl->PathLineTo(at(-radius, legEnd, off));
        for (int i = 0; i <= 20; i++) {
            const float a = 3.14159265f * (1.f + i / 20.f);
            dl->PathLineTo(at(std::cos(a) * radius, archY + std::sin(a) * radius, off));
        }
        dl->PathLineTo(at(radius, legEnd, off));
        dl->PathStroke(col, ImDrawFlags_None, thick);
    };
    const float lift = s_held ? Sc(6.f) : Sc(2.f);   // picked up: the shadow drops away
    horseshoe(ImVec2(lift, lift * 1.4f), r, IM_COL32(0, 0, 0, 90), t);
    horseshoe(ImVec2(0.f, 0.f), r, IM_COL32(200, 28, 34, 255), t);
    horseshoe(ImVec2(0.f, 0.f), r + t * 0.22f, IM_COL32(255, 125, 115, 150), t * 0.18f);
    for (float side : { -1.f, 1.f }) {
        const float x0 = side * r - t * 0.5f, x1 = side * r + t * 0.5f, y1 = h * 0.5f;
        dl->AddQuadFilled(at(x0, legEnd), at(x1, legEnd), at(x1, y1), at(x0, y1), IM_COL32(196, 202, 212, 255));
        dl->AddQuadFilled(at(x0, legEnd), at(x1, legEnd), at(x1, legEnd + tipH * 0.3f), at(x0, legEnd + tipH * 0.3f),
                          IM_COL32(240, 243, 248, 255));
        dl->AddQuad(at(x0, legEnd), at(x1, legEnd), at(x1, y1), at(x0, y1), IM_COL32(110, 115, 125, 255));
    }
    dl->PopClipRect();
    ImGui::End();
    ImGui::PopStyleVar(2);
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
    ImGui::PushItemWidth(-SliderLabelColumn());

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
        if (ImGui::Button("[ DEGAUSS ]", ImVec2(bw, 0.f))) Degauss::Start(s.degaussSound);
        Tip("Demagnetises the tube, clearing any colour blotches.\n"
            "Give it a few seconds between goes - the coil needs to cool down.");
        ImGui::SameLine();
        ImGui::Checkbox("Sound##degauss", &s.degaussSound);
        Tip("Play the degaussing coil's hum.");
    }
    {
        const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("[ CLOSE ]", ImVec2(bw, 0.f))) s.menuVisible = false;
        Tip("Hide the settings panel (Ctrl+Shift+C to re-open).");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button,        kRed);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRedH);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kRedA);
        if (ImGui::Button("[ EXIT ]", ImVec2(bw, 0.f))) RequestQuit();
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
        ImGui::Checkbox("Show the menu in the stream", &s.streamShowMenu);
        Tip("Puts this menu (and the cursor, while the menu is open) into the stream window too,\n"
            "so viewers can see it.  It shows where the menu sits over the stream's source.");
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
        DrawDegaussControls(s);
        DrawAppInfo(s, result, true);
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
        if (ImGui::Button("QUIT", ImVec2(btnW, 0.f))) RequestQuit();
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
        ImGui::PushItemWidth(-SliderLabelColumn());
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
        if (ImGui::Button("[F10] QUIT",  ImVec2(bw, 0.f))) RequestQuit();
        Tip("Quit the overlay.");
        ImGui::PopStyleColor(6);
    } else {
        // ---- Section view — BACK is inline, right after the last slider ----
        if (ImGui::BeginChild("##dos_sec", ImVec2(0.f, 0.f), false)) {
            ImGui::PushItemWidth(-SliderLabelColumn());
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
            ImGui::PushItemWidth(-SliderLabelColumn());
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
    if (ImGui::Button("QUIT", ImVec2(bw, 0.f))) RequestQuit();
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
    ImGui::PushItemWidth(-SliderLabelColumn());

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

    if (ImGui::CollapsingHeader("Menu & App", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawStyleCombo(s);
        DrawMenuSizeControls(s);
        DrawDegaussControls(s);
        DrawAppInfo(s, result, true);
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
            RequestQuit();
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

// The GitHub mark (Octicons "mark-github", MIT licence — see
// src/render/assets/Octicons-LICENSE.txt), flattened from its SVG path into a single
// outline in a 1 × 1 box — clockwise on screen, the order ImGui's concave fill needs.
static constexpr ImVec2 kGitHubMark[] = {
    {0.4184f,0.7119f}, {0.4139f,0.7162f}, {0.4091f,0.7209f}, {0.4042f,0.7259f}, {0.3990f,0.7313f},
    {0.3936f,0.7370f}, {0.3879f,0.7429f}, {0.3818f,0.7490f}, {0.3737f,0.7568f}, {0.3660f,0.7634f},
    {0.3586f,0.7688f}, {0.3513f,0.7731f}, {0.3439f,0.7763f}, {0.3362f,0.7785f}, {0.3281f,0.7798f},
    {0.3193f,0.7802f}, {0.3103f,0.7796f}, {0.3017f,0.7778f}, {0.2933f,0.7747f}, {0.2853f,0.7704f},
    {0.2774f,0.7647f}, {0.2698f,0.7576f}, {0.2623f,0.7492f}, {0.2549f,0.7393f}, {0.2454f,0.7266f},
    {0.2359f,0.7156f}, {0.2265f,0.7064f}, {0.2169f,0.6989f}, {0.2072f,0.6930f}, {0.1973f,0.6889f},
    {0.1872f,0.6864f}, {0.1768f,0.6856f}, {0.1695f,0.6860f}, {0.1624f,0.6871f}, {0.1558f,0.6890f},
    {0.1499f,0.6914f}, {0.1449f,0.6944f}, {0.1410f,0.6979f}, {0.1386f,0.7018f}, {0.1377f,0.7061f},
    {0.1379f,0.7082f}, {0.1385f,0.7102f}, {0.1395f,0.7121f}, {0.1411f,0.7138f}, {0.1432f,0.7152f},
    {0.1459f,0.7164f}, {0.1493f,0.7172f}, {0.1533f,0.7177f}, {0.1612f,0.7194f}, {0.1683f,0.7223f},
    {0.1746f,0.7263f}, {0.1802f,0.7314f}, {0.1852f,0.7377f}, {0.1898f,0.7450f}, {0.1942f,0.7533f},
    {0.1983f,0.7627f}, {0.2090f,0.7858f}, {0.2210f,0.8060f}, {0.2344f,0.8233f}, {0.2493f,0.8375f},
    {0.2659f,0.8487f}, {0.2845f,0.8567f}, {0.3052f,0.8616f}, {0.3281f,0.8632f}, {0.3341f,0.8631f},
    {0.3401f,0.8626f}, {0.3462f,0.8617f}, {0.3523f,0.8606f}, {0.3583f,0.8592f}, {0.3641f,0.8575f},
    {0.3697f,0.8556f}, {0.3750f,0.8535f}, {0.3750f,0.9316f}, {0.3739f,0.9411f}, {0.3706f,0.9495f},
    {0.3656f,0.9566f}, {0.3590f,0.9623f}, {0.3512f,0.9662f}, {0.3425f,0.9683f}, {0.3330f,0.9682f},
    {0.3232f,0.9658f}, {0.2999f,0.9566f}, {0.2771f,0.9462f}, {0.2548f,0.9347f}, {0.2331f,0.9220f},
    {0.2122f,0.9082f}, {0.1919f,0.8934f}, {0.1724f,0.8776f}, {0.1538f,0.8608f}, {0.1360f,0.8431f},
    {0.1192f,0.8245f}, {0.1033f,0.8051f}, {0.0884f,0.7849f}, {0.0746f,0.7639f}, {0.0619f,0.7423f},
    {0.0503f,0.7201f}, {0.0398f,0.6973f}, {0.0305f,0.6740f}, {0.0224f,0.6502f}, {0.0155f,0.6261f},
    {0.0099f,0.6016f}, {0.0055f,0.5769f}, {0.0024f,0.5520f}, {0.0006f,0.5270f}, {0.0000f,0.5019f},
    {0.0101f,0.4010f}, {0.0391f,0.3069f}, {0.0850f,0.2217f}, {0.1459f,0.1474f}, {0.2197f,0.0860f},
    {0.3045f,0.0396f}, {0.3982f,0.0102f}, {0.4990f,0.0000f}, {0.5999f,0.0102f}, {0.6939f,0.0396f},
    {0.7790f,0.0860f}, {0.8531f,0.1473f}, {0.9144f,0.2217f}, {0.9606f,0.3069f}, {0.9898f,0.4010f},
    {1.0000f,0.5019f}, {0.9937f,0.5818f}, {0.9756f,0.6570f}, {0.9468f,0.7267f}, {0.9086f,0.7901f},
    {0.8620f,0.8464f}, {0.8084f,0.8948f}, {0.7489f,0.9346f}, {0.6846f,0.9649f}, {0.6738f,0.9680f},
    {0.6638f,0.9686f}, {0.6547f,0.9669f}, {0.6467f,0.9631f}, {0.6401f,0.9574f}, {0.6351f,0.9500f},
    {0.6319f,0.9410f}, {0.6308f,0.9307f}, {0.6308f,0.8291f}, {0.6298f,0.8093f}, {0.6268f,0.7904f},
    {0.6220f,0.7725f}, {0.6154f,0.7560f}, {0.6074f,0.7409f}, {0.5979f,0.7276f}, {0.5871f,0.7162f},
    {0.5752f,0.7070f}, {0.6223f,0.6974f}, {0.6650f,0.6814f}, {0.7028f,0.6594f}, {0.7351f,0.6321f},
    {0.7612f,0.5999f}, {0.7806f,0.5634f}, {0.7927f,0.5231f}, {0.7969f,0.4795f}, {0.7961f,0.4605f},
    {0.7938f,0.4418f}, {0.7900f,0.4234f}, {0.7847f,0.4055f}, {0.7779f,0.3883f}, {0.7697f,0.3718f},
    {0.7600f,0.3563f}, {0.7490f,0.3418f}, {0.7531f,0.3279f}, {0.7558f,0.3115f}, {0.7573f,0.2937f},
    {0.7575f,0.2753f}, {0.7564f,0.2572f}, {0.7541f,0.2403f}, {0.7507f,0.2256f}, {0.7461f,0.2139f},
    {0.7309f,0.2132f}, {0.7147f,0.2147f}, {0.6980f,0.2181f}, {0.6813f,0.2233f}, {0.6650f,0.2299f},
    {0.6496f,0.2379f}, {0.6354f,0.2469f}, {0.6231f,0.2568f}, {0.6097f,0.2530f}, {0.5959f,0.2497f},
    {0.5816f,0.2469f}, {0.5668f,0.2445f}, {0.5514f,0.2427f}, {0.5353f,0.2413f}, {0.5185f,0.2405f},
    {0.5010f,0.2402f}, {0.4834f,0.2405f}, {0.4666f,0.2414f}, {0.4505f,0.2427f}, {0.4349f,0.2446f},
    {0.4199f,0.2471f}, {0.4053f,0.2501f}, {0.3910f,0.2537f}, {0.3769f,0.2578f}, {0.3643f,0.2476f},
    {0.3500f,0.2383f}, {0.3346f,0.2302f}, {0.3184f,0.2234f}, {0.3018f,0.2182f}, {0.2852f,0.2147f},
    {0.2692f,0.2132f}, {0.2539f,0.2139f}, {0.2490f,0.2262f}, {0.2453f,0.2414f}, {0.2429f,0.2586f},
    {0.2417f,0.2769f}, {0.2418f,0.2953f}, {0.2432f,0.3130f}, {0.2459f,0.3291f}, {0.2500f,0.3427f},
    {0.2396f,0.3567f}, {0.2303f,0.3721f}, {0.2223f,0.3886f}, {0.2156f,0.4060f}, {0.2102f,0.4241f},
    {0.2063f,0.4425f}, {0.2039f,0.4611f}, {0.2031f,0.4795f}, {0.2073f,0.5232f}, {0.2193f,0.5637f},
    {0.2387f,0.6005f}, {0.2646f,0.6329f}, {0.2967f,0.6605f}, {0.3341f,0.6826f}, {0.3764f,0.6986f},
    {0.4229f,0.7080f},
};
static constexpr int kGitHubMarkCount = static_cast<int>(sizeof(kGitHubMark) / sizeof(kGitHubMark[0]));

// The GitHub mark, size px square, centred on c: filled, or as points for an outline.
static void GitHubMark(ImDrawList* dl, ImVec2 c, float size, ImU32 col) {
    for (const ImVec2& p : kGitHubMark)
        dl->PathLineTo(ImVec2(c.x + (p.x - 0.5f) * size, c.y + (p.y - 0.5f) * size));
    dl->PathFillConcave(col);
}
static void GitHubMarkPoints(ImVec2 c, float size, ImVec2 out[kGitHubMarkCount]) {
    for (int i = 0; i < kGitHubMarkCount; ++i)
        out[i] = ImVec2(c.x + (kGitHubMark[i].x - 0.5f) * size, c.y + (kGitHubMark[i].y - 0.5f) * size);
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
    // The GitHub link at the bottom centre; a = the footer band's top-left,
    // b = the panel's bottom-right.  The link must be the last item drawn.
    virtual bool   GitHubButton(const char* id, ImVec2 a, ImVec2 b) = 0;

    // Easter eggs.  Update runs at the start of every frame.  While
    // Takeover() is true the style owns the page area — for a hidden page or
    // a game: DrawTakeover draws it in place of the page content (a..b, in
    // screen space), keyboard navigation is off so the keys are free, and
    // Esc or BACK calls EndTakeover.  DrawExtras runs after the footer for
    // anything clickable outside the page area; a..b is the panel.
    virtual void   Update(CRTSettings&, float /*t*/) {}
    virtual bool   Takeover() const { return false; }
    virtual void   DrawTakeover(ImVec2 /*a*/, ImVec2 /*b*/, CRTSettings&, float /*t*/) {}
    virtual void   EndTakeover(float t) { page = -1; pageTime = t; }
    virtual void   DrawExtras(ImVec2 /*a*/, ImVec2 /*b*/, float /*footerY*/, CRTSettings&, float /*t*/) {}

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
        if (st.Takeover()) st.EndTakeover(t);
        st.openTime = t;
        st.page     = -1;
        st.pageTime = t;
    }
    st.lastFrame = ImGui::GetFrameCount();
    auto goTo = [&](int page) { st.page = page; st.pageTime = t; };
    // Esc and BACK: leave a hidden page or game, else go back to the main page.
    auto back = [&]() { if (st.Takeover()) st.EndTakeover(t); else goTo(-1); };

    // Easter-egg progress and high scores are saved as soon as they change.
    const int         coins0 = s.adv64RedCoins, vecHigh0 = s.vectorHighScore, arcHigh0 = s.arcadeHighScore;
    const int         hits0  = s.webHits;
    const size_t      book0  = s.guestbook.size();
    const bool        gold0  = s.adv64Gold;
    const std::string name0  = s.vectorHighName;
    auto eggsChanged = [&] {
        return s.adv64RedCoins != coins0 || s.adv64Gold != gold0 || s.vectorHighScore != vecHigh0 ||
               s.vectorHighName != name0 || s.arcadeHighScore != arcHigh0 || s.webHits != hits0 ||
               s.guestbook.size() != book0;
    };

    st.Update(s, t);
    // While a style has taken over the page area its keys are its own.
    if (st.Takeover()) ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;

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
                      ImGuiWindowFlags_NoBackground | (st.Takeover() ? ImGuiWindowFlags_NoScrollbar : 0));
    {
        const float pageReveal = st.PageReveal(t - st.pageTime);
        if (pageReveal < 1.f) {
            const ImVec2 cp = ImGui::GetWindowPos(), cs = ImGui::GetWindowSize();
            ImGui::PushClipRect(cp, ImVec2(cp.x + cs.x, cp.y + cs.y * pageReveal), true);
        }

        const float avail  = ImGui::GetContentRegionAvail().x;
        const int   cols   = st.Columns();
        // The gutter is matched on the right so the grid stays centred; with no
        // gutter, a small right margin still leaves room for a hover wobble.
        const float gutter = Sc(st.Gutter()), gap = Sc(st.ColumnGap());
        const float right  = std::max(gutter, Sc(12.f));
        const float bw     = (avail - gutter - right - gap * (cols - 1)) / cols;
        auto gridCell = [&](int i) {
            if (i % cols == 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gutter);
            else               ImGui::SameLine(0.f, gap);
        };
        const ImVec2 itemSize(bw, Sc(st.ItemButtonHeight()));

        if (st.Takeover()) {
            const ImVec2 cp = ImGui::GetWindowPos(), cs = ImGui::GetWindowSize();
            st.DrawTakeover(cp, ImVec2(cp.x + cs.x, cp.y + cs.y), s, t);
        } else if (st.page < 0) {
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
            // The styles as a grid of buttons, the panel-size sliders, then
            // the version and updates.  (The GitHub link is in the footer.)
            for (int i = 0; i < kStyleCount; ++i) {
                gridCell(i);
                ImGui::PushID(i);
                if (st.GroupButton("##style", kStyles[i].name, itemSize, i, s.menuStyle == kStyles[i].id,
                                   PagedMenuStyle::Kind::Style))
                    s.menuStyle = kStyles[i].id;
                ImGui::PopID();
            }
            ImGui::Dummy(ImVec2(0.f, Sc(14.f)));
            ImGui::PushItemWidth(-SliderLabelColumn());
            DrawMenuSizeControls(s);
            ImGui::PopItemWidth();
            DrawDegaussControls(s);
            DrawAppInfo(s, result, false);
        } else {
            ImGui::PushItemWidth(-SliderLabelColumn());
            DrawSectionContent(st.page, s, monitors, selectedMon, secondaryMons,
                               streamOpen, presets, audioDevices, result);
            ImGui::PopItemWidth();
        }
        ImGui::Dummy(ImVec2(0.f, Sc(8.f)));   // room for the last button's shadow

        if (pageReveal < 1.f) ImGui::PopClipRect();
    }
    ImGui::EndChild();

    // Esc steps back — unless it's closing a popup (e.g. the quit dialog).
    const bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool inPage = st.page >= 0 || st.Takeover();
    if (inPage && !popupOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive())
        back();

    // Footer — BACK on the left (not on the main page), the GitHub link in
    // the middle, QUIT on the right.
    const float footerY = panelH - Sc(st.FooterRow());
    const float padX    = ImGui::GetCursorStartPos().x;   // this window's padding
    const float inset   = Sc(st.FooterInset());
    const ImVec2 footerA(pa.x, pa.y + footerY);
    st.DrawFooter(dl, footerA, pb, st.page, t);
    if (inPage) {
        ImGui::SetCursorPos(ImVec2(padX + inset, footerY));
        if (st.BackButton("##back")) back();
        Tip("Back to the main menu (Esc).");
    }
    ImGui::SetCursorPos(ImVec2(panelW - padX - inset - st.QuitWidth(), footerY));
    if (st.QuitButton("##quit")) RequestQuit();
    Tip("Quit the overlay application entirely.");

    // These may sit in the window padding, so widen the clip rect to reach them.
    ImGui::PushClipRect(clipMin, clipMax, false);
    if (st.GitHubButton("##github", footerA, pb)) result.openProjectPage = true;
    Tip(kGitHubTip);
    st.DrawExtras(pa, pb, footerA.y, s, t);
    if (st.CloseButton("##close", st.CloseCenter(pa, pb))) s.menuVisible = false;
    Tip("Close the settings panel (Ctrl+Shift+C to re-open).");
    ImGui::PopClipRect();

    if (reveal < 1.f) ImGui::PopClipRect();
    ImGui::PopFont();
    ImGui::End();
    result.saveSettings = eggsChanged();
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
constexpr ImU32 kGitHubInk = IM_COL32( 36,  41,  47, 255);  // GitHub's near-black
// Gold mode (all eight red coins found): every button in shades of gold.
constexpr ImU32 kGoldPalette[4] = { IM_COL32(255, 196, 24, 255), IM_COL32(236, 160, 20, 255),
                                    IM_COL32(255, 214, 90, 255), IM_COL32(214, 140, 16, 255) };

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
// pill for words like START; a null glyph is the GitHub mark) followed by a
// label.
float PromptGlyphWidth(const char* glyph, float fontSize) {
    const float d = fontSize * 1.6f;
    if (!glyph) return d;
    return std::max(d, TextSize(fontSize * (glyph[1] ? 0.70f : 1.0f), glyph).x + d * 0.6f);
}

float PromptWidth(const char* glyph, const char* label, float fontSize) {
    return PromptGlyphWidth(glyph, fontSize) + Sc(10.f) + TextSize(fontSize, label).x;
}

// Draws a prompt at the cursor.  Returns true when clicked.
bool Prompt(const char* id, const char* glyph, ImU32 color, const char* label, float fontSize) {
    const float  d   = fontSize * 1.6f;
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
    if (glyph) {
        const float  gfs = fontSize * (glyph[1] ? 0.70f : 1.0f);
        const ImVec2 gs  = TextSize(gfs, glyph);
        CartoonText(dl, gfs, ImVec2((a.x + b.x - gs.x) * 0.5f, (a.y + b.y - gs.y) * 0.5f), kWhite, glyph);
    } else {
        GitHubMark(dl, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), d * 0.66f, kWhite);
    }
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
                            Palette(i % 4), ch);
            x += TextSize(size, ch).x + gap;
        }
        titleMin_ = ImVec2(a.x + (b.x - a.x - width) * 0.5f, a.y + Sc(20.f));
        titleMax_ = ImVec2(titleMin_.x + width, a.y + Sc(80.f));
    }

    bool GroupButton(const char* id, const char* label, ImVec2 size, int index, bool selected, Kind kind) override {
        return BubbleButton(id, label, size, Palette((index / 2 + index) % 4),
                            Sc(kind == Kind::Group ? 22.f : 19.f), selected);
    }
    bool   BackButton(const char* id) override { return Prompt(id, "B", kGreen, "BACK", Sc(20.f)); }
    float  QuitWidth() const override { return PromptWidth("C", "QUIT", Sc(20.f)); }
    bool   QuitButton(const char* id) override { return Prompt(id, "C", kYellow, "QUIT", Sc(20.f)); }
    ImVec2 CloseCenter(ImVec2 a, ImVec2 b) const override { return ImVec2(b.x - Sc(48.f), a.y + Sc(48.f)); }
    bool   CloseButton(const char* id, ImVec2 c) override { return adv64::CloseButton(id, c, Sc(22.f)); }
    bool   GitHubButton(const char* id, ImVec2 a, ImVec2 b) override {
        const float fs = Sc(20.f);
        ImGui::SetCursorScreenPos(ImVec2((a.x + b.x - PromptWidth(nullptr, "GitHub", fs)) * 0.5f, a.y));
        return Prompt(id, nullptr, kGitHubInk, "GitHub", fs);
    }

    // Easter egg: eight red coins, one hidden on each of eight pages.
    // Finding them all turns every button gold; once they're all found,
    // clicking the title switches gold mode on and off.
    void Update(CRTSettings& s, float) override { gold_ = s.adv64Gold && AllCoins(s); }

    void DrawExtras(ImVec2 a, ImVec2 b, float footerY, CRTSettings& s, float t) override {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // This page's coin, if it has one still to find.
        for (int i = 0; i < kCoinCount; ++i) {
            if (kCoins[i].page != page || (s.adv64RedCoins & (1 << i))) continue;
            const CoinSpot& cs = kCoins[i];
            const ImVec2 c(cs.left ? a.x + Sc(cs.x) : b.x - Sc(cs.x),
                           cs.footer ? footerY + Sc(16.f) : a.y + (b.y - a.y) * cs.y);
            const float r = Sc(11.f);
            ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##coin", ImVec2(r * 2.f, r * 2.f))) {
                s.adv64RedCoins |= 1 << i;
                collectTime_ = t;
                collectAt_   = c;
                if (AllCoins(s)) s.adv64Gold = true;   // the prize
            }
            ImGui::PopID();
            Coin(dl, ImVec2(c.x, c.y + Sc(2.f) * sinf(t * 3.f + i)), r, t + i * 0.37f);
        }

        // A collected coin flies up, with the running count.  Drawn on top of
        // everything, since it crosses the page area.
        const float since = t - collectTime_;
        if (since < 1.6f) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            fg->PushClipRect(a, b, true);
            if (since < 0.4f) {
                const float k = since / 0.4f;
                Coin(fg, ImVec2(collectAt_.x, collectAt_.y - Sc(50.f) * k), Sc(11.f) * (1.f - 0.5f * k), t * 3.f);
            }
            const int  found = CoinsFound(s);
            const bool all   = found == kCoinCount;
            char buf[32];
            if (all) snprintf(buf, sizeof(buf), "ALL %d RED COINS!", kCoinCount);
            else     snprintf(buf, sizeof(buf), "RED COIN %d/%d", found, kCoinCount);
            const float  fs = Sc(all ? 40.f : 30.f) * (0.8f + 0.2f * EaseOutBack(since / 0.3f));
            const ImVec2 ts = TextSize(fs, buf);
            const ImVec2 p((a.x + b.x - ts.x) * 0.5f, a.y + (b.y - a.y) * 0.42f - Sc(20.f) * since);
            CartoonText(fg, fs, p, all ? kYellow : kRed, buf);
            if (all)
                for (int k = 0; k < 12; ++k) {
                    const float ang = k * 0.5235988f + t;
                    const float rad = Sc(60.f) + since * Sc(220.f);
                    Star(fg, ImVec2((a.x + b.x) * 0.5f + cosf(ang) * rad, p.y + ts.y * 0.5f + sinf(ang) * rad * 0.6f),
                         Sc(12.f), t * 4.f + k, kYellow);
                }
            fg->PopClipRect();
        }

        // Once they're all found, the title toggles gold mode.
        if (AllCoins(s) && titleMax_.x > titleMin_.x) {
            ImGui::SetCursorScreenPos(titleMin_);
            if (ImGui::InvisibleButton("##goldtitle", ImVec2(titleMax_.x - titleMin_.x, titleMax_.y - titleMin_.y)))
                s.adv64Gold = !s.adv64Gold;
            Tip(s.adv64Gold ? "All 8 red coins found! Click to switch gold mode off."
                            : "All 8 red coins found! Click to switch gold mode on.");
        }
    }

private:
    // Where each coin hides: on page `page`, x in from the left or right edge
    // of the panel (the side margin), at height y (a fraction of the panel)
    // or in the footer row.
    struct CoinSpot { int page; bool left; float x; float y; bool footer; };
    static constexpr CoinSpot kCoins[] = {
        {  0, true,   24.f, 0.40f, false },   // Capture Source - left margin
        {  2, false,  24.f, 0.62f, false },   // Geometry - right margin
        {  4, true,  190.f, 0.f,   true  },   // Glow - footer, left of centre
        {  6, true,   24.f, 0.74f, false },   // Vignette - left margin, low
        {  8, false,  24.f, 0.30f, false },   // Static - right margin, high
        { 10, false, 190.f, 0.f,   true  },   // Colour - footer, right of centre
        { 12, true,   24.f, 0.22f, false },   // Streaming - left margin, high
        { 13, false,  24.f, 0.82f, false },   // Menu & App - right margin, low
    };
    static constexpr int kCoinCount = static_cast<int>(sizeof(kCoins) / sizeof(kCoins[0]));

    static int CoinsFound(const CRTSettings& s) {
        int n = 0;
        for (int i = 0; i < kCoinCount; ++i) n += (s.adv64RedCoins >> i) & 1;
        return n;
    }
    static bool AllCoins(const CRTSettings& s) { return CoinsFound(s) == kCoinCount; }

    ImU32 Palette(int i) const { return gold_ ? kGoldPalette[i] : kPalette[i]; }

    // A spinning red coin: an ellipse that narrows edge-on, with a rim and a glint.
    static void Coin(ImDrawList* dl, ImVec2 c, float r, float t) {
        const float w = r * std::max(0.12f, fabsf(cosf(t * 3.f)));
        dl->AddEllipseFilled(ImVec2(c.x + Sc(1.5f), c.y + Sc(3.f)), ImVec2(w, r), IM_COL32(0, 0, 0, 110));
        dl->AddEllipseFilled(c, ImVec2(w, r), kInk);
        dl->AddEllipseFilled(c, ImVec2(std::max(0.5f, w - Sc(2.f)), r - Sc(2.f)), IM_COL32(170, 20, 30, 255));
        dl->AddEllipseFilled(c, ImVec2(std::max(0.5f, w * 0.62f), r * 0.62f), kRed);
        dl->AddEllipseFilled(ImVec2(c.x - w * 0.25f, c.y - r * 0.35f), ImVec2(std::max(0.5f, w * 0.22f), r * 0.2f),
                             IM_COL32(255, 255, 255, 150));
    }

    bool   gold_        = false;
    float  collectTime_ = -10.f;
    ImVec2 collectAt_;
    ImVec2 titleMin_, titleMax_;
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
// power scales the flame and its light: above 1 it flares white-gold, below
// 1 it gutters.
void Torch(ImDrawList* dl, ImVec2 root, float t, float seed, float power = 1.f) {
    const float fl = Flicker(t, seed);
    const ImU32 light = power > 1.f ? IM_COL32(255, 200, 90, 14) : IM_COL32(255, 140, 40, 8);
    for (int i = 0; i < 7; ++i)
        dl->AddCircleFilled(ImVec2(root.x, root.y - Sc(14.f)),
                            Sc(150.f - i * 18.f) * (0.92f + 0.12f * fl) * std::sqrt(power), light, 48);

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
        const float w     = Sc(12.f - k * 2.6f) * (0.9f + 0.15f * fl) * std::min(power, 1.3f);
        const float h     = Sc(34.f - k * 6.f) * (0.85f + 0.3f * Flicker(t * 1.3f, seed + k)) * power;
        const float sway  = Sc(4.f) * sinf(t * 5.f + seed + k * 0.6f);
        const ImU32 col   = power > 1.f ? Mix(kFire[k], kFire[3], 0.45f) : kFire[k];
        Teardrop(dl, ImVec2(root.x, root.y - Sc(4.f) - Sc(1.5f) * k), w, h, sway, col);
    }

    // Embers drifting up and fading.
    const int embers = power > 1.f ? 16 : power < 1.f ? 1 : 6;
    for (int i = 0; i < embers; ++i) {
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

// Bone skull with glowing eyes, centred on c; u is its height scale.
void Skull(ImDrawList* dl, ImVec2 c, float u, float t, ImU32 eyeGlow = kEyeGlow) {
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
            dl->AddCircleFilled(e, u * 0.05f * (1.f + i * 0.6f), WithAlpha(eyeGlow, 0.12f * g), 16);
        dl->AddCircleFilled(e, u * 0.05f, WithAlpha(eyeGlow, 0.55f + 0.45f * g), 12);
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
        const float power = TorchPower(t);
        Torch(dl, ImVec2(a.x + Sc(80.f), a.y + Sc(72.f)), t, 0.f, power);
        Torch(dl, ImVec2(b.x - Sc(80.f), a.y + Sc(72.f)), t, 2.7f, power);
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
    // A skull keeps watch between BACK and QUIT (drawn in DrawExtras, since it's clickable).
    ImVec2 CloseCenter(ImVec2 a, ImVec2 b) const override { return ImVec2(b.x - Sc(36.f), a.y + Sc(36.f)); }
    bool   CloseButton(const char* id, ImVec2 c) override { return CloseSeal(id, c, Sc(22.f)); }

    // A bronze maker's plate riveted to the middle of the frame's bottom edge.
    bool GitHubButton(const char* id, ImVec2 a, ImVec2 b) override {
        const ImVec2 size(Sc(136.f), Sc(26.f));
        const ImVec2 p0((a.x + b.x - size.x) * 0.5f, b.y - Sc(10.f) - size.y * 0.5f);
        const ImVec2 p1(p0.x + size.x, p0.y + size.y);
        ImGui::SetCursorScreenPos(p0);
        const bool clicked = ImGui::InvisibleButton(id, size);
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        if (hovered)
            for (int i = 4; i >= 1; --i) {
                const float g = Sc(2.f) * i;
                dl->AddRectFilled(ImVec2(p0.x - g, p0.y - g), ImVec2(p1.x + g, p1.y + g), WithAlpha(kGold, 0.08f), Sc(4.f) + g);
            }
        dl->AddRectFilled(ImVec2(p0.x + Sc(2.f), p0.y + Sc(3.f)), ImVec2(p1.x + Sc(2.f), p1.y + Sc(3.f)),
                          IM_COL32(0, 0, 0, 150), Sc(4.f));
        dl->AddRectFilled(p0, p1, kShadow, Sc(4.f));
        const int v0 = dl->VtxBuffer.Size;
        dl->AddRectFilled(ImVec2(p0.x + Sc(1.5f), p0.y + Sc(1.5f)), ImVec2(p1.x - Sc(1.5f), p1.y - Sc(1.5f)),
                          IM_COL32_WHITE, Sc(3.f));
        ShadeVerts(dl, v0, [&](ImVec2 q) {
            return hovered ? VGrad3(q.y, p0.y, p1.y, kGoldHi, kGold, kGoldLo)
                           : VGrad3(q.y, p0.y, p1.y, kGoldHi, kBronzeHi, kBronze);
        });
        Stud(dl, ImVec2(p0.x + Sc(8.f), (p0.y + p1.y) * 0.5f), Sc(2.4f));
        Stud(dl, ImVec2(p1.x - Sc(8.f), (p0.y + p1.y) * 0.5f), Sc(2.4f));

        // Stamped into the metal: the mark and the name.
        ImFont*      f   = BodyFont();
        const float  fs  = Sc(16.f);
        const ImVec2 ts  = f->CalcTextSizeA(fs, FLT_MAX, 0.f, "GitHub");
        const float  ms  = Sc(16.f), gap = Sc(6.f);
        const float  x0  = (p0.x + p1.x - (ms + gap + ts.x)) * 0.5f, cy = (p0.y + p1.y) * 0.5f;
        const ImU32  ink = IM_COL32(48, 26, 8, 255);
        GitHubMark(dl, ImVec2(x0 + ms * 0.5f + Sc(0.6f), cy + Sc(0.6f)), ms, IM_COL32(255, 236, 200, 70));
        GitHubMark(dl, ImVec2(x0 + ms * 0.5f, cy), ms, ink);
        dl->AddText(f, fs, ImVec2(x0 + ms + gap + Sc(0.6f), cy - ts.y * 0.5f + Sc(0.6f)), IM_COL32(255, 236, 200, 70), "GitHub");
        dl->AddText(f, fs, ImVec2(x0 + ms + gap, cy - ts.y * 0.5f), ink, "GitHub");
        return clicked;
    }

    // Easter egg: click the skull to roll a d20.  It tumbles, lands and the
    // skull speaks your fortune.  A natural 20 makes the torches flare; a 1
    // makes them gutter.
    void DrawExtras(ImVec2 a, ImVec2 b, float footerY, CRTSettings&, float t) override {
        const ImVec2 c((a.x + b.x) * 0.5f, footerY + Sc(18.f));
        const float  u = Sc(44.f);
        ImGui::SetCursorScreenPos(ImVec2(c.x - u * 0.5f, c.y - u * 0.6f));
        if (ImGui::InvisibleButton("##skull", ImVec2(u, u * 1.1f)) && t - rollTime_ > kTumble) {
            rollTime_ = t;
            seed_     = seed_ * 1664525u + 1013904223u + static_cast<unsigned>(t * 1000.f);
            roll_     = 1 + static_cast<int>((seed_ >> 8) % 20u);
        }
        const bool hovered = ImGui::IsItemHovered();
        Tip("Roll the bones?");

        const float since = t - rollTime_;
        const bool  shown = since < kTumble + kFortune;
        ImU32 eyes = kEyeGlow;
        if (shown && since >= kTumble) eyes = roll_ == 20 ? kGoldHi : roll_ == 1 ? IM_COL32(255, 40, 30, 255) : kEyeGlow;
        else if (hovered)              eyes = Mix(kEyeGlow, IM_COL32_WHITE, 0.5f);
        Skull(ImGui::GetWindowDrawList(), c, u, t, eyes);
        if (!shown) return;

        // The die and the fortune float above the skull, over the page.
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->PushClipRect(a, b, true);
        const float fade = std::clamp((kTumble + kFortune - since) / 0.5f, 0.f, 1.f);
        const int   face = since < kTumble ? 1 + static_cast<int>(Hash(floorf(since / 0.06f), 7.f) * 20.f) % 20 : roll_;
        const ImVec2 dc(c.x, c.y - u * 1.35f - (since < kTumble ? Sc(18.f) * fabsf(sinf(since * 11.f)) : 0.f));
        D20(dl, dc, Sc(26.f), since < kTumble ? since * 9.f : 0.f, face, fade);

        if (since >= kTumble) {
            const char* text = Fortune(roll_);
            ImFont*     f    = BodyFont();
            const float fs   = Sc(16.f), wrap = Sc(380.f);
            const ImVec2 ts  = f->CalcTextSizeA(fs, FLT_MAX, wrap, text);
            const ImVec2 s0(c.x - ts.x * 0.5f - Sc(18.f), dc.y - Sc(36.f) - ts.y - Sc(20.f));
            const ImVec2 s1(c.x + ts.x * 0.5f + Sc(18.f), dc.y - Sc(36.f));
            Scroll(dl, s0, s1, fade);
            const ImU32 ink = roll_ == 20 ? IM_COL32(150, 90, 0, 255) : roll_ == 1 ? kBlood : IM_COL32(60, 34, 14, 255);
            dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f, s0.y + Sc(10.f)), WithAlpha(ink, fade), text, nullptr, wrap);
        }
        dl->PopClipRect();
    }

private:
    static ImVec2 FooterButton() { return ImVec2(Sc(132.f), Sc(40.f)); }

    static constexpr float kTumble  = 0.9f;   // seconds the die tumbles
    static constexpr float kFortune = 5.0f;   // seconds the fortune shows

    // Torch power after a roll: a natural 20 flares them, a 1 gutters them.
    float TorchPower(float t) const {
        const float since = t - rollTime_ - kTumble;
        if (since < 0.f || since > kFortune + 1.f || (roll_ != 20 && roll_ != 1)) return 1.f;
        const float k = std::min(1.f, since / 0.25f) * std::clamp((kFortune + 1.f - since) / 1.f, 0.f, 1.f);
        return 1.f + (roll_ == 20 ? 0.8f : -0.65f) * k;
    }

    static const char* Fortune(int roll) {
        static constexpr const char* kFortunes[20] = {
            "CRITICAL FAIL. The torches gutter... check thy cables, mortal.",
            "Thy picture shall roll. Adjust the vertical hold.",
            "Beware the hum bar, for it cometh again.",
            "A goblin hath stolen thy RGB cable.",
            "The phosphors remember what thou didst.",
            "A CRT awaits thee at the kerb. Lift with thy legs.",
            "Seven scanlines bring seven years' good fortune.",
            "Degauss before battle, warrior.",
            "Thy curvature is pleasing to the gods.",
            "Fortune favours the glowing.",
            "A stranger offers thee a SCART lead. Accept it.",
            "Thy convergence is off. So is thy posture.",
            "An ill omen: someone hath set Bloom to max.",
            "Thy save file is safe. For now.",
            "Hark - the dungeon whines at 15 kHz.",
            "Wisdom: blow not into the cartridge.",
            "A shadow mask hides thy true face.",
            "Thy reflexes are as swift as a light gun.",
            "So close to glory. Roll again, mortal.",
            "CRITICAL SUCCESS! The gods bless thy scanlines.",
        };
        return kFortunes[std::clamp(roll, 1, 20) - 1];
    }

    // A d20 seen face-on: a hexagon outline around a central triangle, with
    // the face's number.
    static void D20(ImDrawList* dl, ImVec2 c, float r, float spin, int face, float alpha) {
        ImVec2 hex[6], tri[3];
        for (int i = 0; i < 6; ++i) {
            const float ang = spin + i * 1.0471976f - 1.5707963f;
            hex[i] = ImVec2(c.x + cosf(ang) * r, c.y + sinf(ang) * r);
        }
        for (int i = 0; i < 3; ++i) {
            const float ang = spin + i * 2.0943951f + 1.5707963f;
            tri[i] = ImVec2(c.x + cosf(ang) * r * 0.62f, c.y + sinf(ang) * r * 0.62f);
        }
        dl->AddConvexPolyFilled(hex, 6, WithAlpha(IM_COL32(120, 24, 30, 255), alpha));
        dl->AddConvexPolyFilled(tri, 3, WithAlpha(IM_COL32(170, 40, 44, 255), alpha));
        for (int i = 0; i < 6; ++i) dl->AddLine(tri[(i + 1) / 2 % 3], hex[i], WithAlpha(IM_COL32(60, 8, 12, 255), alpha), Sc(1.2f));
        dl->AddPolyline(hex, 6, WithAlpha(kGold, alpha), ImDrawFlags_Closed, Sc(2.f));
        dl->AddPolyline(tri, 3, WithAlpha(kGoldLo, alpha), ImDrawFlags_Closed, Sc(1.2f));
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", face);
        ImFont*      f  = TitleFont();
        const float  fs = r * 0.95f;
        const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.f, buf);
        dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f + Sc(1.f), c.y - ts.y * 0.5f + Sc(1.5f)), WithAlpha(kShadow, alpha), buf);
        dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), WithAlpha(face == 20 ? kGoldHi : kEngrave, alpha), buf);
    }

    // A parchment scroll filling a..b, rolled at both ends.
    static void Scroll(ImDrawList* dl, ImVec2 a, ImVec2 b, float alpha) {
        const ImU32 paper = WithAlpha(IM_COL32(232, 214, 170, 255), alpha);
        const ImU32 edge  = WithAlpha(IM_COL32(150, 112, 60, 255), alpha);
        dl->AddRectFilled(ImVec2(a.x + Sc(3.f), a.y + Sc(5.f)), ImVec2(b.x + Sc(3.f), b.y + Sc(5.f)),
                          WithAlpha(IM_COL32(0, 0, 0, 140), alpha), Sc(4.f));
        dl->AddRectFilled(a, b, paper, Sc(3.f));
        dl->AddRect(a, b, edge, Sc(3.f), 0, Sc(1.5f));
        for (int side = 0; side < 2; ++side) {
            const float x = side ? b.x : a.x;
            dl->AddRectFilled(ImVec2(x - Sc(6.f), a.y - Sc(4.f)), ImVec2(x + Sc(6.f), b.y + Sc(4.f)),
                              WithAlpha(IM_COL32(208, 184, 132, 255), alpha), Sc(6.f));
            dl->AddRect(ImVec2(x - Sc(6.f), a.y - Sc(4.f)), ImVec2(x + Sc(6.f), b.y + Sc(4.f)), edge, Sc(6.f), 0, Sc(1.5f));
        }
    }

    float    rollTime_ = -100.f;
    int      roll_     = 0;
    unsigned seed_     = 0x5eed1234u;
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
//   • The fastext row along the bottom: red Index (back), yellow GitHub and
//     cyan Quit.
//   • Typing a three-digit page number goes straight to that page, like a
//     real set — including a few pages that aren't on the index.
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
    ImFont* Font()        const override { return teletext::Font(); }
    float   FontSize()    const override { return 18.f; }

    // Once the page is found it paints in, top to bottom, in a blink.
    float PageReveal(float t) const override { return std::clamp((t - kSearchTime) / 0.12f, 0.f, 1.f); }

    void DrawPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, int pageIndex, float) override {
        cell_   = ImVec2((b.x - a.x) / kCols, (b.y - a.y) / kRows);
        panelA_ = a;
        if (pageIndex == kSubtitlesPage) {
            // Subtitles are boxed captions over the programme: only the
            // header and fastext rows stay black.
            dl->AddRectFilled(a, At(a, kCols, 1), kBlack);
            dl->AddRectFilled(At(a, 0, kRows - 1), b, kBlack);
        } else {
            dl->AddRectFilled(a, b, kBlack);
        }
        dl->AddRect(a, b, IM_COL32(70, 70, 70, 255));
    }

    std::string PageTitle(int pageIndex) const override {
        switch (pageIndex) {
        case kNewsPage: return "SEERT NEWS";
        case kQuizPage: return "QUIZ";
        default:        return pageIndex >= kHiddenBase ? "" : PagedMenuStyle::PageTitle(pageIndex);
        }
    }

    void DrawTitle(ImDrawList* dl, ImVec2 a, ImVec2, const char* title, int pageIndex, float t) override {
        const float since = t - pageTime;
        const bool  found = since >= kSearchTime && Exists(pageIndex);   // a page that doesn't exist is never found
        const int   num   = PageNumber(pageIndex);

        // Header: requested page, service name with the (rolling) page
        // counter, date, clock.
        const time_t now = time(nullptr);
        tm lt{};
        localtime_s(&lt, &now);
        char date[32], buf[64];
        strftime(date, sizeof(date), "%a %d %b", &lt);
        if (dial_.empty()) snprintf(buf, sizeof(buf), "P%03d", num);
        else               snprintf(buf, sizeof(buf), "P%s%s", dial_.c_str(), dial_.size() == 1 ? "--" : "-");
        CellText(dl, At(a, 1, 0), cell_, kWhite, buf);
        snprintf(buf, sizeof(buf), "SeeRT %03d %s", found ? num : 100 + static_cast<int>(since * 230.f) % 800, date);
        CellText(dl, At(a, 6, 0), cell_, kWhite, buf);
        snprintf(buf, sizeof(buf), "%02d:%02d/%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
        CellText(dl, At(a, 27, 0), cell_, kYellow, buf);
        if (!found || pageIndex == kSubtitlesPage || pageIndex == kLolPage) return;

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

    // On the index, a hint above the fastext row.
    void DrawFooter(ImDrawList* dl, ImVec2 a, ImVec2, int pageIndex, float) override {
        if (pageIndex >= 0) return;
        const char* hint = "Type a page number to jump to it";
        CellText(dl, ImVec2(a.x + (kCols - static_cast<int>(strlen(hint))) / 2 * cell_.x, a.y - cell_.y), cell_, kCyan, hint);
    }

    // The yellow fastext link, in the middle of the row.
    bool GitHubButton(const char* id, ImVec2 a, ImVec2) override {
        const ImVec2 pos(a.x + (kCols - kFastextCells) / 2 * cell_.x, a.y);
        const ImVec2 size(cell_.x * kFastextCells, cell_.y);
        ImGui::SetCursorScreenPos(pos);
        const bool  clicked = ImGui::InvisibleButton(id, size);
        const bool  hovered = ImGui::IsItemHovered();
        ImDrawList* dl      = ImGui::GetWindowDrawList();
        if (hovered) dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), kYellow);
        const ImU32 col = hovered ? kBlack : kYellow;
        GitHubMark(dl, ImVec2(pos.x + cell_.x * 0.5f, pos.y + cell_.y * 0.5f), std::min(cell_.x, cell_.y) * 0.95f, col);
        CellText(dl, ImVec2(pos.x + cell_.x * 2.f, pos.y), cell_, col, "GitHub");
        return clicked;
    }

    // Easter egg: page numbers typed on the keyboard.  100 is the index,
    // 101 on the groups, 2xx the presets page, 3xx switches menu style; a
    // few pages aren't on the index at all, and any other number is searched
    // for forever, as on a real set.
    void Update(CRTSettings& s, float t) override {
        if (ImGui::GetIO().WantTextInput ||
            ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            return;
        for (int d = 0; d < 10; ++d) {
            if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + d), false) &&
                !ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_Keypad0 + d), false))
                continue;
            if (dial_.empty() && (d == 0 || d == 9)) continue;   // pages run 100-899
            dial_ += static_cast<char>('0' + d);
            if (dial_.size() == 3) {
                Dial(std::stoi(dial_), s, t);
                dial_.clear();
            }
        }
    }

    bool Takeover() const override { return page >= kHiddenBase; }

    void DrawTakeover(ImVec2, ImVec2, CRTSettings&, float t) override {
        if (t - pageTime < kSearchTime) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = panelA_;
        switch (page) {
        case kNewsPage: {
            static constexpr const char* kHeadlines[] = {
                "Scanlines: experts say MORE",
                "Man tunes curvature for 3hrs",
                "Phosphor glow hits record",
                "Halation 'just fancy bloom'",
                "Tracking fixed by a firm tap",
                "Shadow mask seen at 1080p",
                "Hum bars rolling nationwide",
                "Degauss wobble delights all",
            };
            int row = 4;
            for (const char* h : kHeadlines) {
                const int n = static_cast<int>(strlen(h));
                CellText(dl, At(a, 2, row), cell_, row % 2 ? kCyan : kWhite, h);
                CellText(dl, At(a, 3 + n, row), cell_, kBlue, std::string(31 - n, '.').c_str());
                char num[8];
                snprintf(num, sizeof(num), "%d", 151 + (row - 4) / 2);
                CellText(dl, At(a, 35, row), cell_, kYellow, num);
                row += 2;
            }
            CellText(dl, At(a, 2, row + 1), cell_, kGreen, "All the news that's fit to glow");
            break;
        }
        case kLolPage:   // that's the page
            CellText(dl, At(a, (kCols - 3) / 2, 11), ImVec2(cell_.x, cell_.y * 2.f), kWhite, "lol");
            break;
        case kQuizPage:
            DrawQuiz(dl, a);
            break;
        case kSubtitlesPage: {
            struct Line { const char* text; ImU32 col; };
            static constexpr Line kLines[] = {
                { "[GENTLE HUM OF A CRT]",            kWhite  },
                { "Is that... a degauss button?",     kYellow },
                { "Don't touch the back. It bites.",  kCyan   },
                { "[STATIC CRACKLES]",                kWhite  },
                { "Blow on the cartridge. Again.",    kGreen  },
                { "Back then, pixels were ROUND.",    kYellow },
                { "[SCANLINES INTENSIFY]",            kWhite  },
                { "Be kind, rewind.",                 kCyan   },
                { "He should have fixed the tracking.", kGreen },
            };
            constexpr int kCount = static_cast<int>(sizeof(kLines) / sizeof(kLines[0]));
            const Line&   line   = kLines[static_cast<int>((t - pageTime - kSearchTime) / 3.2f) % kCount];
            const int     len    = static_cast<int>(strlen(line.text));
            const int     col    = (kCols - len) / 2;
            dl->AddRectFilled(At(a, col - 1, 19), At(a, col + len + 1, 21), kBlack);
            CellText(dl, At(a, col, 19), ImVec2(cell_.x, cell_.y * 2.f), line.col, line.text);
            break;
        }
        default:
            break;   // still searching
        }
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
    // Pages off the index: page = kHiddenBase + its number.
    static constexpr int kHiddenBase    = 1000;
    static constexpr int kNewsPage      = kHiddenBase + 150;
    static constexpr int kLolPage       = kHiddenBase + 420;
    static constexpr int kQuizPage      = kHiddenBase + 777;
    static constexpr int kSubtitlesPage = kHiddenBase + 888;

    static int  PageNumber(int pageIndex) {
        return pageIndex < 0 ? 100 : pageIndex >= kHiddenBase ? pageIndex - kHiddenBase : 101 + pageIndex;
    }
    static bool Exists(int pageIndex) {
        return pageIndex < kHiddenBase || pageIndex == kNewsPage || pageIndex == kLolPage ||
               pageIndex == kQuizPage || pageIndex == kSubtitlesPage;
    }

    void Dial(int num, CRTSettings& s, float t) {
        pageTime = t;
        if (num == 100)                                     page = -1;
        else if (num >= 101 && num < 101 + kSectionCount)  page = num - 101;
        else if (num >= 201 && num <= 299)                 page = kPresetsSection;
        else if (num >= 301 && num < 301 + kStyleCount)    { s.menuStyle = kStyles[num - 301].id; page = kMenuSection; }
        else                                               page = kHiddenBase + num;
        quiz_ = 0;
        revealed_ = false;
    }

    // Page 777: a multiple-choice quiz with a concealed answer, shown by REVEAL.
    void DrawQuiz(ImDrawList* dl, ImVec2 a) {
        struct Question { const char* q[2]; const char* options[4]; int answer; const char* note; };
        static constexpr Question kQuiz[] = {
            { { "What does CRT stand for?", "" },
              { "Cathode Ray Tube", "Colour Raster Terminal", "Curved Retro Telly", "Cool Rad Thing" },
              0, "Electrons, fired at phosphor." },
            { { "How many lines make up a", "PAL picture?" },
              { "405", "525", "625", "1080" },
              2, "525 was NTSC; 405 came before." },
            { { "The BBC's teletext service", "was called..." },
              { "Oracle", "Ceefax", "Prestel", "Minitel" },
              1, "On air from 1974 to 2012." },
            { { "What clears colour blotches", "off a CRT?" },
              { "Tracking", "Degaussing", "Dithering", "Defragging" },
              1, "That satisfying BWOMMM." },
            { { "Why do light guns need a", "CRT?" },
              { "They hate LCDs", "Beam timing", "Lead glass", "Magnets" },
              1, "The gun times the beam's sweep." },
        };
        constexpr int kCount = static_cast<int>(sizeof(kQuiz) / sizeof(kQuiz[0]));
        const Question& q = kQuiz[quiz_ % kCount];

        char buf[48];
        snprintf(buf, sizeof(buf), "QUESTION %d OF %d", quiz_ % kCount + 1, kCount);
        CellText(dl, At(a, 2, 4), cell_, kCyan, buf);
        CellText(dl, At(a, 2, 6), cell_, kWhite, q.q[0]);
        CellText(dl, At(a, 2, 7), cell_, kWhite, q.q[1]);
        for (int i = 0; i < 4; ++i) {
            const char letter[2] = { static_cast<char>('A' + i), 0 };
            CellText(dl, At(a, 3, 9 + i), cell_, kYellow, letter);
            CellText(dl, At(a, 6, 9 + i), cell_, revealed_ && i == q.answer ? kGreen : kWhite, q.options[i]);
        }

        if (revealed_) {
            snprintf(buf, sizeof(buf), "ANSWER: %c  %s", 'A' + q.answer, q.options[q.answer]);
            CellText(dl, At(a, 2, 15), cell_, kGreen, buf);
            CellText(dl, At(a, 2, 16), cell_, kCyan, q.note);
        } else if (Link(dl, "##reveal", At(a, 2, 15), "Press REVEAL for the answer", kMagenta)) {
            revealed_ = true;
        }
        if (Link(dl, "##next", At(a, 2, 18), quiz_ % kCount + 1 < kCount ? "Next question >" : "Start again >", kYellow)) {
            quiz_ = (quiz_ + 1) % kCount;
            revealed_ = false;
        }
    }

    // Clickable text at a cell, inverted when hovered.
    bool Link(ImDrawList* dl, const char* id, ImVec2 pos, const char* text, ImU32 col) {
        const ImVec2 size(cell_.x * strlen(text), cell_.y);
        ImGui::SetCursorScreenPos(pos);
        const bool clicked = ImGui::InvisibleButton(id, size);
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), col);
        CellText(dl, pos, cell_, hovered ? kBlack : col, text);
        return clicked;
    }

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

    ImVec2      cell_   = ImVec2(18.f, 24.f);   // set from the panel each frame
    ImVec2      panelA_ = ImVec2(0.f, 0.f);     // the panel's top-left, likewise
    std::string dial_;                          // page number being typed
    int         quiz_     = 0;
    bool        revealed_ = false;
};

} // namespace teletext

// ---------------------------------------------------------------------------
// Vector Monitor layout — an Asteroids / Tempest-style vector display.
//   • Everything is drawn as glowing phosphor beams on black, with a stroke
//     font built from line segments; no filled shapes.
//   • A twinkling starfield with slowly tumbling wireframe asteroids behind.
//   • Hovering a button points the little ship at it, thruster flickering.
//   • The panel switches on like a CRT — a bright line opening to full
//     height — and keeps score in the corner: 150 points per page visited.
//   • Click the score to play Asteroids in the page area.
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

// The ship centred on c, pointing at angle ang (radians), thruster lit when
// thrust is true.
void ShipAt(ImDrawList* dl, ImVec2 c, float ang, float s, ImU32 col, bool thrust, float t) {
    const float ca = cosf(ang), sa = sinf(ang);
    auto at = [&](float fwd, float side) { return ImVec2(c.x + ca * fwd - sa * side, c.y + sa * fwd + ca * side); };
    const ImVec2 hull[4] = { at(1.2f * s, 0.f), at(-0.8f * s, -0.75f * s), at(-0.4f * s, 0.f), at(-0.8f * s, 0.75f * s) };
    Beam(dl, hull, 4, col, Sc(1.3f), true);
    if (thrust && fmodf(t * 17.f, 1.f) < 0.65f) {
        const ImVec2 flame[3] = { at(-0.55f * s, -0.35f * s), at(-(1.3f + 0.4f * Hash(floorf(t * 30.f), 1.f)) * s, 0.f),
                                  at(-0.55f * s, 0.35f * s) };
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
        // Tumbling asteroids, drifting and wrapping around the panel (not
        // during a game, where the rocks are real).
        for (int r = 0; r < (game_ == Game::Off ? 4 : 0); ++r) {
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

    // The mark, glowing, and the name in beams, in the middle of the footer.
    bool GitHubButton(const char* id, ImVec2 a, ImVec2 b) override {
        const float  ms = Sc(22.f), h = Sc(12.f), gap = Sc(10.f);
        const float  w  = ms + gap + TextWidth(h, "GITHUB");
        const ImVec2 p((a.x + b.x - w) * 0.5f, a.y + (FooterButton().y - ms) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(p.x - Sc(6.f), p.y - Sc(4.f)));
        const bool  clicked = ImGui::InvisibleButton(id, ImVec2(w + Sc(12.f), ms + Sc(8.f)));
        const bool  hovered = ImGui::IsItemHovered();
        ImDrawList* dl      = ImGui::GetWindowDrawList();
        const ImU32 col     = hovered ? kHot : kBeam;
        const ImVec2 mc(p.x + ms * 0.5f, p.y + ms * 0.5f);
        ImVec2 mark[kGitHubMarkCount];
        GitHubMarkPoints(mc, ms, mark);
        Beam(dl, mark, kGitHubMarkCount, col, Sc(0.8f), true, 0.35f);   // the glow around it
        GitHubMark(dl, mc, ms, WithAlpha(Mix(col, IM_COL32_WHITE, 0.3f), hovered ? 1.f : 0.8f));
        Text(dl, ImVec2(p.x + ms + gap, p.y + (ms - h) * 0.5f), h, col, "GITHUB", hovered ? 1.f : 0.85f);
        return clicked;
    }

    // Score: 150 points per page visited.
    void Update(CRTSettings&, float) override {
        if (page != lastPage_) {
            if (page >= 0) score_ += 150;
            lastPage_ = page;
        }
    }

    // The score and reserve ships in the top-left corner, Asteroids-style,
    // and the high score above the title.  Clicking the score starts a game.
    void DrawExtras(ImVec2 a, ImVec2 b, float, CRTSettings& s, float t) override {
        ImDrawList* dl      = ImGui::GetWindowDrawList();
        const bool  playing = game_ != Game::Off;
        char buf[32];
        snprintf(buf, sizeof(buf), "SCORE %06d", playing ? gameScore_ : score_);
        const float  h = Sc(10.f), w = TextWidth(h, buf);
        const ImVec2 p(a.x + Sc(26.f), a.y + Sc(20.f));
        ImGui::SetCursorScreenPos(ImVec2(p.x - Sc(5.f), p.y - Sc(5.f)));
        if (ImGui::InvisibleButton("##score", ImVec2(w + Sc(10.f), Sc(36.f))) && !playing) StartGame(t);
        const bool hovered = ImGui::IsItemHovered() && !playing;
        if (!playing) Tip("1 PLAYER - PRESS START");
        Text(dl, p, h, hovered ? kHot : kBeam, buf, hovered ? 1.f : 0.85f);
        const int ships = playing ? lives_ : 3;
        for (int i = 0; i < ships; ++i) {
            const float  u = Sc(5.f);
            const ImVec2 n(p.x + Sc(4.f) + i * Sc(16.f), p.y + Sc(17.f));
            const ImVec2 up[4] = { n, ImVec2(n.x + 0.9f * u, n.y + 2.f * u), ImVec2(n.x, n.y + 1.5f * u),
                                   ImVec2(n.x - 0.9f * u, n.y + 2.f * u) };
            Beam(dl, up, 4, hovered ? kHot : kBeam, Sc(1.f), true, 0.8f);
        }
        if (s.vectorHighScore > 0) {
            snprintf(buf, sizeof(buf), "HI %06d %s", s.vectorHighScore, s.vectorHighName.c_str());
            const float hh = Sc(8.f);
            Text(dl, ImVec2((a.x + b.x - TextWidth(hh, buf)) * 0.5f, a.y + Sc(13.f)), hh, kDim, buf, 0.8f);
        }
    }

    bool Takeover() const override { return game_ != Game::Off; }
    void EndTakeover(float) override {
        if (game_ == Game::Initials) commitInitials_ = true;   // keep the score under what's typed
        else                         game_ = Game::Off;
    }

    // Easter egg: Asteroids, in the page area.
    void DrawTakeover(ImVec2 a, ImVec2 b, CRTSettings& s, float t) override {
        const ImVec2 size(b.x - a.x, b.y - a.y);
        const float  dt = std::min(ImGui::GetIO().DeltaTime, 0.05f);
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##field", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool   inField = ImGui::IsItemHovered() || ImGui::IsItemActive();
        const ImVec2 mouse(ImGui::GetIO().MousePos.x - a.x, ImGui::GetIO().MousePos.y - a.y);

        if (needSetup_) {
            ship_      = ImVec2(size.x * 0.5f, size.y * 0.5f);
            shipV_     = ImVec2(0.f, 0.f);
            angle_     = -1.5707963f;
            invuln_    = 2.f;
            needSetup_ = false;
            NewWave(size);
        }
        auto wrap = [&](ImVec2& q) {
            q.x = fmodf(q.x + size.x, size.x);
            q.y = fmodf(q.y + size.y, size.y);
        };

        // --- Step ----------------------------------------------------------
        bool thrust = false;
        if (game_ == Game::Playing && respawn_ <= 0.f) {
            const bool left  = ImGui::IsKeyDown(ImGuiKey_LeftArrow)  || ImGui::IsKeyDown(ImGuiKey_A);
            const bool right = ImGui::IsKeyDown(ImGuiKey_RightArrow) || ImGui::IsKeyDown(ImGuiKey_D);
            const ImVec2 md  = ImGui::GetIO().MouseDelta;
            if (inField && (md.x != 0.f || md.y != 0.f)) mouseAim_ = true;
            if (left || right) mouseAim_ = false;
            constexpr float kTurn = 4.6f;   // radians per second
            if (left)  angle_ -= kTurn * dt;
            if (right) angle_ += kTurn * dt;
            if (mouseAim_ && inField) {
                float d = atan2f(mouse.y - ship_.y, mouse.x - ship_.x) - angle_;
                d = remainderf(d, 6.2831853f);
                angle_ += std::clamp(d, -kTurn * dt, kTurn * dt);
            }
            thrust = ImGui::IsKeyDown(ImGuiKey_UpArrow) || ImGui::IsKeyDown(ImGuiKey_W) ||
                     (inField && ImGui::IsMouseDown(ImGuiMouseButton_Right));
            const ImVec2 dir(cosf(angle_), sinf(angle_));
            if (thrust) { shipV_.x += dir.x * Sc(280.f) * dt; shipV_.y += dir.y * Sc(280.f) * dt; }
            const float drag = powf(0.5f, dt), speed = sqrtf(shipV_.x * shipV_.x + shipV_.y * shipV_.y);
            const float cap  = speed > Sc(320.f) ? Sc(320.f) / speed : 1.f;
            shipV_ = ImVec2(shipV_.x * drag * cap, shipV_.y * drag * cap);
            ship_  = ImVec2(ship_.x + shipV_.x * dt, ship_.y + shipV_.y * dt);
            wrap(ship_);

            cooldown_ -= dt;
            const bool fire = ImGui::IsKeyDown(ImGuiKey_Space) || (inField && ImGui::IsMouseDown(ImGuiMouseButton_Left));
            if (fire && cooldown_ <= 0.f && shots_.size() < 5) {
                const ImVec2 nose(ship_.x + dir.x * Sc(11.f), ship_.y + dir.y * Sc(11.f));
                shots_.push_back({ nose, ImVec2(dir.x * Sc(440.f) + shipV_.x, dir.y * Sc(440.f) + shipV_.y), 0.85f });
                cooldown_ = 0.2f;
            }
        }
        invuln_ -= dt;
        if (respawn_ > 0.f && (respawn_ -= dt) <= 0.f) {
            ship_   = ImVec2(size.x * 0.5f, size.y * 0.5f);
            shipV_  = ImVec2(0.f, 0.f);
            invuln_ = 2.f;
        }

        for (Rock& r : rocks_) {
            r.p = ImVec2(r.p.x + r.v.x * dt, r.p.y + r.v.y * dt);
            r.rot += r.spin * dt;
            wrap(r.p);
        }
        for (Shot& sh : shots_) {
            sh.p = ImVec2(sh.p.x + sh.v.x * dt, sh.p.y + sh.v.y * dt);
            sh.life -= dt;
            wrap(sh.p);
        }
        for (Spark& sp : sparks_) {
            sp.p = ImVec2(sp.p.x + sp.v.x * dt, sp.p.y + sp.v.y * dt);
            sp.life -= dt;
        }

        // Shots break rocks: big into two medium, medium into two small.
        for (Shot& sh : shots_) {
            for (size_t i = 0; i < rocks_.size() && sh.life > 0.f; ++i) {
                const Rock r = rocks_[i];
                if (Dist(sh.p, r.p) > r.r) continue;
                sh.life = 0.f;
                rocks_.erase(rocks_.begin() + static_cast<std::ptrdiff_t>(i));
                static constexpr int kPoints[3] = { 20, 50, 100 };
                gameScore_ += kPoints[r.size];
                Burst(r.p, 8, kDim);
                if (r.size < 2)
                    for (int k = 0; k < 2; ++k) rocks_.push_back(MakeRock(r.p, r.size + 1));
            }
        }
        shots_.erase(std::remove_if(shots_.begin(), shots_.end(), [](const Shot& sh) { return sh.life <= 0.f; }),
                     shots_.end());
        sparks_.erase(std::remove_if(sparks_.begin(), sparks_.end(), [](const Spark& sp) { return sp.life <= 0.f; }),
                      sparks_.end());

        // A rock hitting the ship costs a life.
        if (game_ == Game::Playing && respawn_ <= 0.f && invuln_ <= 0.f)
            for (const Rock& r : rocks_)
                if (Dist(ship_, r.p) < r.r * 0.85f + Sc(7.f)) {
                    Burst(ship_, 18, kHot);
                    if (--lives_ <= 0) { game_ = Game::Over; stateTime_ = t; }
                    else                respawn_ = 1.6f;
                    break;
                }

        // Next wave once the field is clear.
        if (game_ == Game::Playing && rocks_.empty()) {
            if (waveTime_ < 0.f) waveTime_ = t;
            if (t - waveTime_ > 1.2f) { ++level_; NewWave(size); waveTime_ = -1.f; }
        }

        if (game_ == Game::Over && t - stateTime_ > 2.5f) {
            if (gameScore_ > s.vectorHighScore) { game_ = Game::Initials; initials_.clear(); }
            else                                game_ = Game::Off;
        }
        if (game_ == Game::Initials) {
            for (int k = 0; k < 26; ++k)
                if (initials_.size() < 3 && ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_A + k), false))
                    initials_ += static_cast<char>('A' + k);
            if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !initials_.empty()) initials_.pop_back();
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                ImGui::IsItemClicked())
                commitInitials_ = true;
            if (commitInitials_) {
                s.vectorHighScore = gameScore_;
                if (!initials_.empty()) s.vectorHighName = initials_;
                game_ = Game::Off;
            }
        }
        commitInitials_ = false;

        // --- Draw ----------------------------------------------------------
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (const Rock& r : rocks_) {
            ImVec2 pts[10];
            for (int k = 0; k < 10; ++k) {
                const float ang = r.rot + k * 0.6283185f;
                const float rr  = r.r * (0.72f + 0.28f * Hash(r.seed + k, 10.f));
                pts[k] = ImVec2(a.x + r.p.x + cosf(ang) * rr, a.y + r.p.y + sinf(ang) * rr);
            }
            Beam(dl, pts, 10, kBeam, Sc(1.1f), true, 0.85f);
        }
        for (const Shot& sh : shots_) {
            const ImVec2 q(a.x + sh.p.x, a.y + sh.p.y);
            Beam(dl, &q, 1, kHot, Sc(1.6f));
        }
        for (const Spark& sp : sparks_)
            dl->AddCircleFilled(ImVec2(a.x + sp.p.x, a.y + sp.p.y), Sc(1.2f), WithAlpha(sp.col, sp.life / 0.8f), 6);
        const bool showShip = (game_ == Game::Playing && respawn_ <= 0.f) &&
                              (invuln_ <= 0.f || fmodf(t * 8.f, 1.f) < 0.6f);
        if (showShip) ShipAt(dl, ImVec2(a.x + ship_.x, a.y + ship_.y), angle_, Sc(9.f), kHot, thrust, t);

        auto centred = [&](float y, float hh, ImU32 col, const char* text) {
            Text(dl, ImVec2(a.x + (size.x - TextWidth(hh, text)) * 0.5f, a.y + y), hh, col, text);
        };
        const float since = t - stateTime_;
        if (game_ == Game::Playing && since < 3.f) {
            centred(size.y * 0.30f, Sc(20.f), kBeam, "PLAYER 1");
            centred(size.y - Sc(64.f), Sc(9.f), kDim, "ARROWS OR WASD: TURN + THRUST - SPACE: FIRE");
            centred(size.y - Sc(46.f), Sc(9.f), kDim, "OR MOUSE: AIM - CLICK: FIRE - RIGHT CLICK: THRUST");
            centred(size.y - Sc(28.f), Sc(9.f), kDim, "ESC: QUIT");
        } else if (game_ == Game::Playing && rocks_.empty()) {
            char buf[16];
            snprintf(buf, sizeof(buf), "WAVE %d", level_ + 2);
            centred(size.y * 0.42f, Sc(20.f), kBeam, buf);
        } else if (game_ == Game::Over) {
            centred(size.y * 0.42f, Sc(28.f), kStop, "GAME OVER");
        } else if (game_ == Game::Initials) {
            char buf[32];
            centred(size.y * 0.22f, Sc(18.f), kGo, "NEW HIGH SCORE");
            snprintf(buf, sizeof(buf), "%06d", gameScore_);
            centred(size.y * 0.22f + Sc(32.f), Sc(14.f), kBeam, buf);
            centred(size.y * 0.48f, Sc(10.f), kDim, "TYPE YOUR INITIALS");
            std::string shown = initials_;
            while (shown.size() < 3) shown += (shown.size() == initials_.size() && fmodf(t * 3.f, 1.f) < 0.5f) ? '-' : ' ';
            centred(size.y * 0.48f + Sc(22.f), Sc(30.f), kHot, shown.c_str());
            centred(size.y * 0.48f + Sc(70.f), Sc(10.f), kDim, "PRESS ENTER OR CLICK");
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

    enum class Game { Off, Playing, Over, Initials };
    struct Rock  { ImVec2 p, v; float r, rot, spin, seed; int size; };   // size 0 big … 2 small
    struct Shot  { ImVec2 p, v; float life; };
    struct Spark { ImVec2 p, v; float life; ImU32 col; };

    static float Dist(ImVec2 p, ImVec2 q) { return sqrtf((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y)); }

    float Rand() {   // 0 to 1
        rng_ = rng_ * 1664525u + 1013904223u;
        return (rng_ >> 8) / 16777216.f;
    }

    void StartGame(float t) {
        game_ = Game::Playing;
        lives_ = 3; level_ = 0; gameScore_ = 0;
        rocks_.clear(); shots_.clear(); sparks_.clear();
        respawn_ = 0.f; cooldown_ = 0.f; waveTime_ = -1.f;
        mouseAim_ = false; needSetup_ = true; stateTime_ = t;
        rng_ ^= static_cast<unsigned>(t * 1000.f);
    }

    Rock MakeRock(ImVec2 p, int sizeClass) {
        static constexpr float kRadius[3] = { 36.f, 20.f, 11.f };
        const float ang   = Rand() * 6.2831853f;
        const float speed = Sc(30.f + 30.f * Rand() + 18.f * sizeClass) * (1.f + 0.12f * level_);
        return { p, ImVec2(cosf(ang) * speed, sinf(ang) * speed), Sc(kRadius[sizeClass]), 0.f,
                 (Rand() - 0.5f) * 2.f, Rand() * 100.f, sizeClass };
    }

    // A wave of big rocks, kept away from the ship.
    void NewWave(ImVec2 size) {
        const int n = std::min(4 + level_, 9);
        for (int i = 0; i < n; ++i) {
            ImVec2 p;
            do p = ImVec2(Rand() * size.x, Rand() * size.y);
            while (Dist(p, ship_) < Sc(130.f) && size.x > Sc(300.f) && size.y > Sc(300.f));
            rocks_.push_back(MakeRock(p, 0));
        }
    }

    void Burst(ImVec2 p, int n, ImU32 col) {
        for (int i = 0; i < n; ++i) {
            const float ang = Rand() * 6.2831853f, speed = Sc(40.f + 90.f * Rand());
            sparks_.push_back({ p, ImVec2(cosf(ang) * speed, sinf(ang) * speed), 0.4f + 0.4f * Rand(), col });
        }
    }

    int score_    = 0;    // the menu's: 150 a page
    int lastPage_ = -1;

    // Asteroids.  Positions are in pixels from the page area's top-left.
    Game               game_ = Game::Off;
    std::vector<Rock>  rocks_;
    std::vector<Shot>  shots_;
    std::vector<Spark> sparks_;
    ImVec2      ship_, shipV_;
    float       angle_ = 0.f, invuln_ = 0.f, respawn_ = 0.f, cooldown_ = 0.f;
    float       stateTime_ = 0.f, waveTime_ = -1.f;
    int         lives_ = 3, level_ = 0, gameScore_ = 0;
    bool        mouseAim_ = false, needSetup_ = false, commitInitials_ = false;
    std::string initials_;
    unsigned    rng_ = 0xa57e401du;
};

} // namespace vecmon

// ---------------------------------------------------------------------------
// Arcade Service layout — a coin-op cabinet's hidden test-mode screen.
//   • Black screen over the monitor-convergence crosshatch, with colour-bar
//     strips top and bottom, in a chunky pixel font.
//   • One numbered list, like an operator menu; the hovered item turns
//     yellow with a blinking arrow, the active choice is marked ON.
//   • Pages print in line by line; the footer blinks INSERT COIN.
//   • Insert a coin (click it, or press 5) and push start (click, or 1) to
//     play Breakout against a wall of the test colour bars.
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

    // A small-print line along the bottom, like an arcade copyright notice.
    bool GitHubButton(const char* id, ImVec2 a, ImVec2 b) override {
        const char*  text = "GITHUB.COM/LANECWHEELER/SEERT-OVERLAY";
        const float  fs = Sc(8.f), ms = Sc(10.f), gap = Sc(6.f);
        const float  w  = ms + gap + TextW(fs, text);
        const ImVec2 p((a.x + b.x - w) * 0.5f, a.y + Sc(44.f));
        ImGui::SetCursorScreenPos(ImVec2(p.x - Sc(4.f), p.y - Sc(3.f)));
        const bool  clicked = ImGui::InvisibleButton(id, ImVec2(w + Sc(8.f), ms + Sc(6.f)));
        const bool  hovered = ImGui::IsItemHovered();
        ImDrawList* dl      = ImGui::GetWindowDrawList();
        const ImU32 col     = hovered ? kYellow : kCyan;
        GitHubMark(dl, ImVec2(p.x + ms * 0.5f, p.y + ms * 0.5f), ms, col);
        PixelText(dl, fs, ImVec2(p.x + ms + gap, p.y + (ms - fs) * 0.5f), col, text);
        if (hovered)
            dl->AddRectFilled(ImVec2(p.x + ms + gap, p.y + ms + Sc(1.f)), ImVec2(p.x + w, p.y + ms + Sc(2.f)), col);
        return clicked;
    }

    // MAME's keys: 5 inserts a coin, 1 is player one's start.
    void Update(CRTSettings&, float t) override {
        if (ImGui::GetIO().WantTextInput) return;
        if (ImGui::IsKeyPressed(ImGuiKey_5, false)) AddCoin();
        if (ImGui::IsKeyPressed(ImGuiKey_1, false)) PushStart(t);
    }

    // INSERT COIN (or PUSH START once there's a credit) and the credit count
    // in the middle of the footer; both take coins.
    void DrawExtras(ImVec2 a, ImVec2 b, float footerY, CRTSettings&, float t) override {
        ImDrawList*  dl       = ImGui::GetWindowDrawList();
        const float  cx       = (a.x + b.x) * 0.5f;
        const bool   canStart = credits_ > 0 && game_ == Game::Off;
        const char*  top      = canStart ? "PUSH START" : "INSERT COIN";
        const float  fs       = Sc(16.f);
        const float  w        = TextW(fs, top) + Sc(8.f);
        ImGui::SetCursorScreenPos(ImVec2(cx - w * 0.5f, footerY - Sc(2.f)));
        if (ImGui::InvisibleButton("##coin", ImVec2(w, Sc(22.f)))) {
            if (canStart) PushStart(t);
            else          AddCoin();
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered || Blink(t, 1.f, 0.6f))
            PixelText(dl, fs, ImVec2(cx - TextW(fs, top) * 0.5f, footerY + Sc(1.f)), hovered ? kWhite : kYellow, top);

        char buf[16];
        snprintf(buf, sizeof(buf), "CREDIT %02d", credits_);
        const float ss = Sc(8.f), cw = TextW(ss, buf) + Sc(8.f);
        ImGui::SetCursorScreenPos(ImVec2(cx - cw * 0.5f, footerY + Sc(24.f)));
        if (ImGui::InvisibleButton("##credit", ImVec2(cw, Sc(14.f)))) AddCoin();
        PixelText(dl, ss, ImVec2(cx - TextW(ss, buf) * 0.5f, footerY + Sc(27.f)),
                  ImGui::IsItemHovered() ? kYellow : kWhite, buf);
    }

    bool Takeover() const override { return game_ != Game::Off; }
    void EndTakeover(float) override { game_ = Game::Off; }

    // Easter egg: Breakout, in the page area, against the colour bars.
    void DrawTakeover(ImVec2 a, ImVec2 b, CRTSettings& s, float t) override {
        const ImVec2 size(b.x - a.x, b.y - a.y);
        const float  dt = std::min(ImGui::GetIO().DeltaTime, 0.05f);
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##field", size);
        const bool inField = ImGui::IsItemHovered() || ImGui::IsItemActive();

        // Layout, from the field size.
        const float margin = Sc(4.f), gap = Sc(3.f), top = Sc(40.f), bh = Sc(14.f);
        const float bw     = (size.x - margin * 2.f - gap * (kBrickCols - 1)) / kBrickCols;
        const float pw     = Sc(84.f), ph = Sc(10.f), py = size.y - Sc(28.f);
        const float ball   = Sc(8.f);
        auto brickRect = [&](int row, int col, ImVec2& p0, ImVec2& p1) {
            p0 = ImVec2(margin + col * (bw + gap), top + row * (bh + gap));
            p1 = ImVec2(p0.x + bw, p0.y + bh);
        };
        if (needSetup_) { paddleX_ = size.x * 0.5f; needSetup_ = false; }

        // --- Step ----------------------------------------------------------
        const ImVec2 md = ImGui::GetIO().MouseDelta;
        if (inField && md.x != 0.f) paddleX_ = ImGui::GetIO().MousePos.x - a.x;
        if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)  || ImGui::IsKeyDown(ImGuiKey_A)) paddleX_ -= Sc(560.f) * dt;
        if (ImGui::IsKeyDown(ImGuiKey_RightArrow) || ImGui::IsKeyDown(ImGuiKey_D)) paddleX_ += Sc(560.f) * dt;
        paddleX_ = std::clamp(paddleX_, pw * 0.5f, size.x - pw * 0.5f);

        if (game_ == Game::Serve) {
            ball_ = ImVec2(paddleX_ - ball * 0.5f, py - ball - Sc(1.f));
            const bool launch = ImGui::IsKeyPressed(ImGuiKey_Space, false) || (inField && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
            if (launch && t - stateTime_ > 0.3f) {
                const float ang = (Rand() - 0.5f) * 1.2f;   // up, within about 35 degrees of vertical
                ballV_ = ImVec2(sinf(ang) * speed_, -cosf(ang) * speed_);
                game_  = Game::Playing;
            }
        } else if (game_ == Game::Playing) {
            constexpr int kSteps = 4;   // small steps, so the ball can't skip through a brick
            for (int step = 0; step < kSteps && game_ == Game::Playing; ++step) {
                const float h = dt / kSteps;
                ball_ = ImVec2(ball_.x + ballV_.x * h, ball_.y + ballV_.y * h);
                if (ball_.x < 0.f)                { ball_.x = 0.f;                ballV_.x =  fabsf(ballV_.x); }
                if (ball_.x + ball > size.x)      { ball_.x = size.x - ball;      ballV_.x = -fabsf(ballV_.x); }
                if (ball_.y < Sc(24.f))           { ball_.y = Sc(24.f);           ballV_.y =  fabsf(ballV_.y); }

                // Paddle: where it lands sets the angle, like the original.
                if (ballV_.y > 0.f && ball_.y + ball >= py && ball_.y + ball <= py + ph + ball &&
                    ball_.x + ball >= paddleX_ - pw * 0.5f && ball_.x <= paddleX_ + pw * 0.5f) {
                    const float hit = std::clamp((ball_.x + ball * 0.5f - paddleX_) / (pw * 0.5f), -1.f, 1.f);
                    const float ang = hit * 1.05f;   // up to 60 degrees off vertical
                    ballV_ = ImVec2(sinf(ang) * speed_, -cosf(ang) * speed_);
                    ball_.y = py - ball;
                }

                // Bricks: knock out the first one touched and bounce off its nearer side.
                for (int i = 0; i < kBrickRows * kBrickCols; ++i) {
                    if (!bricks_[i]) continue;
                    ImVec2 p0, p1;
                    brickRect(i / kBrickCols, i % kBrickCols, p0, p1);
                    if (ball_.x + ball < p0.x || ball_.x > p1.x || ball_.y + ball < p0.y || ball_.y > p1.y) continue;
                    bricks_[i] = false;
                    score_ += kRowPoints[i / kBrickCols];
                    speed_  = std::min(speed_ + Sc(5.f), Sc(560.f));
                    const float ox = std::min(ball_.x + ball - p0.x, p1.x - ball_.x);
                    const float oy = std::min(ball_.y + ball - p0.y, p1.y - ball_.y);
                    if (ox < oy) ballV_.x = -ballV_.x;
                    else         ballV_.y = -ballV_.y;
                    const float len = sqrtf(ballV_.x * ballV_.x + ballV_.y * ballV_.y);
                    ballV_ = ImVec2(ballV_.x / len * speed_, ballV_.y / len * speed_);
                    break;
                }

                if (ball_.y > size.y) {
                    if (--lives_ <= 0) { game_ = Game::Over; stateTime_ = t; }
                    else               { game_ = Game::Serve; stateTime_ = t; }
                }
            }
            if (std::none_of(bricks_, bricks_ + kBrickRows * kBrickCols, [](bool alive) { return alive; })) {
                game_ = Game::Clear;
                stateTime_ = t;
            }
        } else if (game_ == Game::Clear && t - stateTime_ > 2.f) {
            ++round_;
            NewWall();
            game_ = Game::Serve;
            stateTime_ = t;
        } else if (game_ == Game::Over && t - stateTime_ > 2.5f) {
            game_ = Game::Off;
        }
        if (score_ > s.arcadeHighScore) s.arcadeHighScore = score_;

        // --- Draw ----------------------------------------------------------
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 200));   // dim the crosshatch behind the game
        char buf[64];
        snprintf(buf, sizeof(buf), "1UP %06d   HI %06d   BALL %d", score_, s.arcadeHighScore,
                 std::max(lives_, 0));
        PixelText(dl, Sc(12.f), ImVec2(a.x + (size.x - TextW(Sc(12.f), buf)) * 0.5f, a.y + Sc(4.f)), kWhite, buf);
        for (int i = 0; i < kBrickRows * kBrickCols; ++i) {
            if (!bricks_[i]) continue;
            ImVec2 p0, p1;
            brickRect(i / kBrickCols, i % kBrickCols, p0, p1);
            const ImU32 col = kRowColours[i / kBrickCols];
            dl->AddRectFilled(ImVec2(a.x + p0.x, a.y + p0.y), ImVec2(a.x + p1.x, a.y + p1.y), col);
            dl->AddRectFilled(ImVec2(a.x + p0.x, a.y + p1.y - Sc(3.f)), ImVec2(a.x + p1.x, a.y + p1.y), Shade(col, 0.55f));
        }
        dl->AddRectFilled(ImVec2(a.x + paddleX_ - pw * 0.5f, a.y + py), ImVec2(a.x + paddleX_ + pw * 0.5f, a.y + py + ph), kWhite);
        if (game_ == Game::Serve || game_ == Game::Playing)
            dl->AddRectFilled(ImVec2(a.x + ball_.x, a.y + ball_.y), ImVec2(a.x + ball_.x + ball, a.y + ball_.y + ball), kWhite);

        auto centred = [&](float y, float fs, ImU32 col, const char* text) {
            PixelText(dl, fs, ImVec2(a.x + (size.x - TextW(fs, text)) * 0.5f, a.y + y), col, text);
        };
        const float msgY = top + kBrickRows * (bh + gap) + Sc(40.f);
        if (game_ == Game::Serve && Blink(t, 0.8f, 0.7f)) {
            centred(msgY, Sc(16.f), kYellow, round_ > 0 || lives_ < 3 ? "READY" : "PLAYER 1");
            centred(msgY + Sc(30.f), Sc(8.f), kCyan, "MOUSE OR ARROWS MOVE - CLICK OR SPACE SERVES - ESC QUITS");
        } else if (game_ == Game::Clear) {
            centred(msgY, Sc(16.f), kGreen, "ROUND CLEAR");
        } else if (game_ == Game::Over) {
            centred(msgY, Sc(20.f), kRed, "GAME OVER");
        }
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

private:
    enum class Game { Off, Serve, Playing, Clear, Over };
    static constexpr int   kBrickRows = 7, kBrickCols = 10;
    static constexpr ImU32 kRowColours[kBrickRows] = { kRed, kMagenta, kYellow, kGreen, kCyan, kBlue, kWhite };
    static constexpr int   kRowPoints[kBrickRows]  = { 70, 60, 50, 40, 30, 20, 10 };

    float Rand() {   // 0 to 1
        rng_ = rng_ * 1664525u + 1013904223u;
        return (rng_ >> 8) / 16777216.f;
    }
    void AddCoin() { credits_ = std::min(credits_ + 1, 99); }
    void PushStart(float t) {
        if (credits_ <= 0 || game_ != Game::Off) return;
        --credits_;
        game_ = Game::Serve;
        lives_ = 3; score_ = 0; round_ = 0;
        speed_ = Sc(300.f);
        needSetup_ = true;
        stateTime_ = t;
        rng_ ^= static_cast<unsigned>(t * 1000.f);
        NewWall();
    }
    void NewWall() {
        std::fill(bricks_, bricks_ + kBrickRows * kBrickCols, true);
        speed_ = Sc(300.f) + Sc(30.f) * round_;
    }

    // Breakout.  Positions are in pixels from the page area's top-left.
    Game     game_ = Game::Off;
    int      credits_ = 0, lives_ = 3, score_ = 0, round_ = 0;
    bool     bricks_[kBrickRows * kBrickCols] = {};
    ImVec2   ball_, ballV_;
    float    paddleX_ = 0.f, speed_ = 300.f, stateTime_ = 0.f;
    bool     needSetup_ = false;
    unsigned rng_ = 0xb2ea4c07u;
};

} // namespace arcade

// ---------------------------------------------------------------------------
// Computer Lab layout — a 90s classroom drawing program.
//   • A sheet of notebook paper: blue ruled lines, a red margin, punched
//     holes and a strip of tape, with the title written in crayon — every
//     letter a different colour, a little wonky.
//   • Main page: every settings group as a construction-paper card; hovering
//     one lifts it and points a pencil at it, the current choice gets a gold
//     star sticker.
//   • The sheet slides up onto the desk and pages slide in from the side.
//   • The box of crayons in the corner opens a drawing canvas: crayons and a
//     rainbow crayon, lines, rectangles and circles (outline or filled), an
//     eraser and a star stamp.
// ---------------------------------------------------------------------------
namespace complab {

constexpr ImU32 kPaper  = IM_COL32(252, 250, 238, 255);
constexpr ImU32 kRule   = IM_COL32(150, 190, 235, 255);
constexpr ImU32 kMargin = IM_COL32(235,  95,  95, 255);
constexpr ImU32 kInk    = IM_COL32( 46,  46,  62, 255);   // pencil / marker
constexpr ImU32 kWhite  = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kGold   = IM_COL32(255, 200,  40, 255);
constexpr ImU32 kGoldLo = IM_COL32(214, 140,  20, 255);

// The crayon box, in order: red, orange, yellow, green, blue, purple, pink,
// brown, black.
constexpr ImU32 kCrayons[] = {
    IM_COL32(228,  52,  52, 255), IM_COL32(245, 140,  32, 255), IM_COL32(250, 208,  40, 255),
    IM_COL32( 62, 170,  74, 255), IM_COL32( 52, 110, 220, 255), IM_COL32(140,  82, 200, 255),
    IM_COL32(240, 112, 170, 255), IM_COL32(150,  96,  52, 255), IM_COL32( 44,  44,  52, 255),
};
constexpr int kCrayonCount = static_cast<int>(sizeof(kCrayons) / sizeof(kCrayons[0]));

ImFont* Font() { return StyleFonts::shortStack ? StyleFonts::shortStack : ImGui::GetFont(); }

ImVec2 TextSize(float size, const char* text) { return Font()->CalcTextSizeA(size, FLT_MAX, 0.f, text); }

// Ink for text on a crayon colour: white on the dark ones.
ImU32 LabelOn(ImU32 col) {
    const float r = ((col >> IM_COL32_R_SHIFT) & 0xFF) / 255.f, g = ((col >> IM_COL32_G_SHIFT) & 0xFF) / 255.f,
                b = ((col >> IM_COL32_B_SHIFT) & 0xFF) / 255.f;
    return 0.299f * r + 0.587f * g + 0.114f * b > 0.62f ? kInk : kWhite;
}

// Rotates the vertices added to dl since vtxStart by ang radians around c.
void RotateVerts(ImDrawList* dl, int vtxStart, ImVec2 c, float ang) {
    const float ca = cosf(ang), sa = sinf(ang);
    for (int i = vtxStart; i < dl->VtxBuffer.Size; ++i) {
        ImVec2&     p = dl->VtxBuffer[i].pos;
        const float x = p.x - c.x, y = p.y - c.y;
        p = ImVec2(c.x + x * ca - y * sa, c.y + x * sa + y * ca);
    }
}

// Text in crayon: a waxy second pass, slightly offset and lighter.
void CrayonText(ImDrawList* dl, float size, ImVec2 pos, ImU32 col, const char* text) {
    dl->AddText(Font(), size, pos, col, text);
    dl->AddText(Font(), size, ImVec2(pos.x + size * 0.03f, pos.y + size * 0.02f),
                WithAlpha(Mix(col, kWhite, 0.35f), 0.45f), text);
}

// A sketchy pencil outline: the shape traced twice, the second time a touch off.
void Sketch(ImDrawList* dl, const ImVec2* pts, int n, ImU32 col, float w) {
    dl->AddPolyline(pts, n, WithAlpha(col, 0.9f), ImDrawFlags_Closed, w);
    std::vector<ImVec2> off(pts, pts + n);
    for (int i = 0; i < n; ++i) off[i] = ImVec2(off[i].x + Sc(0.8f) * ((i & 1) ? 1.f : -0.6f), off[i].y + Sc(0.7f));
    dl->AddPolyline(off.data(), n, WithAlpha(col, 0.35f), ImDrawFlags_Closed, w * 0.7f);
}

// A five-point gold star sticker centred on c, turned by rot.
void StarSticker(ImDrawList* dl, ImVec2 c, float r, float rot, ImU32 fill = kGold) {
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        const float rr = (i % 2 == 0) ? r : r * 0.46f;
        const float a  = rot + i * 0.6283185f - 1.5707963f;
        pts[i] = ImVec2(c.x + cosf(a) * rr, c.y + sinf(a) * rr);
    }
    for (int i = 0; i < 10; ++i)
        dl->AddTriangleFilled(ImVec2(c.x + Sc(1.f), c.y + Sc(1.5f)), ImVec2(pts[i].x + Sc(1.f), pts[i].y + Sc(1.5f)),
                              ImVec2(pts[(i + 1) % 10].x + Sc(1.f), pts[(i + 1) % 10].y + Sc(1.5f)), IM_COL32(0, 0, 0, 40));
    for (int i = 0; i < 10; ++i) dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], fill);
    dl->AddPolyline(pts, 10, Shade(fill, 0.75f), ImDrawFlags_Closed, Sc(1.2f));
    dl->AddCircleFilled(ImVec2(c.x - r * 0.15f, c.y - r * 0.2f), r * 0.16f, IM_COL32(255, 255, 255, 120));
}

// A yellow pencil pointing right with its tip at `tip`, about len long.
void Pencil(ImDrawList* dl, ImVec2 tip, float len) {
    const float h = len * 0.17f, hh = h * 0.5f;
    const float cone = tip.x - len * 0.22f, ferrule = tip.x - len * 0.84f, end = tip.x - len;
    // Shadow, wood cone, graphite point.
    dl->AddRectFilled(ImVec2(end + Sc(2.f), tip.y - hh + Sc(3.f)), ImVec2(cone + Sc(2.f), tip.y + hh + Sc(3.f)),
                      IM_COL32(0, 0, 0, 45), Sc(2.f));
    dl->AddTriangleFilled(tip, ImVec2(cone, tip.y - hh), ImVec2(cone, tip.y + hh), IM_COL32(236, 196, 140, 255));
    const ImVec2 lead(tip.x - len * 0.08f, 0.f);
    dl->AddTriangleFilled(tip, ImVec2(lead.x, tip.y - hh * 0.36f), ImVec2(lead.x, tip.y + hh * 0.36f), kInk);
    // Body: three faces of a hexagonal pencil.
    dl->AddRectFilled(ImVec2(ferrule, tip.y - hh), ImVec2(cone, tip.y + hh), IM_COL32(250, 200, 40, 255));
    dl->AddRectFilled(ImVec2(ferrule, tip.y - hh), ImVec2(cone, tip.y - hh * 0.35f), IM_COL32(255, 222, 96, 255));
    dl->AddRectFilled(ImVec2(ferrule, tip.y + hh * 0.35f), ImVec2(cone, tip.y + hh), IM_COL32(222, 168, 24, 255));
    // Metal ferrule and pink eraser.
    dl->AddRectFilled(ImVec2(end + len * 0.07f, tip.y - hh), ImVec2(ferrule, tip.y + hh), IM_COL32(190, 192, 200, 255));
    for (int i = 1; i < 3; ++i) {
        const float x = end + len * 0.07f + (ferrule - end - len * 0.07f) * i / 3.f;
        dl->AddLine(ImVec2(x, tip.y - hh), ImVec2(x, tip.y + hh), IM_COL32(120, 122, 132, 255), Sc(1.f));
    }
    dl->AddRectFilled(ImVec2(end, tip.y - hh), ImVec2(end + len * 0.08f, tip.y + hh), IM_COL32(240, 128, 150, 255),
                      Sc(3.f), ImDrawFlags_RoundCornersLeft);
    dl->AddRect(ImVec2(end, tip.y - hh), ImVec2(cone, tip.y + hh), WithAlpha(kInk, 0.6f), Sc(2.f), 0, Sc(1.f));
}

// A crayon standing up, its point at the top: body from y0 to y1 at x.
void Crayon(ImDrawList* dl, float x, float y0, float y1, float w, ImU32 col, bool rainbow = false) {
    const float tip = w * 1.1f;
    if (rainbow) {
        const float band = (y1 - y0 - tip) / 6.f;
        for (int i = 0; i < 6; ++i)
            dl->AddRectFilled(ImVec2(x - w * 0.5f, y0 + tip + band * i), ImVec2(x + w * 0.5f, y0 + tip + band * (i + 1)),
                              kCrayons[i]);
        col = kCrayons[0];
    } else {
        dl->AddRectFilled(ImVec2(x - w * 0.5f, y0 + tip), ImVec2(x + w * 0.5f, y1), col, Sc(2.f), ImDrawFlags_RoundCornersBottom);
    }
    dl->AddTriangleFilled(ImVec2(x, y0), ImVec2(x + w * 0.5f, y0 + tip), ImVec2(x - w * 0.5f, y0 + tip), col);
    // Paper wrapper with two stripes.
    const ImU32 wrap = rainbow ? IM_COL32(255, 255, 255, 90) : Shade(col, 0.78f);
    dl->AddRectFilled(ImVec2(x - w * 0.5f, y0 + tip + (y1 - y0) * 0.22f), ImVec2(x + w * 0.5f, y0 + tip + (y1 - y0) * 0.27f), wrap);
    dl->AddRectFilled(ImVec2(x - w * 0.5f, y1 - (y1 - y0) * 0.22f), ImVec2(x + w * 0.5f, y1 - (y1 - y0) * 0.17f), wrap);
    dl->AddLine(ImVec2(x - w * 0.18f, y0 + tip + Sc(2.f)), ImVec2(x - w * 0.18f, y1 - Sc(3.f)), IM_COL32(255, 255, 255, 70), Sc(1.5f));
}

// A construction-paper card, tilted a little.  Hovering lifts it, pressing
// presses it down; `selected` adds a gold star sticker.  True when clicked.
bool Card(const char* id, const char* label, ImVec2 size, ImU32 col, float tilt, float fontSize, bool selected) {
    const ImVec2 pos     = ImGui::GetCursorScreenPos();
    const bool   clicked = ImGui::InvisibleButton(id, size);
    const bool   hovered = ImGui::IsItemHovered();
    const bool   held    = ImGui::IsItemActive();
    const float  t       = static_cast<float>(ImGui::GetTime());

    const float  scale = held ? 0.97f : hovered ? 1.04f : 1.f;
    const float  ang   = hovered ? tilt * 0.3f + 0.012f * sinf(t * 6.f) : tilt;
    const ImVec2 c(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    const ImVec2 half(size.x * 0.5f * scale, size.y * 0.5f * scale);
    ImDrawList*  dl = ImGui::GetWindowDrawList();

    const int v0 = dl->VtxBuffer.Size;
    const float lift = hovered && !held ? Sc(6.f) : Sc(3.f);
    dl->AddRectFilled(ImVec2(c.x - half.x + lift * 0.5f, c.y - half.y + lift), ImVec2(c.x + half.x + lift * 0.5f, c.y + half.y + lift),
                      IM_COL32(0, 0, 0, hovered ? 50 : 35), Sc(3.f));
    dl->AddRectFilled(ImVec2(c.x - half.x, c.y - half.y), ImVec2(c.x + half.x, c.y + half.y), hovered ? Shade(col, 1.08f) : col, Sc(3.f));
    // Paper grain: a few faint lighter flecks.
    for (int i = 0; i < 6; ++i) {
        const float fx = Hash(static_cast<float>(i), size.x) * 2.f - 1.f, fy = Hash(size.y, static_cast<float>(i)) * 2.f - 1.f;
        dl->AddCircleFilled(ImVec2(c.x + fx * half.x * 0.85f, c.y + fy * half.y * 0.7f), Sc(1.2f), IM_COL32(255, 255, 255, 40), 6);
    }
    const ImVec2 box[4] = { ImVec2(c.x - half.x, c.y - half.y), ImVec2(c.x + half.x, c.y - half.y),
                            ImVec2(c.x + half.x, c.y + half.y), ImVec2(c.x - half.x, c.y + half.y) };
    Sketch(dl, box, 4, kInk, Sc(selected ? 2.4f : 1.5f));
    const float  fs = fontSize * scale;
    const ImVec2 ts = TextSize(fs, label);
    dl->AddText(Font(), fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.52f), LabelOn(col), label);
    RotateVerts(dl, v0, c, ang);

    if (selected) StarSticker(dl, ImVec2(c.x + half.x - Sc(4.f), c.y - half.y + Sc(2.f)), Sc(12.f), 0.3f);
    if (hovered)  Pencil(dl, ImVec2(c.x - half.x - Sc(4.f) - Sc(3.f) * fabsf(sinf(t * 7.f)), c.y), Sc(64.f));
    return clicked;
}

// A paper tab for the footer: label on a crayon-coloured card.  True when clicked.
bool Tab(const char* id, const char* label, ImVec2 size, ImU32 col) {
    const ImVec2 pos     = ImGui::GetCursorScreenPos();
    const bool   clicked = ImGui::InvisibleButton(id, size);
    const bool   hovered = ImGui::IsItemHovered();
    const bool   held    = ImGui::IsItemActive();
    ImDrawList*  dl      = ImGui::GetWindowDrawList();
    const float  up      = held ? 0.f : hovered ? Sc(-3.f) : 0.f;
    const ImVec2 a(pos.x, pos.y + up), b(pos.x + size.x, pos.y + size.y + up);
    dl->AddRectFilled(ImVec2(a.x + Sc(2.f), a.y + Sc(4.f) - up), ImVec2(b.x + Sc(2.f), b.y + Sc(4.f) - up), IM_COL32(0, 0, 0, 40), Sc(8.f));
    dl->AddRectFilled(a, b, hovered ? Shade(col, 1.1f) : col, Sc(8.f));
    dl->AddRect(a, b, WithAlpha(kInk, 0.85f), Sc(8.f), 0, Sc(1.5f));
    const float  fs = Sc(19.f);
    const ImVec2 ts = TextSize(fs, label);
    dl->AddText(Font(), fs, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), LabelOn(col), label);
    return clicked;
}

// The Computer Lab look for the shared paged menu.
class Style : public PagedMenuStyle {
public:
    ImVec2  PanelSize()   const override { return ImVec2(700.f, 700.f); }
    ImVec2  Padding()     const override { return ImVec2(72.f, 26.f); }   // content starts right of the margin
    float   TitleSpace()  const override { return 76.f; }
    float   FooterSpace() const override { return 64.f; }
    float   FooterRow()   const override { return 60.f; }
    float   Gutter()      const override { return 46.f; }   // room for the pencil
    float   ColumnGap()   const override { return 48.f; }
    float   GroupButtonHeight() const override { return 46.f; }
    float   ItemButtonHeight()  const override { return 40.f; }
    ImFont* Font()        const override { return complab::Font(); }
    float   FontSize()    const override { return 18.f; }
    std::string PageTitle(int pageIndex) const override {
        return pageIndex < 0 ? "Settings" : TitleCase(kSectionNames[pageIndex]);
    }

    // The sheet slides up onto the desk; pages slide in from the right.
    ImVec2 OpenOffset(float t) const override { return ImVec2(0.f, (1.f - EaseOutBack(t / 0.45f)) * Sc(120.f)); }
    ImVec2 PageOffset(float t) const override { return ImVec2((1.f - EaseOutBack(t / 0.3f)) * Sc(36.f), 0.f); }

    void DrawPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, int, float) override {
        dl->AddRectFilled(ImVec2(a.x + Sc(8.f), a.y + Sc(11.f)), ImVec2(b.x + Sc(8.f), b.y + Sc(11.f)), IM_COL32(0, 0, 0, 80), Sc(4.f));
        dl->AddRectFilled(a, b, kPaper, Sc(4.f));
        // Ruled lines below the title, and the red margin.
        for (float y = a.y + Sc(104.f); y < b.y - Sc(12.f); y += Sc(30.f))
            dl->AddLine(ImVec2(a.x + Sc(2.f), y), ImVec2(b.x - Sc(2.f), y), kRule, Sc(1.2f));
        dl->AddLine(ImVec2(a.x + Sc(56.f), a.y), ImVec2(a.x + Sc(56.f), b.y), kMargin, Sc(1.6f));
        // Punched holes.
        for (float f : { 0.18f, 0.5f, 0.82f }) {
            const ImVec2 h(a.x + Sc(26.f), a.y + (b.y - a.y) * f);
            dl->AddCircleFilled(h, Sc(10.f), IM_COL32(40, 40, 48, 235), 24);
            dl->AddCircle(h, Sc(10.f), IM_COL32(200, 196, 180, 255), 24, Sc(1.5f));
        }
        dl->AddRect(a, b, IM_COL32(200, 196, 180, 255), Sc(4.f), 0, Sc(1.f));
        // A strip of tape across the top-left corner.
        const int v0 = dl->VtxBuffer.Size;
        const ImVec2 tc(a.x + Sc(12.f), a.y + Sc(12.f));
        const ImVec2 t0(tc.x - Sc(42.f), tc.y - Sc(12.f)), t1(tc.x + Sc(42.f), tc.y + Sc(12.f));
        dl->AddRectFilled(t0, t1, IM_COL32(226, 230, 236, 175));
        dl->AddLine(ImVec2(t0.x, t0.y), ImVec2(t1.x, t0.y), IM_COL32(255, 255, 255, 120), Sc(1.f));
        dl->AddRect(t0, t1, IM_COL32(150, 156, 166, 90), 0.f, 0, Sc(1.f));
        RotateVerts(dl, v0, tc, -0.75f);
    }

    // The title in crayon: each letter its own colour, tilted and bobbing a
    // touch, with a wavy underline.
    void DrawTitle(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* title, int, float t) override {
        const float size = Sc(46.f);
        float width = 0.f;
        for (int i = 0; title[i]; ++i) {
            const char ch[2] = { title[i], 0 };
            width += TextSize(size, ch).x;
        }
        float x = a.x + (b.x - a.x - width) * 0.5f;
        const float y = a.y + Sc(18.f);
        for (int i = 0; title[i]; ++i) {
            const char   ch[2] = { title[i], 0 };
            const ImVec2 ts    = TextSize(size, ch);
            const float  fi    = static_cast<float>(i);
            const ImVec2 p(x, y + Sc(3.f) * (Hash(fi, 1.f) - 0.5f) * 2.f + Sc(1.5f) * sinf(t * 2.5f + fi * 0.8f));
            const int v0 = dl->VtxBuffer.Size;
            CrayonText(dl, size, p, kCrayons[i % 6], ch);
            RotateVerts(dl, v0, ImVec2(p.x + ts.x * 0.5f, p.y + ts.y * 0.5f), (Hash(fi, 2.f) - 0.5f) * 0.22f);
            x += ts.x;
        }
        ImVec2 wave[24];
        const float x0 = a.x + (b.x - a.x - width) * 0.5f;
        for (int i = 0; i < 24; ++i)
            wave[i] = ImVec2(x0 + width * i / 23.f, a.y + Sc(78.f) + Sc(2.5f) * sinf(i * 1.3f));
        dl->AddPolyline(wave, 24, WithAlpha(kCrayons[4], 0.8f), 0, Sc(2.5f));
    }

    bool GroupButton(const char* id, const char* label, ImVec2 size, int index, bool selected, Kind kind) override {
        static constexpr int kCardColours[6] = { 0, 1, 2, 3, 4, 5 };
        const ImU32 col  = kCrayons[kCardColours[(index + index / 2) % 6]];
        const float tilt = (Hash(static_cast<float>(index), 3.f) - 0.5f) * 0.05f;
        return Card(id, label, size, col, tilt, Sc(kind == Kind::Group ? 20.f : 18.f), selected);
    }
    bool  BackButton(const char* id) override { return Tab(id, "< Back", FooterButton(), kCrayons[3]); }
    float QuitWidth() const override { return FooterButton().x; }
    bool  QuitButton(const char* id) override { return Tab(id, "Quit", FooterButton(), kCrayons[0]); }

    // A round paper button with a red crayon X.
    ImVec2 CloseCenter(ImVec2 a, ImVec2 b) const override { return ImVec2(b.x - Sc(40.f), a.y + Sc(40.f)); }
    bool   CloseButton(const char* id, ImVec2 c) override {
        const float r = Sc(18.f);
        ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
        const bool  clicked = ImGui::InvisibleButton(id, ImVec2(r * 2.f, r * 2.f));
        const bool  hovered = ImGui::IsItemHovered();
        ImDrawList* dl      = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(ImVec2(c.x + Sc(2.f), c.y + Sc(3.f)), r, IM_COL32(0, 0, 0, 40), 28);
        dl->AddCircleFilled(c, r, hovered ? kCrayons[0] : kWhite, 28);
        dl->AddCircle(c, r, WithAlpha(kInk, 0.8f), 28, Sc(1.5f));
        const ImU32 x = hovered ? kWhite : kCrayons[0];
        const float k = r * 0.45f;
        dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k + Sc(1.f)), x, Sc(3.5f));
        dl->AddLine(ImVec2(c.x + k, c.y - k - Sc(1.f)), ImVec2(c.x - k + Sc(1.f), c.y + k), x, Sc(3.5f));
        return clicked;
    }

    // A name-tag sticker in the middle of the footer.
    bool GitHubButton(const char* id, ImVec2 a, ImVec2 b) override {
        const ImVec2 size(Sc(130.f), Sc(38.f));
        const ImVec2 p0((a.x + b.x - size.x) * 0.5f, a.y);
        ImGui::SetCursorScreenPos(p0);
        const bool  clicked = ImGui::InvisibleButton(id, size);
        const bool  hovered = ImGui::IsItemHovered();
        ImDrawList* dl      = ImGui::GetWindowDrawList();
        const float t       = static_cast<float>(ImGui::GetTime());
        const ImVec2 c(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);
        const int v0 = dl->VtxBuffer.Size;
        dl->AddRectFilled(ImVec2(p0.x + Sc(2.f), p0.y + Sc(3.f)), ImVec2(p0.x + size.x + Sc(2.f), p0.y + size.y + Sc(3.f)),
                          IM_COL32(0, 0, 0, 40), Sc(10.f));
        dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), kWhite, Sc(10.f));
        dl->AddRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), kCrayons[4], Sc(10.f), 0, Sc(2.f));
        const float  ms = Sc(20.f), gap = Sc(8.f), fs = Sc(19.f);
        const ImVec2 ts = TextSize(fs, "GitHub");
        const float  x0 = c.x - (ms + gap + ts.x) * 0.5f;
        GitHubMark(dl, ImVec2(x0 + ms * 0.5f, c.y), ms, kInk);
        dl->AddText(Font(), fs, ImVec2(x0 + ms + gap, c.y - ts.y * 0.52f), hovered ? kCrayons[4] : kInk, "GitHub");
        RotateVerts(dl, v0, c, hovered ? 0.04f * sinf(t * 8.f) : -0.03f);
        return clicked;
    }

    // Easter egg: the box of crayons in the top-left corner opens a canvas.
    void DrawExtras(ImVec2 a, ImVec2, float, CRTSettings&, float t) override {
        const ImVec2 p(a.x + Sc(80.f), a.y + Sc(22.f));   // the box's top-left
        const ImVec2 size(Sc(46.f), Sc(50.f));
        ImGui::SetCursorScreenPos(p);
        if (ImGui::InvisibleButton("##crayons", size)) painting_ = true;
        const bool hovered = ImGui::IsItemHovered();
        if (!painting_) Tip("Art time! Draw something.");

        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Crayons poking out (they jiggle when hovered), then the box in front.
        for (int i = 0; i < 4; ++i) {
            const float x   = p.x + Sc(9.f) + i * Sc(9.5f);
            const float pop = hovered ? Sc(4.f) * (0.5f + 0.5f * sinf(t * 10.f + i * 1.7f)) : 0.f;
            Crayon(dl, x, p.y + Sc(2.f) + (i % 2) * Sc(4.f) - pop, p.y + Sc(30.f), Sc(8.f), kCrayons[(i * 2 + 1) % 6]);
        }
        const ImVec2 box[4] = { ImVec2(p.x + Sc(2.f), p.y + Sc(22.f)), ImVec2(p.x + size.x - Sc(2.f), p.y + Sc(22.f)),
                                ImVec2(p.x + size.x, p.y + size.y), ImVec2(p.x, p.y + size.y) };
        dl->AddConvexPolyFilled(box, 4, IM_COL32(250, 200, 40, 255));
        dl->AddRectFilled(ImVec2(p.x + Sc(1.f), p.y + Sc(34.f)), ImVec2(p.x + size.x - Sc(1.f), p.y + Sc(42.f)), kCrayons[3]);
        Sketch(dl, box, 4, kInk, Sc(1.4f));
    }

    bool Takeover() const override { return painting_; }
    void EndTakeover(float) override { painting_ = false; stroke_ = -1; }   // the drawing is kept for next time

    // The canvas: white paper over most of the page, the tools along the bottom.
    void DrawTakeover(ImVec2 a, ImVec2 b, CRTSettings&, float t) override {
        const float  barH = Sc(88.f);
        const ImVec2 c0(a.x + Sc(4.f), a.y + Sc(4.f)), c1(b.x - Sc(4.f), b.y - barH - Sc(6.f));
        const float  u = Sc(1.f);   // drawings are kept at 1x, so they rescale with the menu
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        canvasSize_ = ImVec2((c1.x - c0.x) / u, (c1.y - c0.y) / u);

        dl->AddRectFilled(ImVec2(c0.x + Sc(3.f), c0.y + Sc(4.f)), ImVec2(c1.x + Sc(3.f), c1.y + Sc(4.f)), IM_COL32(0, 0, 0, 40), Sc(3.f));
        dl->AddRectFilled(c0, c1, kWhite, Sc(3.f));

        // Drawing: a new stroke when the button goes down; while it's held a
        // freehand line gains points and a shape stretches to the mouse.
        ImGui::SetCursorScreenPos(c0);
        ImGui::InvisibleButton("##canvas", ImVec2(c1.x - c0.x, c1.y - c0.y));
        const ImVec2 m  = ImGui::GetIO().MousePos;
        const ImVec2 lp((m.x - c0.x) / u, (m.y - c0.y) / u);
        if (ImGui::IsItemActivated()) {
            Stroke st;
            st.tool   = tool_;
            st.colour = colour_;
            st.size   = kSizes[size_] * (tool_ == kEraser ? 3.f : 1.f);
            st.filled = filled_;
            st.seed   = t;
            st.pts.push_back(lp);
            if (IsShape(tool_)) st.pts.push_back(lp);
            strokes_.push_back(std::move(st));
            stroke_ = tool_ == kStamp ? -1 : static_cast<int>(strokes_.size()) - 1;
        } else if (ImGui::IsItemActive() && stroke_ >= 0) {
            Stroke& st = strokes_[stroke_];
            if (IsShape(st.tool)) {
                st.pts[1] = ShapeEnd(st.tool, st.pts[0], lp, ImGui::GetIO().KeyShift);
            } else {
                const ImVec2 last = st.pts.back();
                if ((lp.x - last.x) * (lp.x - last.x) + (lp.y - last.y) * (lp.y - last.y) > 2.f && st.pts.size() < 4000)
                    st.pts.push_back(lp);
            }
        } else if (stroke_ >= 0) {
            // Let go: a shape that never got any size was just a click.
            const Stroke& st = strokes_[stroke_];
            if (IsShape(st.tool) && fabsf(st.pts[1].x - st.pts[0].x) < 2.f && fabsf(st.pts[1].y - st.pts[0].y) < 2.f)
                strokes_.erase(strokes_.begin() + stroke_);
            stroke_ = -1;
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z) && !strokes_.empty()) { strokes_.pop_back(); stroke_ = -1; }

        dl->PushClipRect(c0, c1, true);
        for (const Stroke& st : strokes_) DrawStroke(dl, c0, u, st);
        if (strokes_.empty()) {
            const char*  hint = "Draw something!";
            const float  fs   = Sc(26.f);
            const ImVec2 ts   = TextSize(fs, hint);
            CrayonText(dl, fs, ImVec2((c0.x + c1.x - ts.x) * 0.5f, (c0.y + c1.y - ts.y) * 0.5f), IM_COL32(200, 200, 210, 255), hint);
        }
        dl->PopClipRect();
        dl->AddRect(c0, c1, WithAlpha(kInk, 0.7f), Sc(3.f), 0, Sc(1.5f));

        DrawToolbar(dl, ImVec2(a.x, b.y - barH), ImVec2(b.x, b.y));
    }

private:
    static ImVec2 FooterButton() { return ImVec2(Sc(110.f), Sc(38.f)); }

    enum Tool { kPen, kLine, kRect, kCircle, kEraser, kStamp };
    static constexpr int   kRainbow = kCrayonCount;   // the colour after the crayons
    static constexpr float kSizes[3] = { 3.f, 7.f, 14.f };

    struct Stroke {
        Tool                tool   = kPen;
        int                 colour = 4;
        float               size   = 7.f;
        bool                filled = false;
        float               seed   = 0.f;
        std::vector<ImVec2> pts;   // at 1x, from the canvas's top-left; a shape's two corners
    };

    static bool IsShape(Tool tool) { return tool == kLine || tool == kRect || tool == kCircle; }

    // Where a shape's dragged corner lands.  Shift makes squares and circles,
    // and snaps lines to the nearest 45 degrees.
    static ImVec2 ShapeEnd(Tool tool, ImVec2 from, ImVec2 to, bool shift) {
        if (!shift) return to;
        const float dx = to.x - from.x, dy = to.y - from.y;
        if (tool == kLine) {
            const float len = sqrtf(dx * dx + dy * dy);
            const float ang = roundf(atan2f(dy, dx) / 0.7853982f) * 0.7853982f;
            return ImVec2(from.x + cosf(ang) * len, from.y + sinf(ang) * len);
        }
        const float side = std::max(fabsf(dx), fabsf(dy));
        return ImVec2(from.x + (dx < 0.f ? -side : side), from.y + (dy < 0.f ? -side : side));
    }

    // A shape's outline as points (1x units, clockwise on screen), spaced
    // closely enough for the rainbow crayon to change colour along it.
    static std::vector<ImVec2> ShapePath(const Stroke& st) {
        std::vector<ImVec2> path;
        const ImVec2 p = st.pts[0], q = st.pts[1];
        auto edge = [&](ImVec2 from, ImVec2 to) {
            const float len = sqrtf((to.x - from.x) * (to.x - from.x) + (to.y - from.y) * (to.y - from.y));
            const int   n   = std::max(1, static_cast<int>(len / 4.f));
            for (int i = 0; i < n; ++i)
                path.push_back(ImVec2(from.x + (to.x - from.x) * i / n, from.y + (to.y - from.y) * i / n));
        };
        if (st.tool == kLine) {
            edge(p, q);
            path.push_back(q);
        } else if (st.tool == kRect) {
            const ImVec2 lo(std::min(p.x, q.x), std::min(p.y, q.y)), hi(std::max(p.x, q.x), std::max(p.y, q.y));
            edge(lo, ImVec2(hi.x, lo.y));
            edge(ImVec2(hi.x, lo.y), hi);
            edge(hi, ImVec2(lo.x, hi.y));
            edge(ImVec2(lo.x, hi.y), lo);
        } else {   // circle: the ellipse filling the dragged box
            const ImVec2 c((p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f);
            const ImVec2 r(fabsf(q.x - p.x) * 0.5f, fabsf(q.y - p.y) * 0.5f);
            const int    n = std::clamp(static_cast<int>((r.x + r.y) * 3.14159f / 4.f), 16, 400);
            for (int i = 0; i < n; ++i) {
                const float ang = i * 6.2831853f / n;
                path.push_back(ImVec2(c.x + cosf(ang) * r.x, c.y + sinf(ang) * r.y));
            }
        }
        return path;
    }

    static ImU32 Rainbow(float h) {
        float r, g, bl;
        ImGui::ColorConvertHSVtoRGB(h - floorf(h), 0.8f, 0.95f, r, g, bl);
        return IM_COL32(static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(bl * 255), 255);
    }

    // A crayon line: round-ended segments, with a waxy lighter streak down
    // the middle.  The rainbow crayon changes colour along the line; a filled
    // shape is coloured in first; a stamp is a gold star.
    static void DrawStroke(ImDrawList* dl, ImVec2 o, float u, const Stroke& st) {
        auto at = [&](ImVec2 p) { return ImVec2(o.x + p.x * u, o.y + p.y * u); };
        if (st.tool == kStamp) {
            StarSticker(dl, at(st.pts[0]), st.size * 2.2f * u, (Hash(st.seed, 4.f) - 0.5f) * 0.8f);
            return;
        }
        const bool                 shape  = IsShape(st.tool);
        const bool                 closed = st.tool == kRect || st.tool == kCircle;
        const std::vector<ImVec2>  path   = shape ? ShapePath(st) : std::vector<ImVec2>();
        const std::vector<ImVec2>& pts    = shape ? path : st.pts;
        const float                w      = st.size * u;
        auto colour = [&](size_t i) -> ImU32 {
            if (st.tool == kEraser)    return kWhite;
            if (st.colour == kRainbow) return Rainbow(st.seed * 0.37f + i * 0.015f);
            return kCrayons[st.colour];
        };

        // Colouring in, a little lighter than the outline.  (Rectangles and
        // ellipses are convex, so a convex fill does.)
        if (closed && st.filled && pts.size() >= 3) {
            std::vector<ImVec2> sp(pts.size());
            float x0 = FLT_MAX, x1 = -FLT_MAX;
            for (size_t i = 0; i < pts.size(); ++i) {
                sp[i] = at(pts[i]);
                x0 = std::min(x0, sp[i].x);
                x1 = std::max(x1, sp[i].x);
            }
            if (st.colour == kRainbow) {
                // Rainbow: vertical bands, each the shape clipped to a strip.
                constexpr int kBands = 24;
                for (int i = 0; i < kBands; ++i) {
                    const float bx0 = x0 + (x1 - x0) * i / kBands, bx1 = x0 + (x1 - x0) * (i + 1) / kBands;
                    dl->PushClipRect(ImVec2(bx0, -1e6f), ImVec2(bx1 + 0.5f, 1e6f), true);
                    dl->AddConvexPolyFilled(sp.data(), static_cast<int>(sp.size()),
                                            Mix(Rainbow(st.seed * 0.37f + (i + 0.5f) / kBands * 0.8f), kWhite, 0.25f));
                    dl->PopClipRect();
                }
            } else {
                dl->AddConvexPolyFilled(sp.data(), static_cast<int>(sp.size()), Mix(colour(0), kWhite, 0.25f));
            }
        }

        const size_t n    = pts.size();
        const size_t segs = closed ? n : (n ? n - 1 : 0);
        for (size_t i = 0; i < segs; ++i)
            dl->AddLine(at(pts[i]), at(pts[(i + 1) % n]), colour(i), w);
        for (size_t i = 0; i < n; ++i)
            dl->AddCircleFilled(at(pts[i]), w * 0.5f, colour(i), w > Sc(6.f) ? 12 : 8);
        if (st.tool != kEraser)
            for (size_t i = 0; i < segs; ++i) {
                const ImVec2 p = at(pts[i]), q = at(pts[(i + 1) % n]);
                dl->AddLine(ImVec2(p.x - w * 0.15f, p.y - w * 0.15f), ImVec2(q.x - w * 0.15f, q.y - w * 0.15f),
                            WithAlpha(Mix(colour(i), kWhite, 0.4f), 0.35f), std::max(1.f, w * 0.25f));
            }
    }

    // Two rows.  Top: the crayons (and the rainbow one), then the brush
    // sizes.  Bottom: freehand, line, rectangle, circle | outline, filled |
    // eraser, star stamp, then Undo and Clear on the right.
    void DrawToolbar(ImDrawList* dl, ImVec2 a, ImVec2 b) {
        const float  row1 = a.y, row2 = a.y + Sc(46.f), rowH2 = b.y - row2;
        const float  cy2  = row2 + rowH2 * 0.5f;
        const ImU32  ink  = WithAlpha(kInk, 0.9f);

        // --- Crayons ----------------------------------------------------------
        float x = a.x + Sc(10.f);
        for (int i = 0; i <= kCrayonCount; ++i) {
            const float w = Sc(22.f);
            ImGui::SetCursorScreenPos(ImVec2(x, row1));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##crayon", ImVec2(w, Sc(44.f)))) {
                colour_ = i;
                if (tool_ == kEraser || tool_ == kStamp) tool_ = kPen;   // picking a colour means drawing
            }
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool  chosen = colour_ == i && tool_ != kEraser && tool_ != kStamp;
            const float lift   = chosen ? Sc(-8.f) : hovered ? Sc(-4.f) : 0.f;
            Crayon(dl, x + w * 0.5f, row1 + Sc(12.f) + lift, row1 + Sc(44.f), Sc(13.f),
                   i < kCrayonCount ? kCrayons[i] : kWhite, i == kRainbow);
            x += w + Sc(3.f);
        }

        // --- Sizes, on the right ---------------------------------------------
        for (int i = 0; i < 3; ++i) {
            const float  cx = b.x - Sc(16.f) - (2 - i) * Sc(26.f);
            const ImVec2 c(cx, row1 + Sc(26.f));
            ImGui::SetCursorScreenPos(ImVec2(cx - Sc(12.f), row1 + Sc(12.f)));
            ImGui::PushID(100 + i);
            if (ImGui::InvisibleButton("##size", ImVec2(Sc(24.f), Sc(28.f)))) size_ = i;
            ImGui::PopID();
            if (size_ == i) dl->AddCircle(c, Sc(11.f), kCrayons[4], 20, Sc(2.f));
            dl->AddCircleFilled(c, Sc(2.5f + i * 3.f), kInk, 16);
        }

        // --- Tools -------------------------------------------------------------
        // A square icon button; `on` draws it chosen.  True when clicked.
        auto iconButton = [&](const char* id, float cx, bool on, const char* tip) {
            const float  s = Sc(32.f);
            const ImVec2 p(cx - s * 0.5f, cy2 - s * 0.5f);
            ImGui::SetCursorScreenPos(p);
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(s, s));
            const bool hovered = ImGui::IsItemHovered();
            if (on || hovered)
                dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), on ? IM_COL32(190, 216, 250, 255) : IM_COL32(230, 240, 252, 255), Sc(7.f));
            if (on) dl->AddRect(p, ImVec2(p.x + s, p.y + s), kCrayons[4], Sc(7.f), 0, Sc(2.f));
            Tip(tip);
            return clicked;
        };
        const float step = Sc(37.f), sep = Sc(12.f);
        x = a.x + Sc(26.f);
        const ImU32 shapeCol = colour_ < kCrayonCount ? kCrayons[colour_] : kCrayons[5];
        const float k = Sc(9.f);   // icon half-size

        if (iconButton("##pen", x, tool_ == kPen, "Crayon - draw freehand")) tool_ = kPen;
        {
            ImVec2 sq[12];
            for (int i = 0; i < 12; ++i) sq[i] = ImVec2(x - k + 2.f * k * i / 11.f, cy2 + k * 0.6f * sinf(i * 0.9f));
            dl->AddPolyline(sq, 12, ink, 0, Sc(2.f));
        }
        x += step;
        if (iconButton("##line", x, tool_ == kLine, "Line - hold Shift to snap to 45 degrees")) tool_ = kLine;
        dl->AddLine(ImVec2(x - k, cy2 + k), ImVec2(x + k, cy2 - k), ink, Sc(2.5f));
        x += step;
        if (iconButton("##rect", x, tool_ == kRect, "Rectangle - hold Shift for a square")) tool_ = kRect;
        dl->AddRect(ImVec2(x - k, cy2 - k * 0.75f), ImVec2(x + k, cy2 + k * 0.75f), ink, 0.f, 0, Sc(2.f));
        x += step;
        if (iconButton("##circle", x, tool_ == kCircle, "Circle - hold Shift for a perfect circle")) tool_ = kCircle;
        dl->AddCircle(ImVec2(x, cy2), k, ink, 24, Sc(2.f));
        x += step + sep;

        // Outline or filled, for rectangles and circles.
        if (iconButton("##outline", x, !filled_, "Outline shapes")) filled_ = false;
        dl->AddRect(ImVec2(x - k, cy2 - k), ImVec2(x + k, cy2 + k), shapeCol, Sc(3.f), 0, Sc(2.5f));
        x += step;
        if (iconButton("##filled", x, filled_, "Filled shapes")) filled_ = true;
        dl->AddRectFilled(ImVec2(x - k, cy2 - k), ImVec2(x + k, cy2 + k), Mix(shapeCol, kWhite, 0.25f), Sc(3.f));
        dl->AddRect(ImVec2(x - k, cy2 - k), ImVec2(x + k, cy2 + k), shapeCol, Sc(3.f), 0, Sc(2.5f));
        x += step + sep;

        if (iconButton("##eraser", x, tool_ == kEraser, "Eraser")) tool_ = kEraser;
        {
            const ImVec2 e0(x - Sc(11.f), cy2 - Sc(7.f)), e1(x + Sc(11.f), cy2 + Sc(7.f));
            dl->AddRectFilled(e0, e1, IM_COL32(240, 140, 160, 255), Sc(3.f));
            dl->AddRectFilled(ImVec2(e0.x, e0.y + Sc(8.f)), e1, IM_COL32(80, 110, 200, 255), Sc(3.f), ImDrawFlags_RoundCornersBottom);
            dl->AddRect(e0, e1, WithAlpha(kInk, 0.7f), Sc(3.f), 0, Sc(1.2f));
        }
        x += step;
        if (iconButton("##stamp", x, tool_ == kStamp, "Star stamp")) tool_ = kStamp;
        StarSticker(dl, ImVec2(x, cy2), Sc(11.f), 0.f);

        // --- Undo and Clear, on the right -------------------------------------
        auto textButton = [&](const char* id, const char* label, float right, const char* tip) {
            const float  fs = Sc(17.f);
            const ImVec2 ts = TextSize(fs, label);
            const ImVec2 p(right - ts.x - Sc(14.f), cy2 - Sc(14.f));
            ImGui::SetCursorScreenPos(p);
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(ts.x + Sc(14.f), Sc(28.f)));
            const bool hovered = ImGui::IsItemHovered();
            dl->AddRectFilled(p, ImVec2(p.x + ts.x + Sc(14.f), p.y + Sc(28.f)), hovered ? IM_COL32(255, 236, 140, 255) : kWhite, Sc(8.f));
            dl->AddRect(p, ImVec2(p.x + ts.x + Sc(14.f), p.y + Sc(28.f)), WithAlpha(kInk, 0.8f), Sc(8.f), 0, Sc(1.5f));
            dl->AddText(Font(), fs, ImVec2(p.x + Sc(7.f), cy2 - ts.y * 0.52f), kInk, label);
            Tip(tip);
            return std::make_pair(clicked, p.x);
        };
        const auto clear = textButton("##clear", "Clear", b.x - Sc(4.f), "Start a new drawing.");
        if (clear.first) { strokes_.clear(); stroke_ = -1; }
        const auto undo = textButton("##undo", "Undo", clear.second - Sc(6.f), "Undo the last thing drawn (Ctrl+Z).");
        if (undo.first && !strokes_.empty()) { strokes_.pop_back(); stroke_ = -1; }
        const auto save = textButton("##save", "Save", undo.second - Sc(6.f), "Save the drawing as a picture in Pictures\\SeeRT.");
        if (save.first && !strokes_.empty()) s_saveDrawingRequested = true;
    }

public:
    // The drawing for saving: its size at `scale`, and the strokes into dl.
    ImVec2 DrawingSize(float scale) const {
        return strokes_.empty() ? ImVec2(0.f, 0.f) : ImVec2(floorf(canvasSize_.x * scale), floorf(canvasSize_.y * scale));
    }
    void RenderDrawing(ImDrawList* dl, float scale) const {
        for (const Stroke& st : strokes_) DrawStroke(dl, ImVec2(0.f, 0.f), scale, st);
    }

private:

    bool                painting_ = false;
    std::vector<Stroke> strokes_;
    int                 stroke_  = -1;     // the stroke being drawn, if any
    Tool                tool_    = kPen;
    int                 colour_  = 4;      // blue crayon
    int                 size_    = 1;
    bool                filled_  = false;
    ImVec2              canvasSize_ = ImVec2(540.f, 400.f);   // at 1x
};

} // namespace complab

// The Computer Lab style lives here rather than in Draw, so its drawing can be saved.
static complab::Style s_complabStyle;

ImVec2 SettingsPanel::DrawingSize(float scale) { return s_complabStyle.DrawingSize(scale); }

void SettingsPanel::DrawDrawing(ImDrawList* dl, float scale) {
    // Strokes size their details with Sc(); draw them at the save scale.
    const float saved = s_menuScale;
    s_menuScale = scale;
    s_complabStyle.RenderDrawing(dl, scale);
    s_menuScale = saved;
}

// ---------------------------------------------------------------------------
// Web 1.0 layout — a 90s personal homepage, in a 90s browser.
//   • The panel is a grey browser window: title bar, menu bar, a toolbar
//     (Back, Home and Reload work), an address bar, a spinning globe while
//     the page loads, and a status bar that shows where a link goes.
//   • The page: a tiled starfield, a rainbow Comic Sans heading, a marquee,
//     an Under Construction sign, lime links with bullets that turn purple
//     once visited, an 88 × 31 GitHub badge and a visitor counter.
//   • Pages load the old way, a band at a time.
//   • The book by the heading is the guestbook: sign it, and it keeps every
//     signature — after a few from 1998.
// ---------------------------------------------------------------------------
namespace web {

constexpr ImU32 kFace   = IM_COL32(192, 192, 192, 255);   // Windows 95 grey
constexpr ImU32 kLight  = IM_COL32(223, 223, 223, 255);
constexpr ImU32 kDark   = IM_COL32(128, 128, 128, 255);
constexpr ImU32 kBlack  = IM_COL32(  0,   0,   0, 255);
constexpr ImU32 kWhite  = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kNavy   = IM_COL32(  0,   0, 128, 255);
constexpr ImU32 kLink   = IM_COL32(  0, 255,   0, 255);   // lime
constexpr ImU32 kHover  = IM_COL32(255, 255,   0, 255);
constexpr ImU32 kVisit  = IM_COL32(255,  80, 255, 255);
constexpr ImU32 kYellow = IM_COL32(255, 255,   0, 255);
constexpr ImU32 kRed    = IM_COL32(255,   0,   0, 255);

constexpr const char* kSite = "file:///C|/My Homepage/";

ImFont* BodyFont()    { return StyleFonts::times  ? StyleFonts::times  : ImGui::GetFont(); }
ImFont* HeadFont()    { return StyleFonts::comic  ? StyleFonts::comic  : ImGui::GetFont(); }
ImFont* ChromeFont()  { return StyleFonts::msSans ? StyleFonts::msSans : ImGui::GetFont(); }

ImVec2 TextSize(ImFont* f, float size, const char* text) { return f->CalcTextSizeA(size, FLT_MAX, 0.f, text); }

ImU32 Rainbow(float h, float alpha = 1.f) {
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h - floorf(h), 0.9f, 1.f, r, g, b);
    return IM_COL32(static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(b * 255),
                    static_cast<int>(alpha * 255));
}

// "Shadow Mask" → "shadow_mask", "Menu & App" → "menu_and_app".
std::string Slug(const char* s) {
    std::string out;
    for (const char* p = s; *p; ++p) {
        const unsigned char ch = static_cast<unsigned char>(*p);
        if (isalnum(ch))       out += static_cast<char>(tolower(ch));
        else if (ch == '&')    out += "and";
        else if (ch == ' ' && !out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

// A one-pixel line of the bevel (whole pixels, so it stays crisp).
void HLine(ImDrawList* dl, float x0, float x1, float y, ImU32 col) {
    const float px = std::max(1.f, floorf(Sc(1.f) + 0.5f));
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + px), col);
}
void VLine(ImDrawList* dl, float x, float y0, float y1, ImU32 col) {
    const float px = std::max(1.f, floorf(Sc(1.f) + 0.5f));
    dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + px, y1), col);
}

// The Windows 95 3-D edge: raised (buttons, the window) or sunken (fields).
void Bevel(ImDrawList* dl, ImVec2 a, ImVec2 b, bool raised) {
    const float px = std::max(1.f, floorf(Sc(1.f) + 0.5f));
    const ImU32 o1 = raised ? kWhite : kDark,  o2 = raised ? kBlack : kWhite;   // outer: top-left, bottom-right
    const ImU32 i1 = raised ? kLight : kBlack, i2 = raised ? kDark  : kLight;   // inner
    HLine(dl, a.x, b.x, a.y, o1);                VLine(dl, a.x, a.y, b.y, o1);
    HLine(dl, a.x, b.x, b.y - px, o2);           VLine(dl, b.x - px, a.y, b.y, o2);
    HLine(dl, a.x + px, b.x - px, a.y + px, i1); VLine(dl, a.x + px, a.y + px, b.y - px, i1);
    HLine(dl, a.x + px, b.x - px, b.y - 2 * px, i2); VLine(dl, b.x - 2 * px, a.y + px, b.y - px, i2);
}

// A grey push button at the cursor; it sinks while held.  True when clicked.
bool Button95(const char* id, const char* label, ImVec2 size, float fontSize, bool enabled = true) {
    const ImVec2 pos     = ImGui::GetCursorScreenPos();
    const bool   clicked = ImGui::InvisibleButton(id, size) && enabled;
    const bool   held    = ImGui::IsItemActive() && enabled;
    ImDrawList*  dl      = ImGui::GetWindowDrawList();
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, b, kFace);
    Bevel(dl, pos, b, !held);
    ImFont*      f  = ChromeFont();
    const ImVec2 ts = TextSize(f, fontSize, label);
    const float  o  = held ? Sc(1.f) : 0.f;
    const ImVec2 tp((pos.x + b.x - ts.x) * 0.5f + o, (pos.y + b.y - ts.y) * 0.5f + o);
    if (!enabled) dl->AddText(f, fontSize, ImVec2(tp.x + Sc(1.f), tp.y + Sc(1.f)), kWhite, label);
    dl->AddText(f, fontSize, tp, enabled ? kBlack : kDark, label);
    return clicked;
}

// A tiled starfield: one tile's worth of stars repeated across a..b, the
// way a background GIF tiled.  A few of them twinkle.
void Starfield(ImDrawList* dl, ImVec2 a, ImVec2 b, float t) {
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 8, 255));
    const float tile = Sc(96.f);
    dl->PushClipRect(a, b, true);
    for (float ty = a.y; ty < b.y; ty += tile)
        for (float tx = a.x; tx < b.x; tx += tile)
            for (int i = 0; i < 14; ++i) {
                const float fi = static_cast<float>(i);
                const ImVec2 p(tx + Hash(fi, 1.f) * tile, ty + Hash(fi, 2.f) * tile);
                const bool   big = i < 2;
                const float  tw  = i == 3 ? 0.5f + 0.5f * sinf(t * 4.f + tx * 0.01f + ty * 0.02f) : 1.f;
                const ImU32  col = WithAlpha(i % 5 == 0 ? IM_COL32(255, 255, 160, 255) : kWhite, (0.35f + 0.5f * Hash(fi, 3.f)) * tw);
                if (big) {
                    dl->AddLine(ImVec2(p.x - Sc(3.f), p.y), ImVec2(p.x + Sc(3.f), p.y), col, Sc(1.f));
                    dl->AddLine(ImVec2(p.x, p.y - Sc(3.f)), ImVec2(p.x, p.y + Sc(3.f)), col, Sc(1.f));
                } else {
                    dl->AddRectFilled(p, ImVec2(p.x + Sc(1.f), p.y + Sc(1.f)), col);
                }
            }
    dl->PopClipRect();
}

// The animated rainbow rule every homepage had between sections.
void RainbowRule(ImDrawList* dl, ImVec2 a, float width, float t) {
    const int n = 24;
    for (int i = 0; i < n; ++i) {
        const float x0 = a.x + width * i / n, x1 = a.x + width * (i + 1) / n;
        const ImU32 c0 = Rainbow(i / static_cast<float>(n) + t * 0.3f), c1 = Rainbow((i + 1) / static_cast<float>(n) + t * 0.3f);
        dl->AddRectFilledMultiColor(ImVec2(x0, a.y), ImVec2(x1, a.y + Sc(4.f)), c0, c1, c1, c0);
    }
}

// The globe in the corner of the toolbar, spinning while a page loads.
void Globe(ImDrawList* dl, ImVec2 c, float r, float spin) {
    dl->AddCircleFilled(c, r, IM_COL32(20, 60, 170, 255), 32);
    for (int i = -1; i <= 1; ++i)   // latitudes
        dl->AddLine(ImVec2(c.x - r * sqrtf(1.f - 0.25f * i * i) * 0.98f, c.y + r * 0.5f * i),
                    ImVec2(c.x + r * sqrtf(1.f - 0.25f * i * i) * 0.98f, c.y + r * 0.5f * i), IM_COL32(140, 200, 255, 200), Sc(1.f));
    for (int k = 0; k < 3; ++k) {   // meridians, turning
        const float ph = spin + k * 1.0471976f;
        const float w  = r * fabsf(cosf(ph));
        dl->AddEllipse(c, ImVec2(std::max(0.5f, w), r), IM_COL32(140, 200, 255, 200), 0.f, 24, Sc(1.f));
    }
    dl->AddCircle(c, r, IM_COL32(0, 0, 60, 255), 32, Sc(1.2f));
    dl->AddCircleFilled(ImVec2(c.x - r * 0.35f, c.y - r * 0.4f), r * 0.25f, IM_COL32(255, 255, 255, 60), 12);
}

// The Web 1.0 look for the shared paged menu.
class Style : public PagedMenuStyle {
public:
    ImVec2  PanelSize()   const override { return ImVec2(740.f, 720.f); }
    ImVec2  Padding()     const override { return ImVec2(44.f, 0.f); }
    float   TitleSpace()  const override { return 196.f; }   // browser chrome + the page heading
    float   FooterSpace() const override { return 78.f; }    // the page's footer row + the status bar
    float   FooterRow()   const override { return 70.f; }
    float   ColumnGap()   const override { return 24.f; }
    float   GroupButtonHeight() const override { return 30.f; }
    float   ItemButtonHeight()  const override { return 28.f; }
    ImFont* Font()        const override { return BodyFont(); }
    float   FontSize()    const override { return 18.f; }
    std::string PageTitle(int pageIndex) const override {
        return pageIndex < 0 ? "My SeeRT Homepage" : TitleCase(kSectionNames[pageIndex]);
    }

    // Pages load a band at a time, top to bottom, after a moment's connecting.
    float PageReveal(float t) const override { return std::clamp(floorf((t - 0.2f) / 0.07f) / 8.f, 0.f, 1.f); }

    void DrawPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, int pageIndex, float t) override {
        // The window.
        dl->AddRectFilled(ImVec2(a.x + Sc(6.f), a.y + Sc(8.f)), ImVec2(b.x + Sc(6.f), b.y + Sc(8.f)), IM_COL32(0, 0, 0, 90));
        dl->AddRectFilled(a, b, kFace);
        Bevel(dl, a, b, true);

        // Title bar.
        const ImVec2 t0(a.x + Sc(4.f), a.y + Sc(4.f)), t1(b.x - Sc(4.f), a.y + Sc(26.f));
        dl->AddRectFilledMultiColor(t0, t1, kNavy, IM_COL32(16, 132, 208, 255), IM_COL32(16, 132, 208, 255), kNavy);
        Globe(dl, ImVec2(t0.x + Sc(11.f), (t0.y + t1.y) * 0.5f), Sc(7.f), 0.6f);
        ImFont* cf = ChromeFont();
        const float cfs = Sc(13.f);
        const char* title = guestbook_ ? "Sign My Guestbook!! - SeeRT Web Navigator" : "My SeeRT Homepage - SeeRT Web Navigator";
        dl->AddText(cf, cfs, ImVec2(t0.x + Sc(24.f), (t0.y + t1.y - cfs) * 0.5f), kWhite, title);
        dl->AddText(cf, cfs, ImVec2(t0.x + Sc(24.6f), (t0.y + t1.y - cfs) * 0.5f), kWhite, title);   // bold
        // Minimise and maximise, for show (close is the real close button).
        for (int i = 0; i < 2; ++i) {
            const ImVec2 p(t1.x - Sc(56.f) + i * Sc(18.f), t0.y + Sc(3.f)), q(p.x + Sc(16.f), p.y + Sc(14.f));
            dl->AddRectFilled(p, q, kFace);
            Bevel(dl, p, q, true);
            if (i == 0) dl->AddRectFilled(ImVec2(p.x + Sc(4.f), q.y - Sc(5.f)), ImVec2(p.x + Sc(10.f), q.y - Sc(3.f)), kBlack);
            else {
                dl->AddRect(ImVec2(p.x + Sc(3.f), p.y + Sc(3.f)), ImVec2(q.x - Sc(4.f), q.y - Sc(3.f)), kBlack, 0.f, 0, Sc(1.f));
                dl->AddRectFilled(ImVec2(p.x + Sc(3.f), p.y + Sc(3.f)), ImVec2(q.x - Sc(4.f), p.y + Sc(5.f)), kBlack);
            }
        }

        // Menu bar.
        float mx = a.x + Sc(12.f);
        for (const char* item : { "File", "Edit", "View", "Go", "Bookmarks", "Options", "Directory", "Window", "Help" }) {
            dl->AddText(cf, cfs, ImVec2(mx, a.y + Sc(30.f)), kBlack, item);
            dl->AddLine(ImVec2(mx, a.y + Sc(30.f) + cfs), ImVec2(mx + TextSize(cf, cfs, item).x * 0.25f, a.y + Sc(30.f) + cfs),
                        kBlack, Sc(1.f));   // the underlined access key
            mx += TextSize(cf, cfs, item).x + Sc(14.f);
        }
        HLine(dl, a.x + Sc(4.f), b.x - Sc(4.f), a.y + Sc(47.f), kDark);
        HLine(dl, a.x + Sc(4.f), b.x - Sc(4.f), a.y + Sc(48.f), kWhite);

        // Address bar: where we are.
        const float ay = a.y + Sc(97.f);
        dl->AddText(cf, cfs, ImVec2(a.x + Sc(12.f), ay + Sc(4.f)), kBlack, "Location:");
        const ImVec2 f0(a.x + Sc(76.f), ay), f1(b.x - Sc(12.f), ay + Sc(22.f));
        dl->AddRectFilled(f0, f1, kWhite);
        Bevel(dl, f0, f1, false);
        std::string url = std::string(kSite) + (guestbook_ ? "cgi-bin/guestbook.cgi"
                                                : pageIndex < 0 ? "index.html" : Slug(kSectionNames[pageIndex]) + ".html");
        dl->PushClipRect(f0, ImVec2(f1.x - Sc(4.f), f1.y), true);
        dl->AddText(cf, cfs, ImVec2(f0.x + Sc(5.f), ay + Sc(4.f)), kBlack, url.c_str());
        dl->PopClipRect();

        // The page: sunken into the window, with its starry background.
        const ImVec2 p0 = PageMin(a), p1 = PageMax(b);
        Bevel(dl, ImVec2(p0.x - Sc(2.f), p0.y - Sc(2.f)), ImVec2(p1.x + Sc(2.f), p1.y + Sc(2.f)), false);
        Starfield(dl, p0, p1, t);

        // Status bar.
        const ImVec2 s0(a.x + Sc(4.f), b.y - Sc(24.f)), s1(b.x - Sc(4.f), b.y - Sc(4.f));
        Bevel(dl, s0, ImVec2(s1.x - Sc(200.f), s1.y), false);
        Bevel(dl, ImVec2(s1.x - Sc(196.f), s0.y), s1, false);
    }

    // The heading in rainbow Comic Sans, a construction sign and the
    // guestbook either side of it, and a marquee (on the main page) or a
    // rainbow rule (on the others) underneath.
    void DrawTitle(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* title, int pageIndex, float t) override {
        const ImVec2 p0 = PageMin(a), p1 = PageMax(b);
        if (guestbook_) title = "Sign My Guestbook!!";
        ImFont*     hf = HeadFont();
        const float hs = Sc(32.f);
        const ImVec2 ts = TextSize(hf, hs, title);
        const ImVec2 hp((p0.x + p1.x - ts.x) * 0.5f, p0.y + Sc(10.f));
        dl->AddText(hf, hs, ImVec2(hp.x + Sc(2.f), hp.y + Sc(2.f)), IM_COL32(80, 0, 120, 255), title);
        const int v0 = dl->VtxBuffer.Size;
        dl->AddText(hf, hs, hp, kWhite, title);
        ShadeVerts(dl, v0, [&](ImVec2 p) { return Rainbow((p.x - hp.x) / std::max(1.f, ts.x) * 0.85f - t * 0.25f); });

        UnderConstruction(dl, ImVec2(p0.x + Sc(40.f), p0.y + Sc(30.f)), t);

        const float ry = p0.y + Sc(54.f);
        if (pageIndex < 0 && !guestbook_) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "*** Welcome to my SeeRT Overlay homepage! ***   You are visitor #%d   *** "
                     "Best viewed at 800x600 ***   Sign my guestbook!!   *** Thanks for stopping by ***", hits_);
            ImFont*      bf = BodyFont();
            const float  bs = Sc(17.f);
            const float  w  = TextSize(bf, bs, msg).x;
            const float  span = (p1.x - p0.x) + w;
            const float  x  = p1.x - Sc(96.f) - fmodf(t * Sc(70.f), span);
            // Between the construction sign and the guestbook.
            dl->PushClipRect(ImVec2(p0.x + Sc(76.f), ry - Sc(2.f)), ImVec2(p1.x - Sc(96.f), ry + bs + Sc(4.f)), true);
            dl->AddText(bf, bs, ImVec2(x, ry), kYellow, msg);
            dl->PopClipRect();
        } else {
            RainbowRule(dl, ImVec2(p0.x + Sc(40.f), ry + Sc(8.f)), (p1.x - p0.x) - Sc(80.f), t);
        }
    }

    // A link: an animated bullet, then underlined text — lime, yellow when
    // hovered, purple once visited.  The current choice is highlighted, as if
    // selected.  The status bar shows where it goes.
    bool GroupButton(const char* id, const char* label, ImVec2 size, int index, bool selected, Kind kind) override {
        const ImVec2 pos     = ImGui::GetCursorScreenPos();
        const bool   clicked = ImGui::InvisibleButton(id, size);
        const bool   hovered = ImGui::IsItemHovered();
        ImDrawList*  dl      = ImGui::GetWindowDrawList();
        const float  t       = static_cast<float>(ImGui::GetTime());

        const std::string folder = kind == Kind::Group ? "" : kind == Kind::Preset ? "presets/" : "styles/";
        if (hovered) hoverUrl_ = std::string(kSite) + folder + Slug(label) + ".html";

        // Bullet: a little glossy ball, changing colour.
        const ImVec2 bc(pos.x + Sc(10.f), pos.y + size.y * 0.5f);
        dl->AddCircleFilled(bc, Sc(6.f), Rainbow(t * 0.5f + index * 0.13f), 16);
        dl->AddCircleFilled(ImVec2(bc.x - Sc(2.f), bc.y - Sc(2.f)), Sc(2.f), IM_COL32(255, 255, 255, 200), 8);

        ImFont*      f  = BodyFont();
        const float  fs = Sc(kind == Kind::Group ? 21.f : 19.f);
        const ImVec2 ts = TextSize(f, fs, label);
        const ImVec2 tp(pos.x + Sc(24.f), pos.y + (size.y - ts.y) * 0.5f);
        const bool   visited = kind == Kind::Group && (visited_ >> index & 1);
        ImU32 col = hovered ? kHover : visited ? kVisit : kLink;
        if (selected) {
            dl->AddRectFilled(ImVec2(tp.x - Sc(2.f), tp.y), ImVec2(tp.x + ts.x + Sc(2.f), tp.y + ts.y), kNavy);
            col = kWhite;
        }
        dl->AddText(f, fs, tp, col, label);
        dl->AddLine(ImVec2(tp.x, tp.y + ts.y - Sc(2.f)), ImVec2(tp.x + ts.x, tp.y + ts.y - Sc(2.f)), col, Sc(1.f));

        // Something on every homepage was NEW!
        if (kind == Kind::Group && index == kMenuSection && fmodf(t, 0.8f) < 0.5f) {
            const ImVec2 n0(tp.x + ts.x + Sc(8.f), pos.y + size.y * 0.5f - Sc(8.f));
            const float  nw = TextSize(ChromeFont(), Sc(11.f), "NEW!").x + Sc(8.f);
            dl->AddRectFilled(n0, ImVec2(n0.x + nw, n0.y + Sc(16.f)), kRed);
            dl->AddText(ChromeFont(), Sc(11.f), ImVec2(n0.x + Sc(4.f), n0.y + Sc(2.f)), kYellow, "NEW!");
        }
        return clicked;
    }

    bool  BackButton(const char* id) override { return Button95(id, "< Back", FooterButton(), Sc(13.f)); }
    float QuitWidth() const override { return FooterButton().x; }
    bool  QuitButton(const char* id) override { return Button95(id, "Quit", FooterButton(), Sc(13.f)); }

    // The status bar: what's loading, or where the hovered link goes; and the
    // visitor counter.
    void DrawFooter(ImDrawList* dl, ImVec2 a, ImVec2 b, int, float t) override {
        ImFont*      cf  = ChromeFont();
        const float  cfs = Sc(12.f);
        const float  y   = b.y - Sc(21.f);
        const float  since = t - pageTime;
        std::string  status = hoverUrl_;
        if (status.empty())
            status = since < 0.2f ? "Connecting to host..." : since < 0.8f ? "Transferring data..." : "Document: Done";
        dl->PushClipRect(ImVec2(a.x, y), ImVec2(b.x - Sc(206.f), b.y), true);
        dl->AddText(cf, cfs, ImVec2(a.x + Sc(12.f), y + Sc(2.f)), kBlack, status.c_str());
        dl->PopClipRect();

        // An odometer: white digits in black boxes.
        char digits[16];
        snprintf(digits, sizeof(digits), "%07d", hits_ % 10000000);
        dl->AddText(cf, cfs, ImVec2(b.x - Sc(190.f), y + Sc(2.f)), kBlack, "Visitors:");
        float x = b.x - Sc(130.f);
        for (const char* d = digits; *d; ++d) {
            dl->AddRectFilled(ImVec2(x, y + Sc(1.f)), ImVec2(x + Sc(15.f), y + Sc(17.f)), kBlack);
            const char ch[2] = { *d, 0 };
            dl->AddText(cf, cfs, ImVec2(x + Sc(4.f), y + Sc(2.f)), kWhite, ch);
            x += Sc(16.f);
        }
    }

    // The title bar's close box.
    ImVec2 CloseCenter(ImVec2 a, ImVec2 b) const override { return ImVec2(b.x - Sc(16.f), a.y + Sc(14.f)); }
    bool   CloseButton(const char* id, ImVec2 c) override {
        const ImVec2 p(c.x - Sc(8.f), c.y - Sc(7.f)), q(p.x + Sc(16.f), p.y + Sc(14.f));
        ImGui::SetCursorScreenPos(p);
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(q.x - p.x, q.y - p.y));
        const bool held    = ImGui::IsItemActive();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, q, kFace);
        Bevel(dl, p, q, !held);
        const float o = held ? Sc(1.f) : 0.f;
        dl->AddLine(ImVec2(p.x + Sc(4.f) + o, p.y + Sc(3.f) + o), ImVec2(q.x - Sc(4.f) + o, q.y - Sc(3.f) + o), kBlack, Sc(1.6f));
        dl->AddLine(ImVec2(q.x - Sc(4.f) + o, p.y + Sc(3.f) + o), ImVec2(p.x + Sc(4.f) + o, q.y - Sc(3.f) + o), kBlack, Sc(1.6f));
        return clicked;
    }

    // An 88 × 31 web badge.
    bool GitHubButton(const char* id, ImVec2 a, ImVec2 b) override {
        const ImVec2 size(Sc(88.f), Sc(31.f));
        const ImVec2 p0((a.x + b.x - size.x) * 0.5f, a.y);
        ImGui::SetCursorScreenPos(p0);
        const bool   clicked = ImGui::InvisibleButton(id, size);
        const bool   hovered = ImGui::IsItemHovered();
        ImDrawList*  dl      = ImGui::GetWindowDrawList();
        const ImVec2 p1(p0.x + size.x, p0.y + size.y);
        dl->AddRectFilledMultiColor(p0, p1, IM_COL32(70, 70, 80, 255), IM_COL32(70, 70, 80, 255), kBlack, kBlack);
        dl->AddRect(p0, p1, hovered ? kHover : kFace, 0.f, 0, Sc(1.f));
        GitHubMark(dl, ImVec2(p0.x + Sc(16.f), p0.y + size.y * 0.5f), Sc(20.f), kWhite);
        ImFont* f = ChromeFont();
        dl->AddText(f, Sc(13.f), ImVec2(p0.x + Sc(31.f), p0.y + Sc(3.f)), kWhite, "GitHub");
        dl->AddText(f, Sc(13.f), ImVec2(p0.x + Sc(31.6f), p0.y + Sc(3.f)), kWhite, "GitHub");
        dl->AddText(f, Sc(10.f), ImVec2(p0.x + Sc(31.f), p0.y + Sc(17.f)), hovered ? kHover : kLink, "source code");
        return clicked;
    }

    // Visited links, the visitor counter, and the hovered link's address.
    void Update(CRTSettings& s, float) override {
        hoverUrl_.clear();
        if (page >= 0 && page < kSectionCount) visited_ |= 1u << page;
        if (openTime != countedOpen_) {   // a new visit
            countedOpen_ = openTime;
            ++s.webHits;
        }
        hits_ = s.webHits;
    }

    // The toolbar (Back, Home, Reload and Stop work), the spinning globe and
    // the guestbook.
    void DrawExtras(ImVec2 a, ImVec2 b, float, CRTSettings&, float t) override {
        ImDrawList* dl    = ImGui::GetWindowDrawList();
        const float since = t - pageTime;
        const bool  busy  = since < 0.8f;

        struct Tool { const char* id; const char* label; const char* tip; };
        static constexpr Tool kTools[] = {
            { "##tb_back",   "Back",   "Back to the main page" },
            { "##tb_fwd",    "Forward", "" },
            { "##tb_reload", "Reload", "Load this page again" },
            { "##tb_home",   "Home",   "The main page" },
            { "##tb_search", "Search", "" },
            { "##tb_stop",   "Stop",   "Stop loading" },
        };
        float x = a.x + Sc(8.f);
        for (int i = 0; i < 6; ++i) {
            const ImVec2 p(x, a.y + Sc(51.f)), size(Sc(54.f), Sc(42.f));
            const bool enabled = i == 0 ? (page >= 0 || guestbook_) : i == 1 || i == 4 ? false : i == 5 ? busy : true;
            ImGui::SetCursorScreenPos(p);
            const bool clicked = ImGui::InvisibleButton(kTools[i].id, size) && enabled;
            const bool held    = ImGui::IsItemActive() && enabled;
            if (enabled && kTools[i].tip[0]) Tip(kTools[i].tip);
            dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), kFace);
            Bevel(dl, p, ImVec2(p.x + size.x, p.y + size.y), !held);
            const float  o = held ? Sc(1.f) : 0.f;
            const ImVec2 ic(p.x + size.x * 0.5f + o, p.y + Sc(14.f) + o);
            ToolIcon(dl, i, ic, enabled);
            ImFont*      cf = ChromeFont();
            const ImVec2 ls = TextSize(cf, Sc(11.f), kTools[i].label);
            const ImVec2 lp(p.x + (size.x - ls.x) * 0.5f + o, p.y + Sc(26.f) + o);
            if (!enabled) dl->AddText(cf, Sc(11.f), ImVec2(lp.x + Sc(1.f), lp.y + Sc(1.f)), kWhite, kTools[i].label);
            dl->AddText(cf, Sc(11.f), lp, enabled ? kBlack : kDark, kTools[i].label);
            if (clicked) {
                if (i == 0 || i == 3) { guestbook_ = false; page = -1; pageTime = t; }
                if (i == 2)           pageTime = t;            // load it all again
                if (i == 5)           pageTime = t - 10.f;     // done, now
            }
            x += size.x + Sc(2.f);
        }

        // The globe spins while the page loads.
        const ImVec2 g0(b.x - Sc(54.f), a.y + Sc(51.f)), g1(b.x - Sc(10.f), a.y + Sc(93.f));
        dl->AddRectFilled(g0, g1, kBlack);
        Bevel(dl, g0, g1, false);
        Globe(dl, ImVec2((g0.x + g1.x) * 0.5f, (g0.y + g1.y) * 0.5f), Sc(15.f), busy ? t * 6.f : 0.6f);
        if (busy)   // a comet across it
            dl->AddCircleFilled(ImVec2(g0.x + Sc(6.f) + fmodf(since * 90.f, 32.f) * Sc(1.f), g0.y + Sc(10.f)), Sc(2.f), kYellow, 8);

        // The guestbook, by the heading.
        const ImVec2 p1 = PageMax(b), p0 = PageMin(a);
        const ImVec2 bc(p1.x - Sc(52.f), p0.y + Sc(30.f));
        ImGui::SetCursorScreenPos(ImVec2(bc.x - Sc(30.f), bc.y - Sc(22.f)));
        if (ImGui::InvisibleButton("##guestbook", ImVec2(Sc(60.f), Sc(48.f)))) {
            guestbook_ = true;
            pageTime   = t;
        }
        const bool hovered = ImGui::IsItemHovered();
        if (!guestbook_) Tip("Sign my guestbook!!");
        Book(dl, bc, hovered, t);
    }

    bool Takeover() const override { return guestbook_; }
    void EndTakeover(float t) override { guestbook_ = false; pageTime = t; }

    // Easter egg: the guestbook — sign it, then read everyone who has.
    void DrawTakeover(ImVec2 a, ImVec2 b, CRTSettings& s, float) override {
        ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + Sc(4.f)));
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, kYellow);
        ImGui::TextUnformatted("Thanks for visiting! Please sign my guestbook :-)");
        ImGui::PopStyleColor();
        const float w = b.x - a.x;
        ImGui::SetNextItemWidth(w * 0.5f);
        ImGui::InputTextWithHint("Your name", "Name", name_, sizeof(name_));
        ImGui::InputTextMultiline("##message", message_, sizeof(message_), ImVec2(w, Sc(58.f)));
        const bool ready = name_[0] && message_[0];
        if (!ready) ImGui::BeginDisabled();
        if (ImGui::Button("Sign Guestbook")) {
            const time_t now = time(nullptr);
            tm lt{};
            localtime_s(&lt, &now);
            char date[16];
            strftime(date, sizeof(date), "%Y-%m-%d", &lt);
            s.guestbook.push_back({ name_, message_, date });
            if (s.guestbook.size() > 100) s.guestbook.erase(s.guestbook.begin());
            name_[0] = message_[0] = 0;
        }
        if (!ready) ImGui::EndDisabled();
        ImGui::EndGroup();

        // Everyone who's signed, newest first, then the ones from 1998.
        ImGui::BeginChild("##entries", ImVec2(w, b.y - ImGui::GetCursorScreenPos().y - Sc(4.f)), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto entry = [&](const char* name, const char* date, const char* message) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            RainbowRule(dl, ImVec2(p.x, p.y + Sc(4.f)), ImGui::GetContentRegionAvail().x, static_cast<float>(ImGui::GetTime()));
            ImGui::Dummy(ImVec2(0.f, Sc(10.f)));
            ImGui::TextColored(ImVec4(0.f, 1.f, 1.f, 1.f), "%s", name);
            ImGui::SameLine();
            ImGui::TextDisabled("- %s", date);
            ImGui::TextWrapped("%s", message);
        };
        for (auto it = s.guestbook.rbegin(); it != s.guestbook.rend(); ++it)
            entry(it->name.c_str(), it->date.c_str(), it->message.c_str());
        static constexpr const char* kOld[][3] = {
            { "WebMaster Steve",   "1998-11-02", "Great scanlines. Adding you to my webring!" },
            { "~*~sk8rgrl~*~",     "1998-07-19", "OMG the CRT effect is SO real. bookmarked!!!!" },
            { "xXDarkAngelXx",     "1998-03-14", "cool site!!! check out my page, it has midi music" },
            { "dad",               "1998-01-05", "How do I turn this off" },
            { "Anonymous",         "1997-12-31", "first" },
        };
        for (const auto& e : kOld) entry(e[0], e[1], e[2]);
        ImGui::EndChild();
    }

private:
    static ImVec2 FooterButton() { return ImVec2(Sc(96.f), Sc(30.f)); }

    // The page area of the window (inside its sunken border).
    static ImVec2 PageMin(ImVec2 a) { return ImVec2(a.x + Sc(10.f), a.y + Sc(126.f)); }
    static ImVec2 PageMax(ImVec2 b) { return ImVec2(b.x - Sc(10.f), b.y - Sc(30.f)); }

    // Toolbar icons: back, forward, reload, home, search, stop.
    static void ToolIcon(ImDrawList* dl, int i, ImVec2 c, bool enabled) {
        const ImU32 green = enabled ? IM_COL32(0, 150, 0, 255) : kDark;
        const float k = Sc(8.f);
        switch (i) {
        case 0: case 1: {   // arrows
            const float d = i == 0 ? -1.f : 1.f;
            dl->AddTriangleFilled(ImVec2(c.x + d * k, c.y), ImVec2(c.x, c.y - k), ImVec2(c.x, c.y + k), green);
            dl->AddRectFilled(ImVec2(std::min(c.x, c.x - d * k), c.y - k * 0.4f), ImVec2(std::max(c.x, c.x - d * k), c.y + k * 0.4f), green);
            break;
        }
        case 2:   // reload: an arc with an arrowhead
            dl->PathArcTo(c, k * 0.8f, 0.6f, 5.6f, 16);
            dl->PathStroke(IM_COL32(0, 0, 160, 255), 0, Sc(2.2f));
            dl->AddTriangleFilled(ImVec2(c.x + k * 0.95f, c.y - k * 0.2f), ImVec2(c.x + k * 0.35f, c.y + k * 0.15f),
                                  ImVec2(c.x + k * 1.05f, c.y + k * 0.55f), IM_COL32(0, 0, 160, 255));
            break;
        case 3:   // home: a house
            dl->AddTriangleFilled(ImVec2(c.x, c.y - k), ImVec2(c.x + k, c.y - k * 0.1f), ImVec2(c.x - k, c.y - k * 0.1f), IM_COL32(200, 0, 0, 255));
            dl->AddRectFilled(ImVec2(c.x - k * 0.7f, c.y - k * 0.1f), ImVec2(c.x + k * 0.7f, c.y + k), IM_COL32(240, 220, 120, 255));
            dl->AddRectFilled(ImVec2(c.x - k * 0.2f, c.y + k * 0.3f), ImVec2(c.x + k * 0.2f, c.y + k), IM_COL32(120, 70, 20, 255));
            break;
        case 4:   // search: a magnifier
            dl->AddCircle(ImVec2(c.x - k * 0.2f, c.y - k * 0.2f), k * 0.6f, kDark, 16, Sc(2.f));
            dl->AddLine(ImVec2(c.x + k * 0.25f, c.y + k * 0.25f), ImVec2(c.x + k, c.y + k), kDark, Sc(2.5f));
            break;
        default: {   // stop: a red octagon
            ImVec2 oct[8];
            for (int j = 0; j < 8; ++j) {
                const float ang = 0.3926991f + j * 0.7853982f;
                oct[j] = ImVec2(c.x + cosf(ang) * k, c.y + sinf(ang) * k);
            }
            dl->AddConvexPolyFilled(oct, 8, enabled ? IM_COL32(210, 0, 0, 255) : kDark);
            break;
        }
        }
    }

    // A yellow diamond road sign with a little worker digging, and a
    // spinning orange beacon on top.
    static void UnderConstruction(ImDrawList* dl, ImVec2 c, float t) {
        const float r = Sc(20.f);
        const ImVec2 dia[4] = { ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y) };
        dl->AddConvexPolyFilled(dia, 4, IM_COL32(255, 200, 0, 255));
        dl->AddPolyline(dia, 4, kBlack, ImDrawFlags_Closed, Sc(2.f));
        // The worker: head, body, legs, and a shovel going up and down.
        const float dig = sinf(t * 6.f) * Sc(2.f);
        dl->AddCircleFilled(ImVec2(c.x - Sc(3.f), c.y - Sc(8.f)), Sc(2.5f), kBlack, 10);
        dl->AddLine(ImVec2(c.x - Sc(3.f), c.y - Sc(6.f)), ImVec2(c.x - Sc(1.f), c.y + Sc(3.f)), kBlack, Sc(2.f));
        dl->AddLine(ImVec2(c.x - Sc(1.f), c.y + Sc(3.f)), ImVec2(c.x - Sc(6.f), c.y + Sc(10.f)), kBlack, Sc(2.f));
        dl->AddLine(ImVec2(c.x - Sc(1.f), c.y + Sc(3.f)), ImVec2(c.x + Sc(3.f), c.y + Sc(10.f)), kBlack, Sc(2.f));
        dl->AddLine(ImVec2(c.x - Sc(2.f), c.y - Sc(3.f)), ImVec2(c.x + Sc(8.f), c.y + Sc(4.f) + dig), kBlack, Sc(1.6f));
        dl->AddTriangleFilled(ImVec2(c.x + Sc(7.f), c.y + Sc(3.f) + dig), ImVec2(c.x + Sc(12.f), c.y + Sc(7.f) + dig),
                              ImVec2(c.x + Sc(8.f), c.y + Sc(9.f) + dig), kBlack);
        // Beacon.
        const ImVec2 bc(c.x, c.y - r - Sc(5.f));
        dl->AddRectFilled(ImVec2(bc.x - Sc(5.f), bc.y + Sc(1.f)), ImVec2(bc.x + Sc(5.f), bc.y + Sc(4.f)), kBlack);
        dl->AddCircleFilled(bc, Sc(4.f), IM_COL32(255, 120, 0, 255), 12);
        const float ang = t * 8.f;
        dl->AddTriangleFilled(bc, ImVec2(bc.x + cosf(ang - 0.3f) * Sc(16.f), bc.y + sinf(ang - 0.3f) * Sc(5.f)),
                              ImVec2(bc.x + cosf(ang + 0.3f) * Sc(16.f), bc.y + sinf(ang + 0.3f) * Sc(5.f)), IM_COL32(255, 160, 0, 120));
    }

    // An open guestbook with a quill; its caption flashes when hovered.
    static void Book(ImDrawList* dl, ImVec2 c, bool hovered, float t) {
        const float w = Sc(22.f), h = Sc(16.f);
        dl->AddRectFilled(ImVec2(c.x - w - Sc(2.f), c.y - h + Sc(2.f)), ImVec2(c.x + w + Sc(2.f), c.y + h + Sc(2.f)), IM_COL32(120, 40, 20, 255), Sc(2.f));
        for (int side = -1; side <= 1; side += 2) {
            const ImVec2 q0(side < 0 ? c.x - w : c.x + Sc(1.f), c.y - h), q1(side < 0 ? c.x - Sc(1.f) : c.x + w, c.y + h);
            dl->AddRectFilled(q0, q1, IM_COL32(250, 246, 226, 255));
            for (int l = 0; l < 4; ++l)
                dl->AddLine(ImVec2(q0.x + Sc(3.f), q0.y + Sc(6.f) + l * Sc(6.f)), ImVec2(q1.x - Sc(3.f), q0.y + Sc(6.f) + l * Sc(6.f)),
                            IM_COL32(120, 120, 160, 200), Sc(1.f));
        }
        dl->AddLine(ImVec2(c.x + Sc(6.f), c.y + Sc(10.f)), ImVec2(c.x + Sc(20.f), c.y - Sc(18.f)), kWhite, Sc(2.f));   // quill
        const char*  cap = "Guestbook";
        const ImVec2 ts  = TextSize(ChromeFont(), Sc(12.f), cap);
        dl->AddText(ChromeFont(), Sc(12.f), ImVec2(c.x - ts.x * 0.5f, c.y + h + Sc(4.f)),
                    hovered ? Rainbow(t * 2.f) : kLink, cap);
    }

    bool        guestbook_   = false;
    unsigned    visited_     = 0;      // a bit per section page visited (this session)
    float       countedOpen_ = -1.f;
    int         hits_        = 0;
    std::string hoverUrl_;
    char        name_[33]     = {};
    char        message_[281] = {};
};

} // namespace web

// ---------------------------------------------------------------------------
// Quit confirmation dialog — drawn in the current style's colours and font.
// ---------------------------------------------------------------------------
static ImFont* MenuFont(int styleId) {
    switch (styleId) {
    case 3:  return StyleFonts::chakraPetch;
    case 6:  return StyleFonts::cherryBomb;
    case 7:  return StyleFonts::cinzel;
    case 8:  return StyleFonts::bedstead;
    case 9:  return StyleFonts::shareTech;
    case 10: return StyleFonts::pressStart;
    case 11: return StyleFonts::shortStack;
    case 12: return StyleFonts::times;
    default: return nullptr;   // the default font
    }
}

// The patch notes (CHANGELOG.md, compiled in), in the current menu style's
// colours and font.  "## " lines are version headings, "- " lines bullets.
static void DrawPatchNotes(int styleId) {
    static constexpr const char* kPopupId = "##patchnotes";
    if (s_patchNotesRequested) {
        ImGui::OpenPopup(kPopupId);
        s_patchNotesRequested = false;
    }

    ImFont* font = MenuFont(styleId);
    if (font) ImGui::PushFont(font, font->LegacySize);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(Sc(560.f), display.x - 40.f), std::min(Sc(480.f), display.y - 40.f)),
                             ImGuiCond_Always);
    if (ImGui::BeginPopupModal(kPopupId, nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        const ImVec4 accent = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.3f);
        ImGui::TextColored(accent, "What's new in SeeRT Overlay");
        ImGui::PopFont();
        ImGui::Separator();

        const float closeH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::BeginChild("##notes", ImVec2(0.f, -closeH), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        std::istringstream in(kPatchNotes);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line.rfind("# ", 0) == 0) continue;   // blank, or the file's title
            if (line.rfind("## ", 0) == 0) {
                ImGui::Spacing();
                ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.15f);
                const bool current = line.compare(3, std::string::npos, UpdateCheck::CurrentVersion()) == 0;
                ImGui::TextColored(accent, "Version %s%s", line.c_str() + 3, current ? "  (you have this one)" : "");
                ImGui::PopFont();
            } else if (line.rfind("- ", 0) == 0) {
                ImGui::Bullet();
                ImGui::TextWrapped("%s", line.c_str() + 2);
            } else {
                ImGui::TextWrapped("%s", line.c_str());
            }
        }
        ImGui::EndChild();

        if (ImGui::Button("Close", ImVec2(Sc(120.f), 0.f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (font) ImGui::PopFont();
}

void SettingsPanel::ShowPatchNotes() { s_patchNotesRequested = true; }

// "vX is ready to install" — Restart & update, What's new, or Later.
static void DrawInstallPrompt(int styleId, SettingsPanel::DrawResult& result) {
    static constexpr const char* kPopupId = "##installprompt";
    if (s_installPromptRequested) {
        ImGui::OpenPopup(kPopupId);
        s_installPromptRequested = false;
    }

    ImFont* font = MenuFont(styleId);
    if (font) ImGui::PushFont(font, font->LegacySize);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kPopupId, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        const Updater::Progress u = Updater::Current();
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.2f);
        ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_CheckMark], "SeeRT Overlay v%s is ready to install", u.version.c_str());
        ImGui::PopFont();
        ImGui::TextUnformatted("Install it now?  The app restarts; your settings and presets are kept.");
        ImGui::Spacing();
        if (ImGui::Button("Restart & update")) {
            result.restartToUpdate = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("What's new")) result.openUpdatePage = true;
        ImGui::SameLine();
        if (ImGui::Button("Later") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (font) ImGui::PopFont();
}

void SettingsPanel::ShowInstallPrompt() { s_installPromptRequested = true; }

static void DrawQuitConfirm(int styleId) {
    static constexpr const char* kPopupId = "##quitconfirm";
    if (s_quitRequested) {
        ImGui::OpenPopup(kPopupId);
        s_quitRequested = false;
    }

    ImFont* font = MenuFont(styleId);
    if (font) ImGui::PushFont(font, font->LegacySize);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kPopupId, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("Quit SeeRT Overlay?");
        ImGui::TextDisabled("The CRT effect will switch off.");
        ImGui::Spacing();

        const ImGuiStyle& st = ImGui::GetStyle();
        const float bw = std::max(ImGui::CalcTextSize("Cancel").x, ImGui::CalcTextSize("Quit").x)
                       + st.FramePadding.x * 2.f + Sc(24.f);
        ImGui::PushStyleColor(ImGuiCol_Button,        kRed);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRedH);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kRedA);
        const bool quit = ImGui::Button("Quit", ImVec2(bw, 0.f));
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(bw, 0.f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        if (quit) {
            ImGui::CloseCurrentPopup();
            PostQuitMessage(0);
        }
        ImGui::EndPopup();
    }
    if (font) ImGui::PopFont();
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
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;   // a game may turn it off below
    if (!s.menuVisible) return {};
    if (!StyleName(s.menuStyle)) s.menuStyle = CRTSettings{}.menuStyle;   // unknown or retired id
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
        case 11:
            result = DrawPagedMenu(s_complabStyle, "##complab", s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices);
            break;
        case 12: {
            static web::Style webStyle;
            result = DrawPagedMenu(webStyle, "##web", s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices);
            break;
        }
        default: result = DrawOSD       (s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices); break;
    }
    DrawFridgeMagnet(s);
    DrawQuitConfirm(s.menuStyle);
    DrawPatchNotes(s.menuStyle);
    DrawInstallPrompt(s.menuStyle, result);
    result.saveDrawing = s_saveDrawingRequested;
    s_saveDrawingRequested = false;

    style = saved;
    s_sizeSliderHeld = s_sizeSliderHeldNext;
    s_cursorInfo = nullptr;
    return result;
}
