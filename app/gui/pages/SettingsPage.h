#pragma once
// Settings page behind the gear button (docs/GUI.md §50-§53, §51, §52, §55):
//   AUDIO         output device (+ current rate / buffer)
//   STARTUP       launch at system startup, start minimized, automatically start masking (requires a
//                 remembered output device), restore previous configuration, remember output device
//   VOICE LIBRARY status, Manage Library dialog (Add Audio / Scan / Rebuild Analysis), import wizard
//   APPEARANCE    UI scale, Advanced mode explanation
//   LOGGING       level, path redaction toggles, open log folder
//   DIAGNOSTICS   save a diagnostics zip, fallback policy Strict / Safe / Continuous
// Preferences persist through model/Prefs (settings.json "prefs"); the fallback policy is part of the
// preset (reliability.fallbackPolicy).
#include <vector>

#include "Widgets.h"
#include "dialogs/HelpParts.h"
#include "dialogs/LibraryDialogs.h"
#include "model/Prefs.h"
#include "pages/Page.h"

namespace bf::gui {

class SettingsPage final : public Page {
public:
    explicit SettingsPage(PageContext& c);

    int layoutPage(int width) override;
    void refreshFromState() override;
    void refreshStatus(const EngineStatus& s) override;

    // Actions (also used by the tests).
    bool saveDiagnosticsZip(const juce::File& target, juce::String* error = nullptr);
    void openLogFolder();
    void openManageLibrary();
    void refreshLibrary();
    juce::File logDirectory() const;

    juce::ToggleButton& launchToggle() { return launch_; }
    juce::ToggleButton& minimizedToggle() { return minimized_; }
    juce::ToggleButton& autoStartToggle() { return autoStart_; }
    juce::ToggleButton& restoreToggle() { return restore_; }
    juce::ToggleButton& rememberToggle() { return remember_; }
    juce::ToggleButton& redactLogsToggle() { return redactLogs_; }
    juce::ToggleButton& redactExportsToggle() { return redactExports_; }
    juce::ComboBox& logLevelBox() { return logLevel_; }
    juce::ComboBox& scaleBox() { return scale_; }
    juce::ComboBox& deviceBox() { return device_; }
    ChoiceGroup& fallbackGroup() { return fallback_; }
    juce::Label& startupNote() { return startupNote_; }
    juce::Label& libraryStatus() { return libStatus_; }
    juce::Label& libraryTalkers() { return libTalkers_; }
    juce::Label& librarySpeech() { return libSpeech_; }
    juce::TextButton& manageButton() { return manage_; }
    juce::TextButton& saveDiagnosticsButton() { return saveDiag_; }
    juce::TextButton& openLogsButton() { return openLogs_; }

private:
    struct Row {
        juce::Component* c = nullptr;
        HelpButton* help = nullptr;
        int h = 0;
        bool header = false;
    };
    void addHeader(SectionLabel& l);
    void addRow(juce::Component& c, int h, HelpButton* help = nullptr);
    void toggle(juce::ToggleButton& t, const juce::String& tip, std::function<void(bool)> fn);
    void refreshPrefs();
    void refreshDevices();
    void chooseDiagnosticsFile();
    bool forcedFallback();

    std::vector<Row> rows_;
    // Audio
    SectionLabel hAudio_{"AUDIO", true}, hStartup_{"STARTUP", true}, hLibrary_{"VOICE LIBRARY", true}, hLook_{"APPEARANCE", true},
        hLog_{"LOGGING", true}, hDiag_{"DIAGNOSTICS", true};
    juce::Label deviceCaption_, deviceInfo_;
    juce::ComboBox device_;
    juce::TextButton refreshDevices_{"Refresh Devices"};
    HelpButton deviceHelp_{"The speakers or audio interface that plays the masking sound. BabbleForge never switches "
                           "devices by itself."};
    std::vector<std::string> deviceIds_;
    // Startup
    juce::ToggleButton launch_{"Launch at system startup"}, minimized_{"Start minimized"},
        autoStart_{"Automatically start masking"}, restore_{"Restore previous configuration"},
        remember_{"Remember output device"};
    HelpButton autoHelp_{"Masking starts as soon as BabbleForge opens. This needs a remembered output device so that the "
                         "sound never plays on the wrong speakers."};
    HelpButton rememberHelp_{"Keeps using the same output device the next time BabbleForge starts."};
    juce::Label startupNote_;
    // Library
    juce::Label libStatus_, libTalkers_, libSpeech_, libPath_;
    juce::TextButton manage_{"Manage Library"};
    HelpButton libHelp_{"The recordings that the voice masking is made from. They are analysed on this computer and never "
                        "leave it."};
    // Appearance
    juce::Label scaleCaption_;
    juce::ComboBox scale_;
    HelpButton scaleHelp_{"Makes everything in the window larger or smaller."};
    juce::TextButton showIntro_{"Show Advanced mode explanation again"};
    juce::ToggleButton tooltips_{"Show tooltips"};
    // Logging
    juce::Label levelCaption_;
    juce::ComboBox logLevel_;
    HelpButton levelHelp_{"How much detail is written to the log files. Info is enough for everyday use; Debug helps when "
                          "support asks for more."};
    juce::ToggleButton redactLogs_{"Hide file paths in log files"}, redactExports_{"Hide file paths and user names in exported diagnostics"};
    HelpButton redactLogsHelp_{"Replaces folder names in the log files with short codes. Takes effect the next time BabbleForge starts."};
    HelpButton redactExportsHelp_{"Removes your folder and user names before a diagnostics file is shared."};
    juce::TextButton openLogs_{"Open Log Folder"};
    // Diagnostics
    juce::TextButton saveDiag_{"Save Diagnostics..."};
    juce::Label diagNote_, saveStatus_;
    ChoiceGroup fallback_{"Fallback policy", {"Strict", "Safe", "Continuous"}};
    HelpButton fallbackHelp_{"What happens if the voice library has problems while masking. Strict stops with an error. Safe "
                             "switches to steady masking noise. Continuous keeps the voices going and replaces what is missing."};
    std::unique_ptr<juce::FileChooser> chooser_;
    bool building_ = true;
    std::string lastDeviceList_;
};

}  // namespace bf::gui
