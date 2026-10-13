#pragma once
#include "../settings/Settings.h"
#include "../settings/Presets.h"
#include "MonitorPicker.h"
#include <string>
#include <vector>

class SettingsPanel {
public:
    struct AudioDevEntry { std::string id; std::string name; };

    // Custom cursor state shown in the Cursor section.
    struct CursorInfo {
        bool        custom = false;  // a custom image is loaded and active
        std::string name;            // configured custom image's file name (UTF-8), if any
        std::string error;           // last load/import failure, empty if none
        int         texW = 0;        // active cursor image size (hotspot slider range)
        int         texH = 0;
    };

    struct DrawResult {
        // Standard actions
        HMONITOR    addMonitor       = nullptr; // add secondary overlay on this monitor
        HMONITOR    removeMonitor    = nullptr; // remove secondary overlay from this monitor
        bool        openStreamPicker = false;
        bool        openStream       = false;
        bool        closeStream      = false;

        // Preset: apply (built-in or user)
        bool        applyPreset      = false;
        CRTSettings presetSettings;

        // Preset: save current settings as a named preset
        bool        savePreset       = false;
        std::string savePresetName;

        // Audio device selection
        bool        selectAudioDevice = false;
        std::string audioDeviceId;

        // Custom cursor image
        bool        pickCustomCursor  = false; // open the file picker
        bool        clearCustomCursor = false; // revert to the built-in cursor

        // Updates
        bool        checkForUpdates   = false; // start an update check now
        bool        openUpdatePage    = false; // open the new release's page

        // Preset management actions from the manage modal (at most one per frame)
        enum class ManageOp { None, MoveUp, MoveDown, Delete, Rename };
        ManageOp    manageOp   = ManageOp::None;
        int         manageIdx  = -1;   // index into the presets vector passed to Draw
        std::string manageName;        // for Rename: the new name
    };

    // Draw the settings overlay.
    //   presets     — ordered flat list (built-ins + user), from App::m_orderedPresets.
    //   selectedMon — currently active capture monitor (highlighted).
    //   streamOpen  — whether the stream window is currently visible.
    // Returns actions to be carried out by the App layer.
    DrawResult Draw(CRTSettings& settings,
                    const std::vector<MonitorPicker::MonitorEntry>& monitors,
                    HMONITOR selectedMon,
                    const std::vector<HMONITOR>& secondaryMons,
                    bool streamOpen,
                    const std::vector<PresetItem>& presets,
                    const std::vector<AudioDevEntry>& audioDevices,
                    const CursorInfo& cursor);
};
