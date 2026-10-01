#pragma once
// PresetSession: Factory / Modified / User Preset state of the current configuration
// (docs/GUI.md §46-§48, §57; PRESETS.md §2).
//
//  - Recommended (factory) preset = AppState::factoryPreset(area, strategy).
//  - "Modified": any effective macro (Character, Voice Amount, Voice Variety, Clear Voice
//    Reduction, Mask Mix) or advanced field differs from the recommended configuration.
//    Strength and the output configuration are the user's level / installation and do not
//    make a preset "Modified"; Reset to Recommended keeps them.
//  - "User Preset": loaded or saved from user_presets/*.bfpreset and unchanged since.
//  - advancedChanges(): the "ADVANCED SETTINGS MODIFIED" list (GUI §57), e.g.
//    "Talkers 8 → 12".
//
// Message thread only.
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

#include "model/AppState.h"

namespace bf::gui {

enum class PresetOrigin { Factory, Modified, User, UserModified };

struct ChangeEntry {
    juce::String label, from, to;
    bool advanced = true;  // false: a Simple macro (Character, Voice Amount, ...)
};

class PresetSession final {
public:
    PresetSession(AppState& state, std::filesystem::path userPresetDir);

    Preset recommended() const;  // factory preset for the current area / strategy
    PresetOrigin origin() const;
    bool isModified() const;     // differs from the recommended configuration
    std::vector<ChangeEntry> changes() const;          // all differences vs recommended
    std::vector<ChangeEntry> advancedChanges() const;  // GUI §57 list
    bool differsSignificantly() const;                 // GUI §48 advisory

    juce::String areaName() const;
    juce::String strategyName() const;
    juce::String title() const;      // "Open Office / Balanced" or the user preset name
    juce::String stateText() const;  // "Recommended" | "Modified" | "User Preset" | "User Preset · Modified"

    void resetToRecommended();  // undoable; keeps Strength and outputs (PRESETS §2)

    // Save As (GUI §47). Writes <dir>/<sanitized name>.bfpreset atomically. The device is
    // stored only when rememberDevice (PRESETS §6.1).
    bool saveAs(const juce::String& name, bool rememberDevice, const std::string& deviceId, juce::String* error = nullptr);
    bool loadUserPreset(const std::filesystem::path& file, juce::String* error = nullptr);
    std::vector<std::filesystem::path> userPresets() const;
    const std::filesystem::path& userPresetDir() const noexcept { return dir_; }
    std::filesystem::path pathForName(const juce::String& name) const;

private:
    AppState& s_;
    std::filesystem::path dir_;
    std::optional<std::string> userHash_;  // content hash of the loaded / saved user preset
    juce::String userName_;
};

}  // namespace bf::gui
