#include "model/OutputModel.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/engine/Scenario.h"  // layoutFromPreset

namespace bf::gui::outputs {

namespace {

double clampd(double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }

const char* layoutId(SpatialMode m) {
    switch (m) {
    case SpatialMode::Mono: return "mono";
    case SpatialMode::Stereo: return "stereo";
    case SpatialMode::Ring4: return "ring4";
    case SpatialMode::Ring6: return "ring6";
    case SpatialMode::Ring8: return "ring8";
    case SpatialMode::Custom: return "custom";
    }
    return "stereo";
}

std::vector<OutputChannel> channelsOf(const OutputLayout& L) {
    std::vector<OutputChannel> out;
    for (const OutputDef& d : L.outputs) {
        OutputChannel c;
        c.index = d.index;
        c.deviceChannel = d.deviceChannel;
        c.label = d.label;
        c.azimuthDeg = d.azimuthDeg;
        c.zone = d.zone;
        c.enabled = d.enabled;
        out.push_back(c);
    }
    return out;
}

// Explicit channel entries are needed to store per-speaker settings.
void materialize(Preset& p) {
    if (!p.outputs.channels.empty()) return;
    p.outputs.channels = channelsOf(layoutFromPreset(p));
    if (!p.outputs.layout) p.outputs.layout = "stereo";
}

int zoneIndex(const Preset& p, int id) {
    for (std::size_t i = 0; i < p.outputs.zones.size(); ++i)
        if (p.outputs.zones[i].id == id) return static_cast<int>(i);
    return -1;
}

void setLayoutOnly(Preset& p, SpatialMode m, int customCount) {
    const OutputLayout L = layoutFor(m, customCount);
    p.outputs.layout = layoutId(m);
    p.outputs.channels.clear();
    p.outputs.zones.clear();
    if (m == SpatialMode::Custom) p.outputs.channels = channelsOf(L);  // "custom" needs explicit channels
}

}  // namespace

OutputLayout layoutFor(SpatialMode m, int customCount) {
    switch (m) {
    case SpatialMode::Mono: return makeMonoLayout();
    case SpatialMode::Stereo: return makeStereoLayout();
    case SpatialMode::Ring4: return makeRing4Layout();
    case SpatialMode::Ring6: return makeRing6Layout();
    case SpatialMode::Ring8: return makeRing8Layout();
    case SpatialMode::Custom: return makeCustomRingLayout(std::clamp(customCount, kCustomMin, kCustomMax));
    }
    return makeStereoLayout();
}

OutputLayout currentLayout(const Preset& p) { return layoutFromPreset(p); }
int outputCount(const Preset& p) { return layoutFromPreset(p).size(); }

SpatialMode spatialMode(const Preset& p) {
    const std::string l = p.outputs.layout.value_or("stereo");
    const std::size_t n = p.outputs.channels.size();
    if (l == "mono") return SpatialMode::Mono;
    if (l == "stereo") return SpatialMode::Stereo;
    if (n == 0) {
        if (l == "ring4") return SpatialMode::Ring4;
        if (l == "ring6") return SpatialMode::Ring6;
        if (l == "ring8") return SpatialMode::Ring8;
        return SpatialMode::Stereo;
    }
    if (l == "ring4" && n == 4) return SpatialMode::Ring4;
    if (l == "ring6" && n == 6) return SpatialMode::Ring6;
    if (l == "ring8" && n == 8) return SpatialMode::Ring8;
    return SpatialMode::Custom;
}

juce::String spatialModeName(SpatialMode m) {
    switch (m) {
    case SpatialMode::Mono: return "Mono";
    case SpatialMode::Stereo: return "Stereo";
    case SpatialMode::Ring4: return "4 Channel";
    case SpatialMode::Ring6: return "6 Channel";
    case SpatialMode::Ring8: return "8 Channel";
    case SpatialMode::Custom: return "Custom";
    }
    return {};
}

// ---- Simple --------------------------------------------------------------------------------

AreaSize areaSize(const AppState& s) {
    const double sp = spread(s);
    return sp < 0.45 ? AreaSize::Small : sp < 0.75 ? AreaSize::Medium : AreaSize::Large;
}

void setAreaSize(AppState& s, AreaSize a) {
    const double v = a == AreaSize::Small ? 0.35 : a == AreaSize::Medium ? 0.60 : 0.85;
    s.edit("Area size", {}, [v](Preset& p) { p.spatial.spread = v; });
}

SpeakerSetup speakerSetup(const AppState& s) {
    const int n = outputCount(s.preset());
    return n <= 2 ? SpeakerSetup::Two : n == 4 ? SpeakerSetup::Four : SpeakerSetup::MoreThanFour;
}

void setSpeakerSetup(AppState& s, SpeakerSetup v) {
    const SpatialMode m = v == SpeakerSetup::Two ? SpatialMode::Stereo : v == SpeakerSetup::Four ? SpatialMode::Ring4 : SpatialMode::Ring6;
    s.edit("Speaker setup", {}, [m](Preset& p) { setLayoutOnly(p, m, 4); });
}

void setOutdoorSpeakers(AppState& s, int n) {
    const SpatialMode m = n <= 2 ? SpatialMode::Stereo : n <= 4 ? SpatialMode::Ring4 : n <= 6 ? SpatialMode::Ring6 : SpatialMode::Ring8;
    s.edit("Speaker layout", {}, [m](Preset& p) { setLayoutOnly(p, m, 4); });
}

int outdoorSpeakers(const AppState& s) {
    const int n = outputCount(s.preset());
    return n <= 2 ? 2 : n <= 4 ? 4 : n <= 6 ? 6 : 8;
}

// ---- Advanced spatial --------------------------------------------------------------------------

void setSpatialMode(AppState& s, SpatialMode m, int customCount) {
    s.edit("Spatial output", {}, [=](Preset& p) { setLayoutOnly(p, m, customCount); });
}

double spread(const AppState& s) { return s.preset().spatial.spread.value_or(s.effective().spread); }
void setSpread(AppState& s, double v) {
    s.edit("Spatial spread", "spread", [v](Preset& p) { p.spatial.spread = clampd(v, 0, 1); });
}
std::string speakerVariation(const AppState& s) {
    return s.preset().spatial.speakerVariation.value_or(s.effective().speakerVariation);
}
void setSpeakerVariation(AppState& s, const std::string& v) {
    s.edit("Speaker variation", {}, [v](Preset& p) { p.spatial.speakerVariation = v; });
}

// ---- speakers -----------------------------------------------------------------------------------

const OutputChannel* channelAt(const Preset& p, int ch) {
    return ch >= 0 && static_cast<std::size_t>(ch) < p.outputs.channels.size() ? &p.outputs.channels[static_cast<std::size_t>(ch)] : nullptr;
}

template <typename Fn>
void editChannel(AppState& s, const char* name, const std::string& key, int ch, Fn fn) {
    s.edit(name, key, [=](Preset& p) {
        materialize(p);
        if (ch < 0 || static_cast<std::size_t>(ch) >= p.outputs.channels.size()) return;
        fn(p.outputs.channels[static_cast<std::size_t>(ch)]);
    });
}

void setChannelEnabled(AppState& s, int ch, bool on) {
    editChannel(s, "Speaker enabled", {}, ch, [on](OutputChannel& c) { c.enabled = on; });
}
void setChannelGainDb(AppState& s, int ch, double db) {
    editChannel(s, "Speaker level", "chgain" + std::to_string(ch), ch, [db](OutputChannel& c) { c.gainDb = clampd(db, -40, 6); });
}
void setChannelDelayMs(AppState& s, int ch, double ms) {
    editChannel(s, "Speaker delay", "chdelay" + std::to_string(ch), ch, [ms](OutputChannel& c) { c.delayMs = clampd(ms, 0, 100); });
}
void setChannelZone(AppState& s, int ch, int zone) {
    s.edit("Speaker zone", {}, [=](Preset& p) {
        materialize(p);
        if (ch < 0 || static_cast<std::size_t>(ch) >= p.outputs.channels.size() || zoneIndex(p, zone) < 0) return;
        p.outputs.channels[static_cast<std::size_t>(ch)].zone = zone;
    });
}

// ---- zones ---------------------------------------------------------------------------------------

juce::String zoneName(int zoneId) { return "Zone " + juce::String::charToString(static_cast<juce::juce_wchar>('A' + std::clamp(zoneId, 0, 25))); }

void addZone(AppState& s) {
    s.edit("Add zone", {}, [](Preset& p) {
        materialize(p);
        auto addOne = [&p](int id) {
            OutputZone z;
            z.id = id;
            z.name = zoneName(id).toStdString();
            p.outputs.zones.push_back(z);
        };
        if (p.outputs.zones.empty()) {
            addOne(0);
            addOne(1);
            // "Channels 1-4 / 5-8": the first half of the speakers is zone A, the rest zone B.
            const std::size_t n = p.outputs.channels.size(), half = (n + 1) / 2;
            for (std::size_t i = 0; i < n; ++i) p.outputs.channels[i].zone = i < half ? 0 : 1;
        } else if (static_cast<int>(p.outputs.zones.size()) < kMaxZones) {
            int id = 0;
            while (zoneIndex(p, id) >= 0) ++id;
            addOne(id);
        }
    });
}

void removeZone(AppState& s) {
    s.edit("Remove zone", {}, [](Preset& p) {
        if (p.outputs.zones.empty()) return;
        const int gone = p.outputs.zones.back().id;
        p.outputs.zones.pop_back();
        const bool last = p.outputs.zones.size() <= 1;
        const int into = p.outputs.zones.empty() ? 0 : p.outputs.zones.back().id;
        for (auto& c : p.outputs.channels)
            if (c.zone == gone || last) c.zone = last ? 0 : into;
        if (last) p.outputs.zones.clear();
    });
}

template <typename Fn>
void editZone(AppState& s, const char* name, const std::string& key, int zoneId, Fn fn) {
    s.edit(name, key, [=](Preset& p) {
        const int i = zoneIndex(p, zoneId);
        if (i >= 0) fn(p.outputs.zones[static_cast<std::size_t>(i)]);
    });
}

void setZoneEnabled(AppState& s, int zoneId, bool on) {
    editZone(s, "Zone enabled", {}, zoneId, [on](OutputZone& z) { z.enabled = on; });
}
void setZoneLevelDb(AppState& s, int zoneId, double db) {
    editZone(s, "Zone level", "zlevel" + std::to_string(zoneId), zoneId, [db](OutputZone& z) { z.levelDb = clampd(db, -24, 6); });
}
void setZoneMixOffset(AppState& s, int zoneId, double offset) {
    editZone(s, "Zone mix offset", "zmix" + std::to_string(zoneId), zoneId,
             [offset](OutputZone& z) { z.babbleFractionOffset = clampd(offset, -0.5, 0.5); });
}

// ---- limiter --------------------------------------------------------------------------------------

bool limiterEnabled(const AppState& s) { return s.preset().outputs.limiterEnabled.value_or(s.effective().limiterEnabled); }
void setLimiterEnabled(AppState& s, bool on) {
    s.edit("Limiter", {}, [on](Preset& p) { p.outputs.limiterEnabled = on; });
}
double limiterCeilingDbtp(const AppState& s) {
    return s.preset().outputs.limiterCeilingDbtp.value_or(s.effective().limiterCeilingDbtp);
}
void setLimiterCeilingDbtp(AppState& s, double db) {
    s.edit("Limiter ceiling", "ceiling", [db](Preset& p) { p.outputs.limiterCeilingDbtp = clampd(db, -6.0, -0.1); });
}

// ---- recommendations --------------------------------------------------------------------------------

juce::String recommendation(const AppState& s) {
    const auto it = s.data().areas.find(s.preset().area);
    if (it == s.data().areas.end()) return {};
    const AreaModel& a = it->second;
    const int n = outputCount(s.preset());
    const juce::String name = juce::String::fromUTF8(a.displayName.c_str());
    for (const auto& adv : a.advisories) {  // "outputs < N"
        const auto lt = adv.when.find('<');
        if (adv.when.rfind("outputs", 0) == 0 && lt != std::string::npos && n < std::atoi(adv.when.c_str() + lt + 1))
            return juce::String::fromUTF8(adv.text.c_str());
    }
    if (n < a.outputs.minRecommended)
        return "This preset works best with at least " + juce::String(a.outputs.minRecommended) +
               " independent speakers. You selected " + name + " with " + juce::String(n) + (n == 1 ? " speaker" : " speakers") +
               "; coverage may be uneven.";
    return {};
}

// ---- level band ---------------------------------------------------------------------------------------

LevelBand levelBand(double leq60Db, double lRefDbfs, bool limiterSustained) {
    if (leq60Db <= -150.0) return LevelBand::None;
    const double rel = leq60Db - lRefDbfs;
    if (limiterSustained || rel > 6.0) return LevelBand::High;
    return rel < -12.0 ? LevelBand::Low : LevelBand::Good;
}

juce::String levelBandName(LevelBand b) {
    switch (b) {
    case LevelBand::Low: return "LOW";
    case LevelBand::Good: return "GOOD";
    case LevelBand::High: return "HIGH";
    case LevelBand::None: break;
    }
    return "-";
}

}  // namespace bf::gui::outputs
