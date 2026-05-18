#pragma once
#include "Settings.h"
#include <string>
#include <vector>

// A named snapshot of CRTSettings that can be applied or saved by the user.
struct UserPreset {
    std::string name;
    CRTSettings settings;
};

// A single entry in the ordered preset list shown to the user.
// Built-in entries reference a factory preset; user entries carry their own settings.
struct PresetItem {
    std::string name;
    bool        isBuiltIn;  // true = factory preset (immutable settings, not deletable/renameable)
    CRTSettings settings;   // ready-to-apply copy — valid for both built-in and user entries
};

namespace Presets {
    // Apply only the visual parameters from src → dst, preserving UI/session
    // fields (menuStyle, menuVisible, toggleX/Y, hiddenSources, time).
    void Apply(CRTSettings& dst, const CRTSettings& src);

    // Returns the list of built-in presets in their natural order.
    struct BuiltInPreset { const char* name; CRTSettings settings; };
    const std::vector<BuiltInPreset>& GetBuiltIn();

    // True if name matches any built-in preset.
    bool IsBuiltIn(const std::string& name);

    // Build the ordered flat display list from a saved name-order and the user preset list.
    // Names in order that match built-ins → built-in entries.
    // Names in order that match user presets → user entries.
    // User presets not mentioned in order are appended at the end.
    std::vector<PresetItem> BuildOrderedList(
        const std::vector<std::string>& order,
        const std::vector<UserPreset>& userPresets);

    // Default order: all built-ins in natural order, then all user presets.
    std::vector<std::string> DefaultOrder(const std::vector<UserPreset>& userPresets);

    // User preset + order I/O — stored as { "order": [...], "presets": [...] }.
    // Backwards-compatible: if the file is an old-format JSON array it is treated as
    // just user presets with no saved order (DefaultOrder will be used by the caller).
    std::string DefaultPath();
    void Load(const std::string& path,
              std::vector<UserPreset>& outPresets,
              std::vector<std::string>& outOrder);
    void Save(const std::string& path,
              const std::vector<UserPreset>& presets,
              const std::vector<std::string>& order);
}
