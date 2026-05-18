#include "SettingsPanel.h"
#include "../settings/Presets.h"
#include "StyleFonts.h"
#include <imgui.h>
#include <algorithm> // std::clamp
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
    "CRT OSD", "Simple", "N64", "Camcorder", "DOS Terminal", "VCR"
};
static constexpr int kStyleCount = 6;

static const char* kSectionNames[] = {
    "CAPTURE SOURCE", "PRESETS", "GEOMETRY", "SCANLINES",
    "GLOW", "DIFFUSION", "VIGNETTE", "FLICKER", "STATIC",
    "SHADOW MASK", "COLOUR", "CURSOR", "STREAMING"
};
static constexpr int kSectionCount = 13;

// Show a tooltip for the most recently drawn item.
static void Tip(const char* text) {
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", text);
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
    ImGui::SetNextWindowSize(ImVec2(300.f, 0.f), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("##savepresetdlg", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(kCyan, "Save new preset");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextUnformatted("Name:");
        static char s_nameBuf[128] = {};
        ImGui::SetNextItemWidth(-1.f);
        const bool commit = ImGui::InputText("##newprename", s_nameBuf, sizeof(s_nameBuf),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        const float hw = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
        const bool doSave   = (commit || ImGui::Button("Save",   ImVec2(hw,   0.f))) && s_nameBuf[0] != '\0';
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
            ImGui::SameLine();
            if ((commit || ImGui::SmallButton("OK")) && s_renameBuf[0] != '\0') {
                result.manageOp   = ManageOp::Rename;
                result.manageIdx  = i;
                result.manageName = s_renameBuf;
                s_renameIdx = -1;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel")) s_renameIdx = -1;
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
    ImGui::SetNextWindowSize(ImVec2(460.f, 0.f), ImGuiCond_Always);
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

    // Style selector
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo("##menustyle", kStyleNames[s.menuStyle])) {
        for (int i = 0; i < kStyleCount; ++i)
            if (ImGui::Selectable(kStyleNames[i], s.menuStyle == i))
                s.menuStyle = i;
        ImGui::EndCombo();
    }
    Tip("Choose the menu style.");

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

    Sec("SCANLINES");
    ImGui::SliderFloat("Intensity",     &s.scanlineIntensity, 0.0f,  1.0f,   "%.2f");
    Tip("Darkness of the gaps between horizontal scanlines — 0 is flat, 1 is maximum dark lines.");
    ImGui::SliderFloat("Count",         &s.scanlineCount,     200.f, 1200.f, "%.0f");
    Tip("Number of scanline pairs across the screen — higher values match higher-resolution CRT displays.");
    ImGui::SliderFloat("Hum Intensity", &s.humBarIntensity,   0.0f,  1.0f,   "%.2f");
    Tip("Strength of rolling brightness bands simulating mains-frequency (50/60 Hz) interference.");
    ImGui::SliderFloat("Hum Speed",     &s.humBarSpeed,       0.0f,  2.0f,   "%.2f");
    Tip("Speed at which the hum bars scroll upward, in screen heights per second.");
    ImGui::SliderFloat("Hum Bands",     &s.humBarFrequency,   0.5f,  20.0f,  "%.1f");
    Tip("Number of hum bands visible on screen — higher values give tighter, more frequent banding.");

    Sec("GLOW");
    ImGui::SliderFloat("Strength##g",   &s.glowStrength,      0.0f,  2.0f,   "%.2f");
    Tip("Phosphor bloom — bright edges bleed into adjacent pixels, mimicking CRT phosphor halation.");

    Sec("DIFFUSION");
    ImGui::SliderFloat("Strength##d",   &s.diffuseStrength,   0.0f,  2.0f,   "%.2f");
    Tip("Brightness-weighted bloom — only pixels above the threshold radiate light outward.");
    ImGui::SliderFloat("Threshold##d",  &s.diffuseThreshold,  0.0f,  1.0f,   "%.2f");
    Tip("Luminance cutoff — pixels dimmer than this value do not contribute to the diffusion glow.");

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
        static const char* kMaskTypes[] = { "Aperture Grille", "Shadow Mask", "Slot Mask" };
        int t = (int)s.shadowMaskType;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::Combo("##masktype", &t, kMaskTypes, 3))
            s.shadowMaskType = (float)t;
        Tip("Aperture Grille — vertical RGB stripes (Trinitron-style).\n"
            "Shadow Mask — offset dot triads.\n"
            "Slot Mask — stripes with a horizontal black gap every other row.");
    }
    ImGui::SliderFloat("Intensity##sm", &s.shadowMaskIntensity, 0.0f, 1.0f, "%.2f");
    Tip("How strongly the sub-pixel pattern modulates the image — 0 is off, 1 is full mask.");
    ImGui::SliderFloat("Scale##sm",     &s.shadowMaskScale,     0.5f, 4.0f, "%.1f px");
    Tip("Width of each phosphor cell in screen pixels — try 1.5-2.0 to make the pattern clearly visible.");

    Sec("COLOUR");
    ImGui::SliderFloat("Brightness",   &s.brightness,       0.5f,  2.0f,   "%.2f");
    Tip("Overall image brightness multiplier — 1.0 is unchanged.");
    ImGui::SliderFloat("Contrast",     &s.contrast,         0.5f,  2.0f,   "%.2f");
    Tip("Contrast strength pivoted around mid-grey — above 1.0 increases light/dark separation.");
    ImGui::SliderFloat("Saturation",   &s.saturation,       0.0f,  2.0f,   "%.2f");
    Tip("Colour saturation — 0 is greyscale, 1.0 is original, 2.0 is double-saturated.");
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
    const float panelW = 500.f;
    ImVec2 display = ImGui::GetIO().DisplaySize;
    const float maxH = display.y * 0.70f;
    ImGui::SetNextWindowPos(
        ImVec2((display.x - panelW) * 0.5f, display.y * 0.15f), ImGuiCond_Always);
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
        ImGui::SetNextWindowSize(ImVec2(460.f, 0.f), ImGuiCond_Always);
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
        break;
    case 3:
        ImGui::SliderFloat("Intensity",     &s.scanlineIntensity, 0.0f,    1.0f,   "%.2f");
        Tip("Darkness of scanline gaps.");
        ImGui::SliderFloat("Count",         &s.scanlineCount,     200.f, 1200.f,   "%.0f");
        Tip("Number of scanline pairs across the screen.");
        ImGui::SliderFloat("Hum Intensity", &s.humBarIntensity,   0.0f,    1.0f,   "%.2f");
        Tip("Rolling brightness band strength.");
        ImGui::SliderFloat("Hum Speed",     &s.humBarSpeed,       0.0f,    2.0f,   "%.2f");
        Tip("Hum bar scroll speed.");
        ImGui::SliderFloat("Hum Bands",     &s.humBarFrequency,   0.5f,   20.0f,   "%.1f");
        Tip("Number of hum bands visible on screen.");
        break;
    case 4:
        ImGui::SliderFloat("Strength##g", &s.glowStrength, 0.0f, 2.0f, "%.2f");
        Tip("Phosphor bloom strength.");
        break;
    case 5:
        ImGui::SliderFloat("Strength##d",  &s.diffuseStrength,  0.0f, 2.0f, "%.2f");
        Tip("Brightness-weighted bloom strength.");
        ImGui::SliderFloat("Threshold##d", &s.diffuseThreshold, 0.0f, 1.0f, "%.2f");
        Tip("Luminance cutoff — pixels below this don't glow.");
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
        static const char* kMaskTypes[] = { "Aperture Grille", "Shadow Mask", "Slot Mask" };
        int t = (int)s.shadowMaskType;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::Combo("##masktype", &t, kMaskTypes, 3)) s.shadowMaskType = (float)t;
        Tip("Aperture Grille / Shadow Mask / Slot Mask");
        ImGui::SliderFloat("Intensity##sm", &s.shadowMaskIntensity, 0.0f, 1.0f, "%.2f");
        Tip("Sub-pixel pattern strength.");
        ImGui::SliderFloat("Scale##sm",     &s.shadowMaskScale,     0.5f, 4.0f, "%.1f px");
        Tip("Phosphor cell width in pixels.");
        break;
    }
    case 10: {
        ImGui::SliderFloat("Brightness", &s.brightness, 0.5f, 2.0f, "%.2f");
        Tip("Image brightness multiplier.");
        ImGui::SliderFloat("Contrast",   &s.contrast,   0.5f, 2.0f, "%.2f");
        Tip("Light/dark separation.");
        ImGui::SliderFloat("Saturation", &s.saturation, 0.0f, 2.0f, "%.2f");
        Tip("Colour saturation.");
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

    const float panelW = 560.f;
    ImVec2 display = ImGui::GetIO().DisplaySize;
    const float maxH = display.y * 0.78f;

    ImGui::SetNextWindowPos(
        ImVec2((display.x - panelW) * 0.5f, display.y * 0.11f), ImGuiCond_Always);
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

    // Style selector
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::BeginCombo("##menustyle", kStyleNames[s.menuStyle])) {
        for (int i = 0; i < kStyleCount; ++i)
            if (ImGui::Selectable(kStyleNames[i], s.menuStyle == i))
                s.menuStyle = i;
        ImGui::EndCombo();
    }
    Tip("Choose the menu style.");
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
            ImGui::Indent(20.f);
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.58f);
            DrawSectionContent(i, s, monitors, selectedMon, secondaryMons,
                               streamOpen, presets, audioDevices, result);
            ImGui::PopItemWidth();
            ImGui::Unindent(20.f);
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
    const float kBarH   = 50.f;
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.f, 10.f));
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.f, 10.f));
    ImGui::Begin("##cam_ftr", nullptr, barFlags);
    {
        ImGui::TextColored({ 0.55f, 0.55f, 0.55f, 1.f },
                           "\u25c4\u25ba Navigate  \u25cf Select  MENU Close");
        ImGui::SameLine();
        const float btnW = 80.f;
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
    const float  panelW   = twoPanel ? 720.f : 460.f;
    const float  panelH   = display.y * 0.35f;
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

    // Style selector — always visible in the panel
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::BeginCombo("##menustyle", kStyleNames[s.menuStyle])) {
        for (int i = 0; i < kStyleCount; ++i)
            if (ImGui::Selectable(kStyleNames[i], s.menuStyle == i))
                s.menuStyle = i;
        ImGui::EndCombo();
    }
    Tip("Choose the menu style.");
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
        const float leftW  = 185.f;
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

    const float panelW = 500.f;
    ImVec2 display = ImGui::GetIO().DisplaySize;
    const float maxH = display.y * 0.42f;  // 60% of the old 70%
    ImGui::SetNextWindowPos(
        ImVec2((display.x - panelW) * 0.5f, display.y * 0.15f), ImGuiCond_Always);
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
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::BeginCombo("##menustyle", kStyleNames[s.menuStyle])) {
            for (int i = 0; i < kStyleCount; ++i)
                if (ImGui::Selectable(kStyleNames[i], s.menuStyle == i))
                    s.menuStyle = i;
            ImGui::EndCombo();
        }
        Tip("Choose the menu style.");
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

    const float panelW = 580.f;
    ImVec2 display = ImGui::GetIO().DisplaySize;
    const float maxH = display.y * 0.70f;
    ImGui::SetNextWindowPos(
        ImVec2((display.x - panelW) * 0.5f, display.y * 0.15f), ImGuiCond_Always);
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

    // Style selector
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::BeginCombo("##menustyle", kStyleNames[s.menuStyle])) {
        for (int i = 0; i < kStyleCount; ++i)
            if (ImGui::Selectable(kStyleNames[i], s.menuStyle == i))
                s.menuStyle = i;
        ImGui::EndCombo();
    }
    Tip("Choose the menu style.");
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
            ImGui::Indent(14.f);
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.58f);
            DrawSectionContent(i, s, monitors, selectedMon, secondaryMons,
                               streamOpen, presets, audioDevices, result);
            ImGui::PopItemWidth();
            ImGui::Unindent(14.f);
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

    const float panelW = 500.f;
    ImVec2 display = ImGui::GetIO().DisplaySize;
    const float maxH = display.y * 0.70f;
    ImGui::SetNextWindowPos(
        ImVec2((display.x - panelW) * 0.5f, display.y * 0.15f),
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

    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::BeginCombo("##menustyle", kStyleNames[s.menuStyle])) {
        for (int i = 0; i < kStyleCount; ++i) {
            if (ImGui::Selectable(kStyleNames[i], s.menuStyle == i))
                s.menuStyle = i;
        }
        ImGui::EndCombo();
    }
    Tip("Choose the menu style.");
    ImGui::Spacing();
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
    }
    if (ImGui::CollapsingHeader("Scanlines", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Intensity",     &s.scanlineIntensity, 0.0f,  1.0f,   "%.2f");
        Tip("Darkness of the gaps between horizontal scanlines — 0 is flat, 1 is maximum dark lines.");
        ImGui::SliderFloat("Count",         &s.scanlineCount,     200.f, 1200.f, "%.0f");
        Tip("Number of scanline pairs across the screen — higher values match higher-resolution CRT displays.");
        ImGui::SliderFloat("Hum Intensity", &s.humBarIntensity,   0.0f,  1.0f,   "%.2f");
        Tip("Strength of rolling brightness bands simulating mains-frequency (50/60 Hz) interference.");
        ImGui::SliderFloat("Hum Speed",     &s.humBarSpeed,       0.0f,  2.0f,   "%.2f");
        Tip("Speed at which the hum bars scroll upward, in screen heights per second.");
        ImGui::SliderFloat("Hum Bands",     &s.humBarFrequency,   0.5f, 20.0f,   "%.1f");
        Tip("Number of hum bands visible on screen — higher values give tighter, more frequent banding.");
    }
    if (ImGui::CollapsingHeader("Glow", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Strength##g",  &s.glowStrength,     0.0f, 2.0f, "%.2f");
        Tip("Phosphor bloom — bright edges bleed into adjacent pixels, mimicking CRT phosphor halation.");
    }
    if (ImGui::CollapsingHeader("Diffusion", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Strength##d",  &s.diffuseStrength,  0.0f, 2.0f, "%.2f");
        Tip("Brightness-weighted bloom — only pixels above the threshold radiate light outward, creating a glowing look.");
        ImGui::SliderFloat("Threshold##d", &s.diffuseThreshold, 0.0f, 1.0f, "%.2f");
        Tip("Luminance cutoff — pixels dimmer than this value do not contribute to the diffusion glow.");
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
        static const char* kMaskTypes[] = { "Aperture Grille", "Shadow Mask", "Slot Mask" };
        int t = (int)s.shadowMaskType;
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::Combo("Pattern##sm", &t, kMaskTypes, 3))
            s.shadowMaskType = (float)t;
        Tip("Aperture Grille — vertical RGB stripes (Trinitron-style).\n"
            "Shadow Mask — offset dot triads.\n"
            "Slot Mask — stripes with a horizontal black gap every other row.");
        ImGui::SliderFloat("Intensity##sm", &s.shadowMaskIntensity, 0.0f, 1.0f, "%.2f");
        Tip("How strongly the sub-pixel pattern modulates the image — 0 is off, 1 is full mask.");
        ImGui::SliderFloat("Scale##sm",     &s.shadowMaskScale,     0.5f, 4.0f, "%.1f px");
        Tip("Width of each phosphor cell in screen pixels — try 1.5-2.0 to make the pattern clearly visible.");
    }
    if (ImGui::CollapsingHeader("Colour", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Brightness",  &s.brightness, 0.5f, 2.0f, "%.2f");
        Tip("Overall image brightness multiplier — 1.0 is unchanged.");
        ImGui::SliderFloat("Contrast",    &s.contrast,   0.5f, 2.0f, "%.2f");
        Tip("Contrast strength pivoted around mid-grey — above 1.0 increases light/dark separation.");
        ImGui::SliderFloat("Saturation",  &s.saturation, 0.0f, 2.0f, "%.2f");
        Tip("Colour saturation — 0 is greyscale, 1.0 is original, 2.0 is double-saturated.");
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
// Public entry point
// ---------------------------------------------------------------------------
SettingsPanel::DrawResult SettingsPanel::Draw(CRTSettings& s,
                                               const Monitors& monitors,
                                               HMONITOR selectedMon,
                                               const std::vector<HMONITOR>& secondaryMons,
                                               bool streamOpen,
                                               const std::vector<PresetItem>& presets,
                                               const std::vector<AudioDevEntry>& audioDevices) {
    if (!s.menuVisible) return {};
    switch (s.menuStyle) {
        case 1:  return DrawDefault   (s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices);
        case 2:  return DrawN64       (s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices);
        case 3:  return DrawCamcorder (s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices);
        case 4:  return DrawDOS       (s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices);
        case 5:  return DrawVCR       (s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices);
        default: return DrawOSD       (s, monitors, selectedMon, secondaryMons, streamOpen, presets, audioDevices);
    }
}
