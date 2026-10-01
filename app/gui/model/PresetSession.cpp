#include "model/PresetSession.h"

#include <algorithm>
#include <cmath>

#include "core/config/AtomicFile.h"
#include "core/config/Preset.h"
#include "core/engine/Scenario.h"
#include "core/Version.h"

namespace bf::gui {

namespace {

juce::String num(double v, int decimals) { return juce::String(v, decimals); }
juce::String pct(double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; }
juce::String onOff(bool b) { return b ? "On" : "Off"; }

struct Snapshot {
    EffectiveConfig eff;
    ComposedPlan plan;
};

Snapshot snapshot(const DataSet& ds, const Preset& p) {
    Snapshot s;
    const ComposeResult c = compose(ds, p);
    if (c.ok) s.eff = c.config;
    s.plan = composePlan(ds, p, CorpusSummary{}, layoutFromPreset(p));
    return s;
}

bool differs(double a, double b, double eps = 1e-6) { return std::abs(a - b) > eps; }

juce::String targetName(const DataSet& ds, const std::string& id) {
    if (id == "custom") return "Custom";
    if (auto it = ds.targets.find(id); it != ds.targets.end() && !it->second.displayName.empty())
        return it->second.displayName;
    return id;
}

}  // namespace

PresetSession::PresetSession(AppState& state, std::filesystem::path userPresetDir)
    : s_(state), dir_(std::move(userPresetDir)) {}

Preset PresetSession::recommended() const {
    const Preset& cur = s_.preset();
    Preset f = AppState::factoryPreset(s_.data(), cur.area, cur.strategy);
    f.macros.strengthDb = cur.macros.strengthDb;
    f.outputs = cur.outputs;
    return f;
}

std::vector<ChangeEntry> PresetSession::changes() const {
    std::vector<ChangeEntry> out;
    const DataSet& ds = s_.data();
    const Preset& cur = s_.preset();
    const Snapshot f = snapshot(ds, recommended());
    const EffectiveConfig& e = s_.effective();
    const auto& ft = f.plan.plan.talkers;
    const bool hybrid = cur.strategy == "hybrid";

    auto add = [&](const juce::String& label, const juce::String& from, const juce::String& to, bool adv) {
        if (from != to) out.push_back({label, from, to, adv});
    };

    // Simple macros (effective values).
    const double fChar = f.plan.ok ? f.plan.plan.character : f.eff.character;
    const double cChar = s_.plan().ok ? s_.plan().plan.character : e.character;
    if (differs(fChar, cChar, 0.005)) add("Character", pct(fChar), pct(cChar), false);
    if (differs(f.eff.voiceAmount, e.voiceAmount, 0.005)) add("Voice amount", pct(f.eff.voiceAmount), pct(e.voiceAmount), false);
    add("Voice variety", f.eff.voiceDiversity, e.voiceDiversity, false);
    if (differs(f.eff.clearVoiceReduction, e.clearVoiceReduction, 0.005))
        add("Clear voice reduction", pct(f.eff.clearVoiceReduction), pct(e.clearVoiceReduction), false);
    if (differs(f.eff.babbleFraction, e.babbleFraction, 0.005))
        add(hybrid ? "Mask mix (voices)" : "Noise contribution", pct(hybrid ? f.eff.babbleFraction : f.eff.stationaryFraction()),
            pct(hybrid ? e.babbleFraction : e.stationaryFraction()), !hybrid);

    // Advanced overrides: listed when explicitly set and different from the recommendation.
    const auto& t = cur.talkers;
    if (t.pool) add("Talker pool", juce::String(static_cast<int>(ft.pool)), juce::String(*t.pool), true);
    if (t.meanActive) add("Talkers (average)", num(ft.mean, 1), num(*t.meanActive, 1), true);
    if (t.minActive) add("Minimum active", juce::String(static_cast<int>(ft.minActive)), juce::String(*t.minActive), true);
    if (t.maxActive) add("Maximum active", juce::String(static_cast<int>(ft.maxActive)), juce::String(*t.maxActive), true);
    if (t.maxInternalGapMs) add("Maximum internal gap", num(ft.maxGapMs, 0) + " ms", num(*t.maxInternalGapMs, 0) + " ms", true);
    if (t.segmentMinS) add("Minimum segment length", num(ft.segMinS, 1) + " s", num(*t.segmentMinS, 1) + " s", true);
    if (t.segmentMaxS) add("Maximum segment length", num(ft.segMaxS, 1) + " s", num(*t.segmentMaxS, 1) + " s", true);
    if (t.gainVariationDb)
        add("Talker gain variation", juce::String::fromUTF8("\xc2\xb1") + num(ft.gainSigmaDb, 1) + " dB",
            juce::String::fromUTF8("\xc2\xb1") + num(*t.gainVariationDb, 1) + " dB", true);
    if (t.fadeInMs) add("Fade time", num(ft.fadeInMs, 0) + " ms", num(*t.fadeInMs, 0) + " ms", true);

    add("Stationary masker", onOff(f.eff.stationaryEnabled), onOff(e.stationaryEnabled), true);
    add("Noise seed", f.eff.seedMode == "fixed" ? "Fixed" : "Random", e.seedMode == "fixed" ? "Fixed" : "Random", true);
    add("Spectrum", targetName(ds, f.eff.spectrumTarget), targetName(ds, e.spectrumTarget), true);
    if (e.spectrumTarget == "custom") {
        const auto eq = s_.customEqDb();
        juce::String curve;
        for (std::size_t b = 0; b < eq.size(); ++b)
            if (std::abs(eq[b]) > 0.05)
                curve << (curve.isEmpty() ? "" : ", ") << eqBandLabel(b) << " " << (eq[b] > 0 ? "+" : "") << num(eq[b], 1);
        if (curve.isNotEmpty()) add("Custom spectrum", "flat", curve, true);
    }
    add("Maintain target spectrum", onOff(f.eff.correctionEnabled), onOff(e.correctionEnabled), true);
    add("Correction speed", f.eff.correctionSpeed, e.correctionSpeed, true);
    if (differs(f.eff.spread, e.spread, 0.005)) add("Spatial spread", pct(f.eff.spread), pct(e.spread), true);
    if (differs(f.eff.motion, e.motion, 0.005)) add("Spatial motion", pct(f.eff.motion), pct(e.motion), true);
    add("Speaker variation", f.eff.speakerVariation, e.speakerVariation, true);
    add("Output limiter", onOff(f.eff.limiterEnabled), onOff(e.limiterEnabled), true);
    if (differs(f.eff.limiterCeilingDbtp, e.limiterCeilingDbtp, 0.05))
        add("Limiter ceiling", num(f.eff.limiterCeilingDbtp, 1) + " dBTP", num(e.limiterCeilingDbtp, 1) + " dBTP", true);
    return out;
}

std::vector<ChangeEntry> PresetSession::advancedChanges() const {
    std::vector<ChangeEntry> out;
    for (auto& c : changes())
        if (c.advanced) out.push_back(c);
    return out;
}

bool PresetSession::isModified() const { return !changes().empty(); }

bool PresetSession::differsSignificantly() const { return advancedChanges().size() >= 3; }

PresetOrigin PresetSession::origin() const {
    if (userHash_) {
        return computePresetContentHash(s_.preset()) == *userHash_ ? PresetOrigin::User : PresetOrigin::UserModified;
    }
    return isModified() ? PresetOrigin::Modified : PresetOrigin::Factory;
}

juce::String PresetSession::areaName() const {
    const auto& a = s_.data().areas;
    if (auto it = a.find(s_.preset().area); it != a.end()) return juce::String::fromUTF8(it->second.displayName.c_str());
    return s_.preset().area;
}

juce::String PresetSession::strategyName() const {
    const auto& m = s_.data().strategies;
    if (auto it = m.find(s_.preset().strategy); it != m.end()) return juce::String::fromUTF8(it->second.displayName.c_str());
    return s_.preset().strategy;
}

juce::String PresetSession::title() const {
    if (userHash_ && userName_.isNotEmpty()) return userName_;
    return areaName() + " / " + strategyName();
}

juce::String PresetSession::stateText() const {
    switch (origin()) {
    case PresetOrigin::Factory: return "Recommended";
    case PresetOrigin::Modified: return "Modified";
    case PresetOrigin::User: return "User Preset";
    case PresetOrigin::UserModified: return "User Preset " + juce::String::fromUTF8("\xc2\xb7") + " Modified";
    }
    return {};
}

void PresetSession::resetToRecommended() {
    userHash_.reset();
    userName_.clear();
    s_.replacePreset(recommended(), "Reset to Recommended");
}

std::filesystem::path PresetSession::pathForName(const juce::String& name) const {
    juce::String file = juce::File::createLegalFileName(name.trim());
    if (file.isEmpty()) file = "Preset";
    return dir_ / toPath(file + ".bfpreset");
}

bool PresetSession::saveAs(const juce::String& name, bool rememberDevice, const std::string& deviceId, juce::String* error) {
    if (name.trim().isEmpty()) {
        if (error) *error = "Please enter a name.";
        return false;
    }
    if (dir_.empty()) {
        if (error) *error = "No preset folder.";
        return false;
    }
    Preset p = s_.preset();
    p.name = name.trim().toStdString();
    PresetBasedOn b;
    b.area = p.area;
    b.strategy = p.strategy;
    b.factoryPresetVersion = "1.0.0";
    p.basedOn = b;
    p.created = juce::Time::getCurrentTime().toISO8601(true).toStdString();
    p.appVersion = std::string(bf::versionString());
    p.outputs.rememberDevice = rememberDevice;
    if (rememberDevice && !deviceId.empty()) p.outputs.device = deviceId;
    else p.outputs.device.reset();
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    std::string err;
    if (!bf::writeFileAtomic(pathForName(name), serializePreset(p), &err)) {
        if (error) *error = juce::String(err);
        return false;
    }
    // Saving is not an undoable edit of the configuration: apply the name silently.
    s_.applyPresetNoUndo(p);
    userHash_ = computePresetContentHash(p);
    userName_ = name.trim();
    return true;
}

bool PresetSession::loadUserPreset(const std::filesystem::path& file, juce::String* error) {
    std::string text, err;
    if (!bf::readFile(file, text, &err)) {
        if (error) *error = juce::String(err);
        return false;
    }
    const PresetParseResult r = parsePreset(text);
    if (!r.ok) {
        if (error) *error = juce::String(r.error);
        return false;
    }
    s_.replacePreset(r.preset, "Load preset");
    userHash_ = computePresetContentHash(r.preset);
    userName_ = juce::String::fromUTF8(r.preset.name.c_str());
    return true;
}

std::vector<std::filesystem::path> PresetSession::userPresets() const {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (dir_.empty() || !std::filesystem::is_directory(dir_, ec)) return out;
    for (const auto& e : std::filesystem::directory_iterator(dir_, ec))
        if (e.path().extension() == ".bfpreset") out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace bf::gui
