#pragma once
// Application settings (docs/PRESETS.md §9 settings.json): Simple/Advanced mode, the one-time
// Advanced explanation flag, output device, UI scale, last page. Written atomically
// (config/AtomicFile, temp + rename + .bak) 2 s after the last change, and on destruction.
// An empty path keeps the settings in memory only (tests).
#include <filesystem>
#include <functional>
#include <string>

#include <juce_events/juce_events.h>
#include <nlohmann/json.hpp>

namespace bf::gui {

// UTF-8 safe conversions between juce::String and std::filesystem::path (MSVC: a narrow
// std::string would be interpreted in the ANSI code page).
inline std::filesystem::path toPath(const juce::String& s) {
    const std::string u = s.toStdString();
    return std::filesystem::path(std::u8string(u.begin(), u.end()));
}
inline juce::String fromPath(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return juce::String::fromUTF8(reinterpret_cast<const char*>(u.data()), static_cast<int>(u.size()));
}

struct AppSettingsData {
    bool advanced = false;              // GUI §1: Simple is the default, persisted
    bool advancedIntroShown = false;    // GUI §25: explanation shown once
    std::string deviceId;               // explicit output device (never auto-switched)
    std::string lastPage = "run";
    double uiScale = 1.0;               // desktop scale factor (GUI look & feel)
    std::string theme = "dark";
    bool tooltipsEnabled = true;
    std::string corpusRoot;             // voice library root (Settings page)
    nlohmann::json extra = nlohmann::json::object();  // unknown keys / other pages' settings, preserved
};

class AppSettings final : private juce::Timer {
public:
    explicit AppSettings(std::filesystem::path file = {}, int debounceMs = 2000);
    ~AppSettings() override;

    const AppSettingsData& get() const noexcept { return d_; }
    // Mutates the settings and schedules a debounced atomic write.
    void update(const std::function<void(AppSettingsData&)>& fn);
    bool flush();  // writes now if dirty
    const std::filesystem::path& path() const noexcept { return file_; }

    static nlohmann::json toJson(const AppSettingsData& d);
    static AppSettingsData fromJson(const nlohmann::json& j);
    // Per-user application data directory (…/BabbleForge).
    static std::filesystem::path defaultDirectory();

private:
    void timerCallback() override { flush(); }
    std::filesystem::path file_;
    int debounceMs_;
    AppSettingsData d_;
    bool dirty_ = false;
};

}  // namespace bf::gui
