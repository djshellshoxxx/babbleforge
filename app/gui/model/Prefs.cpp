#include "model/Prefs.h"

#include <juce_core/juce_core.h>

namespace bf::gui {

namespace {
bool getBool(const nlohmann::json& o, const char* k, bool def) {
    return o.is_object() && o.contains(k) && o[k].is_boolean() ? o[k].get<bool>() : def;
}
std::string getStr(const nlohmann::json& o, const char* k, const std::string& def) {
    return o.is_object() && o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : def;
}
}  // namespace

Prefs loadPrefs(const AppSettingsData& d) {
    Prefs p;
    if (!d.extra.is_object() || !d.extra.contains("prefs")) return p;
    const auto& j = d.extra["prefs"];
    p.launchAtLogin = getBool(j, "launchAtLogin", p.launchAtLogin);
    p.startMinimized = getBool(j, "startMinimized", p.startMinimized);
    p.autoStart = getBool(j, "autoStart", p.autoStart);
    p.restorePrevious = getBool(j, "restorePrevious", p.restorePrevious);
    p.rememberDevice = getBool(j, "rememberDevice", p.rememberDevice);
    p.logLevel = getStr(j, "logLevel", p.logLevel);
    if (j.is_object() && j.contains("sampleRate") && j["sampleRate"].is_number()) p.sampleRate = j["sampleRate"].get<double>();
    if (j.is_object() && j.contains("bufferFrames") && j["bufferFrames"].is_number_integer()) p.bufferFrames = j["bufferFrames"].get<int>();
    if (!(p.sampleRate >= 8000.0 && p.sampleRate <= 384000.0)) p.sampleRate = 48000.0;
    if (p.bufferFrames < 16 || p.bufferFrames > 16384) p.bufferFrames = 512;
    p.redactLogs = getBool(j, "redactLogs", p.redactLogs);
    p.redactExports = getBool(j, "redactExports", p.redactExports);
    p.lastImportDir = getStr(j, "lastImportDir", p.lastImportDir);
    p.firstRunDone = getBool(j, "firstRunDone", p.firstRunDone);
    // Auto-start without a remembered device is never valid (GUI §51).
    if (!p.rememberDevice) p.autoStart = false;
    return p;
}

void savePrefs(AppSettings& s, const Prefs& p) {
    s.update([&](AppSettingsData& d) {
        if (!d.extra.is_object()) d.extra = nlohmann::json::object();
        d.extra["prefs"] = {{"launchAtLogin", p.launchAtLogin},   {"startMinimized", p.startMinimized},
                            {"autoStart", p.autoStart && p.rememberDevice},
                            {"restorePrevious", p.restorePrevious}, {"rememberDevice", p.rememberDevice},
                            {"logLevel", p.logLevel},             {"redactLogs", p.redactLogs},
                            {"redactExports", p.redactExports},   {"lastImportDir", p.lastImportDir},
                            {"firstRunDone", p.firstRunDone},     {"sampleRate", p.sampleRate},
                            {"bufferFrames", p.bufferFrames}};
    });
}

void updatePrefs(AppSettings& s, const std::function<void(Prefs&)>& fn) {
    Prefs p = loadPrefs(s.get());
    fn(p);
    savePrefs(s, p);
}

bool applyLaunchAtLogin(bool enable, bool startMinimized) {
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
#if JUCE_WINDOWS
    const juce::String key = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\BabbleForge";
    if (!enable) return juce::WindowsRegistry::deleteValue(key);
    juce::String cmd = exe.getFullPathName().quoted();
    if (startMinimized) cmd << " --minimized";
    return juce::WindowsRegistry::setValue(key, cmd);
#elif JUCE_LINUX
    const auto file = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                          .getChildFile(".config/autostart/babbleforge.desktop");
    if (!enable) return !file.existsAsFile() || file.deleteFile();
    file.getParentDirectory().createDirectory();
    juce::String text = "[Desktop Entry]\nType=Application\nName=BabbleForge\nExec=" + exe.getFullPathName().quoted();
    if (startMinimized) text << " --minimized";
    text << "\nX-GNOME-Autostart-enabled=true\n";
    return file.replaceWithText(text);
#else
    juce::ignoreUnused(enable, startMinimized, exe);
    return false;
#endif
}

}  // namespace bf::gui
