#include "core/rt/Persistence.h"

#include <cstdio>
#include <system_error>

#include "core/config/AtomicFile.h"
#include "core/engine/Scenario.h"
#include "core/rt/Logging.h"

namespace bf::rt {

namespace {
std::string correctionKey(std::uint64_t planHash, const std::string& corpusVersion, double fs) {
    char b[32];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(planHash));
    return std::string(b) + ":" + corpusVersion + ":" + std::to_string(static_cast<long long>(fs));
}
}  // namespace

SessionStore::SessionStore(std::filesystem::path dir) : dir_(std::move(dir)) {}

std::optional<nlohmann::json> SessionStore::readJson(const std::filesystem::path& p, std::string* error) const {
    std::string text;
    if (!enabled() || !readFile(p, text, error)) return std::nullopt;
    if (text.size() > 10u * 1024 * 1024) {  // RELIABILITY §8 size limit
        if (error) *error = p.filename().string() + ": larger than 10 MB";
        return std::nullopt;
    }
    try {
        return nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        if (error) *error = p.filename().string() + ": " + e.what();
        return std::nullopt;
    }
}

bool SessionStore::writeJson(const std::filesystem::path& p, const nlohmann::json& j, std::string* error) {
    if (!enabled()) return false;
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    return writeFileAtomic(p, j.dump(2), error);
}

bool SessionStore::saveSession(const nlohmann::json& preset, std::uint64_t seed, std::string* error) {
    return writeJson(sessionPath(),
                     {{"schema", "babbleforge.session/1"}, {"savedAt", isoUtcNow()}, {"seed", seed}, {"preset", preset}},
                     error);
}

std::optional<nlohmann::json> SessionStore::loadSessionPreset(std::string* error) const {
    auto j = readJson(sessionPath(), error);
    if (!j) return std::nullopt;
    if (!j->is_object() || j->value("schema", "") != "babbleforge.session/1" || !j->contains("preset")) {
        if (error) *error = "session.json: not a babbleforge.session/1 document";
        return std::nullopt;
    }
    return j->at("preset");
}

bool SessionStore::saveLastKnownGood(const nlohmann::json& preset, std::string* error) {
    return writeJson(lkgPath(), {{"schema", "babbleforge.lkg/1"}, {"savedAt", isoUtcNow()}, {"preset", preset}}, error);
}

std::optional<nlohmann::json> SessionStore::loadLastKnownGood(std::string* error) const {
    auto j = readJson(lkgPath(), error);
    if (!j) return std::nullopt;
    if (!j->is_object() || j->value("schema", "") != "babbleforge.lkg/1" || !j->contains("preset")) {
        if (error) *error = "last_known_good.json: not a babbleforge.lkg/1 document";
        return std::nullopt;
    }
    return j->at("preset");
}

bool SessionStore::saveCorrection(std::uint64_t planHash, const std::string& corpusVersion, double fs,
                                  const OperatingBands& c, std::string* error) {
    nlohmann::json doc = readJson(correctionPath(), nullptr).value_or(nlohmann::json::object());
    if (!doc.is_object() || !doc.contains("entries")) doc = {{"schema", "babbleforge.correction/1"}, {"entries", nlohmann::json::object()}};
    doc["entries"][correctionKey(planHash, corpusVersion, fs)] = {{"savedAt", isoUtcNow()},
                                                                  {"bandsDb", std::vector<double>(c.begin(), c.end())}};
    return writeJson(correctionPath(), doc, error);
}

std::optional<OperatingBands> SessionStore::loadCorrection(std::uint64_t planHash, const std::string& corpusVersion,
                                                           double fs) const {
    const auto doc = readJson(correctionPath(), nullptr);
    if (!doc || !doc->is_object() || !doc->contains("entries")) return std::nullopt;
    const auto& e = doc->at("entries");
    const std::string key = correctionKey(planHash, corpusVersion, fs);
    if (!e.contains(key)) return std::nullopt;
    const auto& arr = e.at(key).value("bandsDb", nlohmann::json::array());
    OperatingBands c{};
    if (!arr.is_array() || arr.size() != c.size()) return std::nullopt;
    for (std::size_t i = 0; i < c.size(); ++i) {
        if (!arr[i].is_number()) return std::nullopt;
        c[i] = arr[i].get<double>();
    }
    return c;
}

bool SessionStore::saveSelectorState(const nlohmann::json& state, const std::string& corpusVersion, std::string* error) {
    return writeJson(selectorPath(),
                     {{"schema", "babbleforge.selectorState/1"}, {"corpusVersion", corpusVersion}, {"savedAt", isoUtcNow()},
                      {"state", state}},
                     error);
}

std::optional<nlohmann::json> SessionStore::loadSelectorState(const std::string& corpusVersion) const {
    const auto doc = readJson(selectorPath(), nullptr);
    if (!doc || !doc->is_object() || doc->value("corpusVersion", "") != corpusVersion || !doc->contains("state"))
        return std::nullopt;
    return doc->at("state");
}

nlohmann::json factoryDefaultPreset() {
    return {{"schema", "babbleforge.preset"},
            {"schemaVersion", "1.0"},
            {"name", "Factory default"},
            {"area", "office"},
            {"strategy", "balanced"},
            {"macros", {{"strengthDb", 0.0}, {"character", 0.55}, {"voiceAmount", 0.5}, {"clearVoiceReduction", 1.0}}},
            {"outputs", {{"layout", "stereo"}, {"limiter", {{"enabled", true}, {"ceilingDbtp", -1.0}}}}},
            {"reliability", {{"fallbackPolicy", "continuous"}}}};
}

bool validatePresetDoc(const DataSet& ds, const nlohmann::json& doc, const CorpusSummary& corpus, std::string* error) {
    if (!doc.is_object()) {
        if (error) *error = "preset is not a JSON object";
        return false;
    }
    try {
        const ScenarioPlan sp = buildScenarioPlan(ds, doc, std::nullopt, corpus, 1);
        if (!sp.ok && error) *error = sp.error;
        return sp.ok;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

StartupConfig resolveStartupConfig(const DataSet& ds, const SessionStore& store, const CorpusSummary& corpus,
                                   const std::optional<nlohmann::json>& explicitPreset) {
    StartupConfig r;
    std::string err;
    if (explicitPreset) {
        if (validatePresetDoc(ds, *explicitPreset, corpus, &err)) {
            r.preset = *explicitPreset;
            r.source = "explicit";
            return r;
        }
        r.problems.push_back("explicit preset: " + err);
    }
    bool sessionPresent = false;
    if (store.enabled()) {
        std::error_code ec;
        sessionPresent = std::filesystem::exists(store.sessionPath(), ec);
        if (sessionPresent) {
            err.clear();
            if (auto s = store.loadSessionPreset(&err)) {
                if (validatePresetDoc(ds, *s, corpus, &err)) {
                    r.preset = *s;
                    r.source = "session";
                    return r;
                }
            }
            r.problems.push_back("session.json: " + err);
        }
        err.clear();
        if (auto l = store.loadLastKnownGood(&err)) {
            if (validatePresetDoc(ds, *l, corpus, &err)) {
                r.preset = *l;
                r.source = "lkg";
                r.lkgRestored = sessionPresent || explicitPreset.has_value();
                return r;
            }
            r.problems.push_back("last_known_good.json: " + err);
        }
    }
    r.preset = factoryDefaultPreset();
    r.source = "factory";
    return r;
}

}  // namespace bf::rt
