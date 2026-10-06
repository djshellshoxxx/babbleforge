#include "model/AppSettings.h"

#include <juce_core/juce_core.h>

#include "core/config/AtomicFile.h"

namespace bf::gui {

AppSettings::AppSettings(std::filesystem::path file, int debounceMs) : file_(std::move(file)), debounceMs_(debounceMs) {
    if (file_.empty()) return;
    std::string text;
    if (bf::readFile(file_, text)) {
        const auto j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_object()) d_ = fromJson(j);
    }
}

AppSettings::~AppSettings() {
    stopTimer();
    flush();
}

void AppSettings::update(const std::function<void(AppSettingsData&)>& fn) {
    fn(d_);
    dirty_ = true;
    if (!file_.empty()) startTimer(debounceMs_);
}

bool AppSettings::flush() {
    stopTimer();
    if (!dirty_ || file_.empty()) return true;
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    const bool ok = bf::writeFileAtomic(file_, toJson(d_).dump(2));
    if (ok) dirty_ = false;
    return ok;
}

nlohmann::json AppSettings::toJson(const AppSettingsData& d) {
    nlohmann::json j = d.extra.is_object() ? d.extra : nlohmann::json::object();
    j["schema"] = "babbleforge.settings/1";
    j["ui"] = {{"advanced", d.advanced},
               {"advancedIntroShown", d.advancedIntroShown},
               {"lastPage", d.lastPage},
               {"scale", d.uiScale},
               {"theme", d.theme},
               {"tooltipsEnabled", d.tooltipsEnabled}};
    j["audio"] = {{"deviceId", d.deviceId}};
    j["corpus"] = {{"root", d.corpusRoot}};
    return j;
}

AppSettingsData AppSettings::fromJson(const nlohmann::json& j) {
    AppSettingsData d;
    d.extra = j;
    auto str = [](const nlohmann::json& o, const char* k, std::string& out) {
        if (o.is_object() && o.contains(k) && o[k].is_string()) out = o[k].get<std::string>();
    };
    if (j.contains("ui") && j["ui"].is_object()) {
        const auto& u = j["ui"];
        if (u.contains("advanced") && u["advanced"].is_boolean()) d.advanced = u["advanced"].get<bool>();
        if (u.contains("advancedIntroShown") && u["advancedIntroShown"].is_boolean())
            d.advancedIntroShown = u["advancedIntroShown"].get<bool>();
        if (u.contains("scale") && u["scale"].is_number())
            d.uiScale = juce::jlimit(0.5, 3.0, u["scale"].get<double>());
        str(u, "lastPage", d.lastPage);
        str(u, "theme", d.theme);
        if (u.contains("tooltipsEnabled") && u["tooltipsEnabled"].is_boolean())
            d.tooltipsEnabled = u["tooltipsEnabled"].get<bool>();
    }
    if (j.contains("audio")) str(j["audio"], "deviceId", d.deviceId);
    if (j.contains("corpus")) str(j["corpus"], "root", d.corpusRoot);
    return d;
}

std::filesystem::path AppSettings::defaultDirectory() {
    const auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("BabbleForge");
    return toPath(dir.getFullPathName());
}

}  // namespace bf::gui
