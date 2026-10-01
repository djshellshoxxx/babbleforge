#pragma once
// GUI preferences stored in settings.json under "prefs" (Startup §51, Logging, Voice Library
// §52). They live in AppSettingsData::extra so that AppSettings itself stays unchanged and
// unknown keys keep being preserved.
#include <functional>
#include <string>

#include "model/AppSettings.h"

namespace bf::gui {

struct Prefs {
    // Startup (GUI §51).
    bool launchAtLogin = false;
    bool startMinimized = false;
    bool autoStart = false;        // automatically start masking; requires rememberDevice
    bool restorePrevious = true;   // restore the previous configuration
    bool rememberDevice = false;   // keep the output device across launches
    // Logging.
    std::string logLevel = "info";  // trace | debug | info | warn | error
    bool redactLogs = false;        // redact paths in the log files
    bool redactExports = true;      // redact paths / user names in diagnostics exports
    // Voice library.
    std::string lastImportDir;      // folder of the last import (Rebuild Analysis)
    // First run (GUI §60).
    bool firstRunDone = false;
};

Prefs loadPrefs(const AppSettingsData& d);
void savePrefs(AppSettings& s, const Prefs& p);
// Convenience: load, mutate, save.
void updatePrefs(AppSettings& s, const std::function<void(Prefs&)>& fn);

// Registers / removes the OS "launch at login" entry (Windows: HKCU Run key; Linux: XDG
// autostart). Returns false when unsupported or the write failed.
bool applyLaunchAtLogin(bool enable, bool startMinimized);

}  // namespace bf::gui
