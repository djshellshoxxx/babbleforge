#include "core/engine/Scenario.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "core/config/Compose.h"
#include "core/config/Preset.h"
#include "core/strategy/MaskStrategy.h"
#include "core/strategy/PlanComposer.h"

namespace bf {

namespace {

bool fail(std::string* error, const std::string& m) {
    if (error) *error = m;
    return false;
}

bool readJsonFile(const std::filesystem::path& p, nlohmann::json& out, std::string* error) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return fail(error, "cannot open " + p.string());
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        out = nlohmann::json::parse(ss.str());
    } catch (const std::exception& e) {
        return fail(error, p.string() + ": " + e.what());
    }
    return true;
}

}  // namespace

std::optional<LabSubMode> labSubModeFromString(const std::string& s) noexcept {
    for (LabSubMode m : {LabSubMode::ContinuousN, LabSubMode::Stochastic, LabSubMode::Ssn, LabSubMode::Pink,
                         LabSubMode::Hybrid, LabSubMode::LtassMatchedBabble})
        if (toString(m) == s) return m;
    return std::nullopt;
}

void setDottedPath(nlohmann::json& doc, const std::string& path, const nlohmann::json& v) {
    nlohmann::json* cur = &doc;
    std::size_t a = 0;
    while (true) {
        const std::size_t b = path.find('.', a);
        const std::string key = path.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (!cur->is_object()) *cur = nlohmann::json::object();
        if (b == std::string::npos) {
            (*cur)[key] = v;
            return;
        }
        cur = &(*cur)[key];
        a = b + 1;
    }
}

bool parseScenario(const nlohmann::json& in, const std::filesystem::path& baseDir, Scenario& out, std::string* error) {
    if (!in.is_object()) return fail(error, "scenario: not a JSON object");
    Scenario s;
    s.doc = in;
    const std::string schema = in.value("schema", std::string("babbleforge.scenario"));
    if (schema != "babbleforge.scenario") return fail(error, "scenario: unexpected schema \"" + schema + "\"");
    try {
        if (!in.contains("preset")) return fail(error, "scenario: missing \"preset\"");
        const nlohmann::json& pj = in.at("preset");
        if (pj.is_string()) {
            std::filesystem::path pp = pj.get<std::string>();
            if (pp.is_relative()) pp = baseDir / pp;
            if (!readJsonFile(pp, s.preset, error)) return false;
        } else if (pj.is_object()) {
            s.preset = pj;
        } else {
            return fail(error, "scenario: \"preset\" must be an object or a path");
        }
        if (!s.preset.contains("schema")) s.preset["schema"] = "babbleforge.preset";
        if (!s.preset.contains("schemaVersion")) s.preset["schemaVersion"] = "1.0";
        s.doc["preset"] = s.preset;

        if (in.contains("seed") && !in.at("seed").is_null()) {
            s.seed = in.at("seed").get<std::uint64_t>();
            s.seedGiven = true;
        } else if (s.preset.contains("random") && s.preset["random"].contains("seed") &&
                   !s.preset["random"]["seed"].is_null()) {
            s.seed = s.preset["random"]["seed"].get<std::uint64_t>();
            s.seedGiven = true;
        }
        s.durationS = in.value("durationS", 60.0);
        if (!(s.durationS > 0.0) || s.durationS > 86400.0) return fail(error, "scenario: durationS out of range");
        s.sampleRate = in.value("sampleRate", 48000.0);
        if (s.sampleRate != 44100.0 && s.sampleRate != 48000.0 && s.sampleRate != 88200.0 && s.sampleRate != 96000.0)
            return fail(error, "scenario: unsupported sampleRate");
        s.corpusVersion = in.value("corpusVersion", std::string());
        if (in.contains("outputFormat")) {
            const auto& of = in.at("outputFormat");
            if (of.value("container", std::string("wav")) != "wav" ||
                of.value("sampleFormat", std::string("float32")) != "float32")
                return fail(error, "scenario: only wav/float32 output is supported");
        }
        if (in.contains("events")) {
            for (const auto& e : in.at("events")) {
                ScenarioEvent ev;
                ev.atS = e.at("atS").get<double>();
                ev.set = e.value("set", nlohmann::json::object());
                if (!ev.set.is_object()) return fail(error, "scenario: event \"set\" must be an object");
                if (ev.atS < 0.0) return fail(error, "scenario: event atS < 0");
                s.events.push_back(std::move(ev));
            }
            std::stable_sort(s.events.begin(), s.events.end(),
                             [](const ScenarioEvent& a, const ScenarioEvent& b) { return a.atS < b.atS; });
        }
        if (in.contains("laboratory") && !in.at("laboratory").is_null()) {
            const auto& l = in.at("laboratory");
            LabSpec lab;
            const std::string mode = l.value("mode", std::string("continuousN"));
            const auto m = labSubModeFromString(mode);
            if (!m) return fail(error, "scenario: unknown laboratory mode \"" + mode + "\"");
            lab.mode = *m;
            lab.talkers = l.value("talkers", 4);
            if (lab.talkers < 1 || lab.talkers > 64) return fail(error, "scenario: laboratory.talkers must be 1..64");
            if (l.contains("maxGapMs") && !l.at("maxGapMs").is_null()) lab.maxGapMs = l.at("maxGapMs").get<double>();
            if (l.contains("babbleFraction") && !l.at("babbleFraction").is_null())
                lab.babbleFraction = l.at("babbleFraction").get<double>();
            lab.twoPassRms = l.value("rmsNormalization", std::string("two-pass")) == "two-pass";
            lab.strictSpectrum = l.value("spectrumMatch", std::string(lab.mode == LabSubMode::LtassMatchedBabble
                                                                            ? "strict"
                                                                            : "none")) == "strict";
            lab.limiter = l.value("limiter", false);
            s.lab = lab;
        }
    } catch (const std::exception& e) {
        return fail(error, std::string("scenario: ") + e.what());
    }
    out = std::move(s);
    return true;
}

bool loadScenarioFile(const std::filesystem::path& file, Scenario& out, std::string* error) {
    nlohmann::json j;
    if (!readJsonFile(file, j, error)) return false;
    return parseScenario(j, file.parent_path(), out, error);
}

OutputLayout layoutFromPreset(const Preset& p) {
    const auto& ch = p.outputs.channels;
    if (!ch.empty()) {
        OutputLayout L;
        L.id = p.outputs.layout.value_or("custom");
        const std::size_t n = ch.size();
        L.kind = n == 1 ? LayoutKind::Mono : n == 2 ? LayoutKind::Stereo : LayoutKind::Ring;
        for (std::size_t i = 0; i < n; ++i) {
            OutputDef d;
            d.index = static_cast<std::uint8_t>(i);
            d.deviceChannel = static_cast<std::uint16_t>(std::max(ch[i].deviceChannel, 0));
            d.label = ch[i].label;
            d.azimuthDeg = static_cast<float>(ch[i].azimuthDeg);
            d.zone = static_cast<std::uint8_t>(std::clamp(ch[i].zone, 0, 7));
            d.enabled = ch[i].enabled;
            L.outputs.push_back(d);
        }
        return L;
    }
    const std::string name = p.outputs.layout.value_or("stereo");
    if (name == "mono") return makeMonoLayout();
    if (name == "ring4") return makeRing4Layout();
    if (name == "ring6") return makeRing6Layout();
    if (name == "ring8") return makeRing8Layout();
    return makeStereoLayout();
}

ScenarioPlan buildScenarioPlan(const DataSet& ds, const nlohmann::json& presetDoc, const std::optional<LabSpec>& lab,
                               const CorpusSummary& corpus, std::uint64_t seed) {
    ScenarioPlan out;
    nlohmann::json pd = presetDoc;
    if (!pd.contains("schema")) pd["schema"] = "babbleforge.preset";
    if (!pd.contains("schemaVersion")) pd["schemaVersion"] = "1.0";
    if (lab) pd["strategy"] = "research";
    PresetParseResult pr = parsePreset(pd.dump());
    if (!pr.ok) {
        out.error = "preset: " + pr.error;
        return out;
    }
    out.preset = pr.preset;
    const ComposeResult c = compose(ds, out.preset);
    if (!c.ok) {
        out.error = c.error;
        return out;
    }
    auto strategy = makeStrategy(ds, out.preset.strategy);
    if (!strategy) {
        out.error = "strategy: cannot instantiate \"" + out.preset.strategy + "\"";
        return out;
    }
    out.layout = layoutFromPreset(out.preset);
    StrategyParams params = strategyParamsFromPreset(out.preset);
    MacroState macros = macroStateFromPreset(out.preset);
    params.seed = seed;
    if (lab) {
        params.labMode = lab->mode;
        params.labN = lab->talkers;
        if (lab->maxGapMs) params.maxInternalGapMs = *lab->maxGapMs;
        if (lab->babbleFraction) params.babbleFraction = *lab->babbleFraction;
        params.limiterEnabled = lab->limiter;
    }
    ValidationResult v = strategy->validate(params, macros, corpus);
    out.plan = strategy->buildPlan(ds.areas.at(out.preset.area), params, macros, corpus, out.layout);
    out.plan.talkers.seed = seed;
    out.conflicts = std::move(v.conflicts);
    out.ok = true;
    return out;
}

}  // namespace bf
