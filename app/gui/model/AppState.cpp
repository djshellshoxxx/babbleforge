#include "model/AppState.h"

#include <algorithm>
#include <cmath>

#include "core/engine/Scenario.h"  // layoutFromPreset

namespace bf::gui {

namespace {

class PresetEditAction final : public juce::UndoableAction {
public:
    PresetEditAction(AppState& s, Preset before, Preset after, std::string key)
        : s_(s), before_(std::move(before)), after_(std::move(after)), key_(std::move(key)) {}
    bool perform() override {
        s_.applyPresetNoUndo(after_);
        return true;
    }
    bool undo() override {
        s_.applyPresetNoUndo(before_);
        return true;
    }
    int getSizeInUnits() override { return 4096; }
    juce::UndoableAction* createCoalescedAction(juce::UndoableAction* next) override {
        auto* n = dynamic_cast<PresetEditAction*>(next);
        if (n == nullptr || key_.empty() || n->key_ != key_) return nullptr;
        return new PresetEditAction(s_, before_, n->after_, key_);
    }

private:
    AppState& s_;
    Preset before_, after_;
    std::string key_;
};

double clampd(double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }

}  // namespace

Preset AppState::factoryPreset(const DataSet& ds, const std::string& area, const std::string& strategy) {
    Preset p;
    p.name = "Recommended";
    p.area = area;
    p.strategy = strategy;
    PresetBasedOn b;
    b.area = area;
    b.strategy = strategy;
    b.factoryPresetVersion = "1.0.0";
    p.basedOn = b;
    if (auto it = ds.areas.find(area); it != ds.areas.end()) p.macros.strengthDb = it->second.level.defaultStrengthDb;
    else p.macros.strengthDb = 0.0;
    p.outputs.layout = "stereo";
    p.outputs.limiterEnabled = true;
    p.outputs.limiterCeilingDbtp = ds.engineDefaults.limiterCeilingDbtp;
    return p;
}

AppState::AppState(const DataSet& ds, AppSettings& settings, juce::UndoManager& undo, Preset initial)
    : ds_(ds), settings_(settings), undo_(undo), preset_(std::move(initial)) {
    mode_ = settings_.get().advanced ? UiMode::Advanced : UiMode::Simple;
    if (ds_.areas.find(preset_.area) == ds_.areas.end()) preset_.area = "office";
    if (ds_.strategies.find(preset_.strategy) == ds_.strategies.end()) preset_.strategy = "balanced";
    recompute();
}

AppState::~AppState() = default;

void AppState::setMode(UiMode m) {
    if (m == mode_) return;
    mode_ = m;
    settings_.update([m](AppSettingsData& d) { d.advanced = (m == UiMode::Advanced); });
    // Simple mode cannot express strengths outside its range; the stored value is kept as is
    // (no write), the slider only displays the clamped value.
    notify(kModeChanged);
}

void AppState::recompute() {
    const ComposeResult c = compose(ds_, preset_);
    if (c.ok) eff_ = c.config;
    ComposedPlan p = composePlan(ds_, preset_, CorpusSummary{}, layoutFromPreset(preset_));
    if (p.ok) plan_ = std::move(p);
}

void AppState::notify(unsigned changes) {
    listeners_.call([changes](Listener& l) { l.appStateChanged(changes); });
}

void AppState::applyPresetNoUndo(const Preset& p) {
    preset_ = p;
    ++revision_;
    recompute();
    notify(kPresetChanged);
}

void AppState::beginGesture(const juce::String& name) {
    gesture_ = true;
    undo_.beginNewTransaction(name);
}

void AppState::endGesture() { gesture_ = false; }

void AppState::ensureTransaction(const juce::String& name) {
    if (!gesture_) undo_.beginNewTransaction(name);
}

void AppState::edit(const juce::String& name, const std::string& coalesceKey, const std::function<void(Preset&)>& fn) {
    Preset after = preset_;
    fn(after);
    if (computePresetContentHash(after) == computePresetContentHash(preset_)) return;  // no-op edit
    ensureTransaction(name);
    undo_.perform(new PresetEditAction(*this, preset_, std::move(after), gesture_ ? coalesceKey : std::string()));
}

void AppState::replacePreset(const Preset& p, const juce::String& name) {
    gesture_ = false;
    undo_.beginNewTransaction(name);
    undo_.perform(new PresetEditAction(*this, preset_, p, {}));
}

bool AppState::undo() {
    gesture_ = false;
    const bool ok = undo_.undo();
    undo_.beginNewTransaction();
    return ok;
}

bool AppState::redo() {
    gesture_ = false;
    const bool ok = undo_.redo();
    undo_.beginNewTransaction();
    return ok;
}

// ---- simple controls ------------------------------------------------------------------

void AppState::setArea(const std::string& id) {
    if (id == preset_.area || ds_.areas.find(id) == ds_.areas.end()) return;
    // Selecting an area loads its recommended settings (GUI §5). Strength and the output
    // configuration are the user's and are kept.
    Preset p = factoryPreset(ds_, id, preset_.strategy);
    p.macros.strengthDb = preset_.macros.strengthDb;
    p.outputs = preset_.outputs;
    p.name = preset_.name;
    replacePreset(p, "Area: " + juce::String(ds_.areas.at(id).displayName));
}

void AppState::setStrategy(const std::string& id) {
    if (id == preset_.strategy || ds_.strategies.find(id) == ds_.strategies.end()) return;
    edit("Mask type", {}, [&](Preset& p) {
        p.strategy = id;
        if (p.basedOn) p.basedOn->strategy = id;
        // Character / mix defaults are strategy-specific (PRESETS §5).
        p.macros.character.reset();
        p.macros.babbleFraction.reset();
        p.talkers.multiVoiceK.reset();
    });
}

void AppState::setStrengthDb(double db) {
    db = clampd(db, ds_.engineDefaults.advancedMinDb, ds_.engineDefaults.advancedMaxDb);
    edit("Strength", "strength", [db](Preset& p) { p.macros.strengthDb = db; });
}
void AppState::setCharacter(double c) {
    edit("Character", "character", [c](Preset& p) { p.macros.character = clampd(c, 0, 1); });
}
void AppState::setVoiceAmount(double v) {
    edit("Voice Amount", "voiceAmount", [v](Preset& p) { p.macros.voiceAmount = clampd(v, 0, 1); });
}
void AppState::setVoiceVariety(const std::string& v) {
    edit("Voice Variety", "voiceVariety", [v](Preset& p) { p.macros.voiceDiversity = v; });
}
void AppState::setClearVoiceReduction(double r) {
    edit("Clear Voice Reduction", "cvr", [r](Preset& p) { p.macros.clearVoiceReduction = clampd(r, 0, 1); });
}
void AppState::setBabbleFraction(double b) {
    edit("Mask Mix", "mix", [b](Preset& p) { p.macros.babbleFraction = clampd(b, 0, 1); });
}
void AppState::setCoverage(Coverage c) {
    edit("Coverage", {}, [c](Preset& p) {
        p.outputs.layout = c == Coverage::Stereo ? "stereo" : "ring4";
        p.outputs.channels.clear();  // explicit channel maps are owned by the OUTPUT page
    });
}

double AppState::strengthDb() const { return preset_.macros.strengthDb.value_or(eff_.strengthDb); }
double AppState::character() const { return preset_.macros.character.value_or(plan_.ok ? plan_.plan.character : eff_.character); }
double AppState::voiceAmount() const { return preset_.macros.voiceAmount.value_or(eff_.voiceAmount); }
std::string AppState::voiceVariety() const { return preset_.macros.voiceDiversity.value_or(eff_.voiceDiversity); }
double AppState::clearVoiceReduction() const {
    return preset_.macros.clearVoiceReduction.value_or(eff_.clearVoiceReduction);
}
double AppState::babbleFraction() const { return preset_.macros.babbleFraction.value_or(eff_.babbleFraction); }
Coverage AppState::coverage() const {
    if (preset_.outputs.channels.size() > 2) return Coverage::MultiSpeaker;
    if (!preset_.outputs.channels.empty()) return Coverage::Stereo;
    const std::string l = preset_.outputs.layout.value_or("stereo");
    return (l == "stereo" || l == "mono") ? Coverage::Stereo : Coverage::MultiSpeaker;
}
double AppState::strengthMinDb() const {
    return advanced() ? ds_.engineDefaults.advancedMinDb : ds_.engineDefaults.simpleMinDb;
}
double AppState::strengthMaxDb() const {
    return advanced() ? ds_.engineDefaults.advancedMaxDb : ds_.engineDefaults.simpleMaxDb;
}

// ---- talker engine ----------------------------------------------------------------------

TalkerCounts AppState::talkerCounts() const {
    TalkerCounts c;
    const auto& t = plan_.plan.talkers;
    c.pool = static_cast<int>(plan_.ok ? t.pool : static_cast<std::uint32_t>(eff_.pool));
    c.average = plan_.ok ? t.mean : eff_.meanActive;
    c.minimum = static_cast<int>(plan_.ok ? t.minActive : static_cast<std::uint32_t>(eff_.minActive));
    c.maximum = static_cast<int>(plan_.ok ? t.maxActive : static_cast<std::uint32_t>(eff_.maxActive));
    const auto& pt = preset_.talkers;
    if (pt.pool) c.pool = *pt.pool;
    if (pt.meanActive) c.average = *pt.meanActive;
    if (pt.minActive) c.minimum = *pt.minActive;
    if (pt.maxActive) c.maximum = *pt.maxActive;
    return c;
}

TalkerCounts AppState::enforceTalkerCounts(TalkerCounts c, TalkerField changed) {
    using L = TalkerLimits;
    c.pool = std::clamp(c.pool, L::kPoolMin, L::kPoolMax);
    c.average = clampd(c.average, L::kAvgMin, L::kAvgMax);
    c.minimum = std::clamp(c.minimum, L::kMinMin, L::kMaxMax);
    c.maximum = std::clamp(c.maximum, 1, L::kMaxMax);
    switch (changed) {
    case TalkerField::Minimum:
        if (c.average < c.minimum) c.average = c.minimum;
        if (c.maximum < static_cast<int>(std::ceil(c.average))) c.maximum = static_cast<int>(std::ceil(c.average));
        break;
    case TalkerField::Maximum:
        if (c.average > c.maximum) c.average = c.maximum;
        if (c.minimum > static_cast<int>(std::floor(c.average))) c.minimum = static_cast<int>(std::floor(c.average));
        break;
    case TalkerField::Average:
    case TalkerField::Pool:
        if (c.minimum > c.average) c.minimum = static_cast<int>(std::floor(c.average));
        if (c.maximum < c.average) c.maximum = static_cast<int>(std::ceil(c.average));
        break;
    }
    if (changed == TalkerField::Pool) {
        // The pool must hold at least the maximum number of simultaneous talkers.
        if (c.maximum > c.pool) c.maximum = c.pool;
        if (c.average > c.maximum) c.average = c.maximum;
        if (c.minimum > c.average) c.minimum = static_cast<int>(std::floor(c.average));
    } else if (c.pool < c.maximum) {
        c.pool = std::min(L::kPoolMax, c.maximum);
    }
    return c;
}

void AppState::setTalkerValue(TalkerField f, double v) {
    TalkerCounts c = talkerCounts();
    switch (f) {
    case TalkerField::Pool: c.pool = static_cast<int>(std::lround(v)); break;
    case TalkerField::Average: c.average = std::round(v * 10.0) / 10.0; break;
    case TalkerField::Minimum: c.minimum = static_cast<int>(std::lround(v)); break;
    case TalkerField::Maximum: c.maximum = static_cast<int>(std::lround(v)); break;
    }
    c = enforceTalkerCounts(c, f);
    static const char* keys[] = {"talkers.pool", "talkers.avg", "talkers.min", "talkers.max"};
    edit("Talkers", keys[static_cast<int>(f)], [c](Preset& p) {
        p.talkers.pool = c.pool;
        p.talkers.meanActive = c.average;
        p.talkers.minActive = c.minimum;
        p.talkers.maxActive = c.maximum;
    });
}

// ---- timing -----------------------------------------------------------------------------

AppState::Timing AppState::timing() const {
    Timing t;
    const auto& pt = plan_.plan.talkers;
    if (plan_.ok) {
        t.maxInternalGapMs = pt.maxGapMs;
        t.segmentMinS = pt.segMinS;
        t.segmentMaxS = pt.segMaxS;
        t.gainVariationDb = pt.gainSigmaDb;
        t.fadeMs = pt.fadeInMs;
    }
    const auto& p = preset_.talkers;
    if (p.maxInternalGapMs) t.maxInternalGapMs = *p.maxInternalGapMs;
    if (p.segmentMinS) t.segmentMinS = *p.segmentMinS;
    if (p.segmentMaxS) t.segmentMaxS = *p.segmentMaxS;
    if (p.gainVariationDb) t.gainVariationDb = *p.gainVariationDb;
    if (p.fadeInMs) t.fadeMs = *p.fadeInMs;
    return t;
}

void AppState::setMaxInternalGapMs(double v) {
    edit("Maximum internal gap", "gap", [v](Preset& p) { p.talkers.maxInternalGapMs = clampd(v, 20, 2000); });
}
void AppState::setSegmentMinS(double v) {
    const Timing t = timing();
    edit("Minimum segment length", "segMin", [v, t](Preset& p) {
        p.talkers.segmentMinS = clampd(v, 0.5, 30);
        p.talkers.segmentMaxS = std::max(*p.talkers.segmentMinS, p.talkers.segmentMaxS.value_or(t.segmentMaxS));
    });
}
void AppState::setSegmentMaxS(double v) {
    const Timing t = timing();
    edit("Maximum segment length", "segMax", [v, t](Preset& p) {
        p.talkers.segmentMaxS = clampd(v, 1, 60);
        p.talkers.segmentMinS = std::min(*p.talkers.segmentMaxS, p.talkers.segmentMinS.value_or(t.segmentMinS));
    });
}
void AppState::setGainVariationDb(double v) {
    edit("Talker gain variation", "gainVar", [v](Preset& p) { p.talkers.gainVariationDb = clampd(v, 0, 6); });
}
void AppState::setFadeMs(double v) {
    edit("Fade time", "fade", [v](Preset& p) {
        p.talkers.fadeInMs = clampd(v, 10, 1000);
        p.talkers.fadeOutMs = p.talkers.fadeInMs;
    });
}

// ---- stationary / spectrum --------------------------------------------------------------

bool AppState::stationaryEnabled() const { return preset_.stationary.enabled.value_or(eff_.stationaryEnabled); }
void AppState::setStationaryEnabled(bool on) {
    edit("Stationary masker", {}, [on](Preset& p) { p.stationary.enabled = on; });
}
std::string AppState::seedMode() const { return preset_.stationary.seedMode.value_or(eff_.seedMode); }
void AppState::setSeedMode(const std::string& m) {
    edit("Noise seed", {}, [m](Preset& p) { p.stationary.seedMode = m; });
}
std::string AppState::spectrumTarget() const { return preset_.spectrum.target.value_or(eff_.spectrumTarget); }

double AppState::ltassAt(double hz) const {
    const auto it = ds_.targets.find("ltass_universal");
    if (it == ds_.targets.end()) return 0.0;
    const auto& t = it->second;
    for (std::size_t i = 0; i < t.bandCentersHz.size() && i < t.levelsDb.size(); ++i)
        if (std::abs(t.bandCentersHz[i] - hz) < 0.03 * hz) return t.levelsDb[i];
    return 0.0;
}

std::array<double, 7> AppState::customEqDb() const {
    std::array<double, 7> out{};
    const auto& c = preset_.spectrum.custom;
    if (!c) return out;
    for (std::size_t b = 0; b < kEqBandsHz.size(); ++b)
        for (std::size_t i = 0; i < c->bandCentersHz.size() && i < c->levelsDb.size(); ++i)
            if (std::abs(c->bandCentersHz[i] - kEqBandsHz[b]) < 1.0) out[b] = c->levelsDb[i] - ltassAt(kEqBandsHz[b]);
    return out;
}

void AppState::setSpectrumTarget(const std::string& id) {
    const auto eq = customEqDb();
    edit("Spectrum", {}, [this, id, eq](Preset& p) {
        p.spectrum.target = id;
        if (id == "custom" && !p.spectrum.custom) {
            SpectrumTargetDef d;
            d.id = "custom";
            d.displayName = "Custom";
            for (std::size_t b = 0; b < kEqBandsHz.size(); ++b) {
                d.bandCentersHz.push_back(kEqBandsHz[b]);
                d.levelsDb.push_back(ltassAt(kEqBandsHz[b]) + eq[b]);
            }
            p.spectrum.custom = d;
        }
    });
}

void AppState::setCustomEqDb(int band, double db) {
    if (band < 0 || band >= static_cast<int>(kEqBandsHz.size())) return;
    auto eq = customEqDb();
    eq[static_cast<std::size_t>(band)] = clampd(db, -kEqMaxDb, kEqMaxDb);
    edit("Custom spectrum", "eq" + std::to_string(band), [this, eq](Preset& p) {
        SpectrumTargetDef d;
        d.id = "custom";
        d.displayName = "Custom";
        for (std::size_t b = 0; b < kEqBandsHz.size(); ++b) {
            d.bandCentersHz.push_back(kEqBandsHz[b]);
            d.levelsDb.push_back(ltassAt(kEqBandsHz[b]) + eq[b]);
        }
        p.spectrum.custom = d;
        p.spectrum.target = "custom";
    });
}

bool AppState::correctionEnabled() const {
    return preset_.spectrum.correction.enabled.value_or(eff_.correctionEnabled);
}
void AppState::setCorrectionEnabled(bool on) {
    edit("Maintain target spectrum", {}, [on](Preset& p) { p.spectrum.correction.enabled = on; });
}
std::string AppState::correctionSpeed() const {
    return preset_.spectrum.correction.speed.value_or(eff_.correctionSpeed);
}
void AppState::setCorrectionSpeed(const std::string& s) {
    edit("Correction speed", {}, [s](Preset& p) { p.spectrum.correction.speed = s; });
}

}  // namespace bf::gui
