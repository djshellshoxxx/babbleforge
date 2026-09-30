#include "core/config/Preset.h"

#include <algorithm>
#include <cmath>

#include "core/config/JsonRead.h"
#include "core/config/Sha256.h"

namespace bf {
using namespace detail;

namespace {

// ---- parsing ---------------------------------------------------------------

struct Parser {
  std::vector<Adjustment>& adj;

  template <class T>
  void clamp(T& v, double lo, double hi, const std::string& path, const std::string& what = "") {
    double d = static_cast<double>(v);
    double c = std::min(std::max(d, lo), hi);
    if (c != d || std::isnan(d)) {
      if (std::isnan(d)) c = lo;
      adj.push_back({path, d, c, what.empty() ? "out of range [" + fmt(lo) + ", " + fmt(hi) + "]" : what});
      v = static_cast<T>(c);
    }
  }
  template <class T>
  void clampOpt(std::optional<T>& v, double lo, double hi, const std::string& path) {
    if (v) clamp(*v, lo, hi, path);
  }
  static std::string fmt(double d) {
    std::string s = std::to_string(d);
    s.erase(s.find_last_not_of('0') + 1);
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
  }

  // Applies max<=pool, mean<=max, min<=mean over the values that are present.
  void invariants(PresetTalkers& t) {
    if (t.pool && t.maxActive && *t.maxActive > *t.pool) {
      adj.push_back({"talkers.maxActive", double(*t.maxActive), double(*t.pool), "maxActive must not exceed pool"});
      t.maxActive = t.pool;
    }
    std::optional<double> upper;
    if (t.maxActive) upper = double(*t.maxActive);
    else if (t.pool) upper = double(*t.pool);
    if (t.meanActive && upper && *t.meanActive > *upper) {
      adj.push_back({"talkers.meanActive", *t.meanActive, *upper, "meanActive must not exceed maxActive"});
      t.meanActive = *upper;
    }
    if (t.minActive) {
      std::optional<double> up2;
      if (t.meanActive) up2 = *t.meanActive;
      else up2 = upper;
      if (up2 && double(*t.minActive) > *up2) {
        int to = static_cast<int>(std::floor(*up2));
        adj.push_back({"talkers.minActive", double(*t.minActive), double(to), "minActive must not exceed meanActive"});
        t.minActive = to;
      }
    }
    if (t.segmentMinS && t.segmentMaxS && *t.segmentMinS > *t.segmentMaxS) {
      adj.push_back({"talkers.segmentMinS", *t.segmentMinS, *t.segmentMaxS, "segmentMinS must not exceed segmentMaxS"});
      t.segmentMinS = t.segmentMaxS;
    }
  }

  SpectrumTargetDef target(const json& j, const std::string& path) {
    SpectrumTargetDef t;
    t.id = getDef<std::string>(j, "id", path, "");
    t.displayName = getDef<std::string>(j, "displayName", path, "");
    t.notes = getDef<std::string>(j, "notes", path, "");
    t.bandCentersHz = getVec<double>(j, "bandCentersHz", path);
    t.levelsDb = getVec<double>(j, "levelsDb", path);
    if (t.bandCentersHz.size() != t.levelsDb.size())
      throw ParseError(path + ": bandCentersHz and levelsDb must have equal length");
    return t;
  }

  void run(const json& j, Preset& p) {
    p.name = getDef<std::string>(j, "name", "", "");
    if (const json* b = getObj(j, "basedOn", "")) {
      PresetBasedOn bo;
      bo.area = getOpt<std::string>(*b, "area", "basedOn");
      bo.strategy = getOpt<std::string>(*b, "strategy", "basedOn");
      bo.factoryPresetVersion = getOpt<std::string>(*b, "factoryPresetVersion", "basedOn");
      p.basedOn = bo;
    }
    p.created = getDef<std::string>(j, "created", "", "");
    p.appVersion = getDef<std::string>(j, "appVersion", "", "");
    p.notes = getDef<std::string>(j, "notes", "", "");
    p.area = getReq<std::string>(j, "area", "");
    p.strategy = getReq<std::string>(j, "strategy", "");
    p.contentHash = getDef<std::string>(j, "contentHash", "", "");

    if (const json* m = getObj(j, "macros", "")) {
      auto& o = p.macros;
      o.strengthDb = getOpt<double>(*m, "strengthDb", "macros");
      o.character = getOpt<double>(*m, "character", "macros");
      o.voiceAmount = getOpt<double>(*m, "voiceAmount", "macros");
      o.clearVoiceReduction = getOpt<double>(*m, "clearVoiceReduction", "macros");
      o.voiceDiversity = getOpt<std::string>(*m, "voiceDiversity", "macros");
      if (const json* mix = getObj(*m, "mix", "macros"))
        o.babbleFraction = getOpt<double>(*mix, "babbleFraction", "macros.mix");
      clampOpt(o.strengthDb, -40, 12, "macros.strengthDb");
      clampOpt(o.character, 0, 1, "macros.character");
      clampOpt(o.voiceAmount, 0, 1, "macros.voiceAmount");
      clampOpt(o.clearVoiceReduction, 0, 1, "macros.clearVoiceReduction");
      clampOpt(o.babbleFraction, 0, 1, "macros.mix.babbleFraction");
    }

    if (const json* t = getObj(j, "talkers", "")) {
      auto& o = p.talkers;
      o.pool = getOpt<int>(*t, "pool", "talkers");
      o.meanActive = getOpt<double>(*t, "meanActive", "talkers");
      o.minActive = getOpt<int>(*t, "minActive", "talkers");
      o.maxActive = getOpt<int>(*t, "maxActive", "talkers");
      o.segmentMinS = getOpt<double>(*t, "segmentMinS", "talkers");
      o.segmentMaxS = getOpt<double>(*t, "segmentMaxS", "talkers");
      o.segmentMedianS = getOpt<double>(*t, "segmentMedianS", "talkers");
      o.maxInternalGapMs = getOpt<double>(*t, "maxInternalGapMs", "talkers");
      o.gainVariationDb = getOpt<double>(*t, "gainVariationDb", "talkers");
      o.fadeInMs = getOpt<double>(*t, "fadeInMs", "talkers");
      o.fadeOutMs = getOpt<double>(*t, "fadeOutMs", "talkers");
      o.reEntryCooldownS = getOpt<double>(*t, "reEntryCooldownS", "talkers");
      o.segmentCooldownMin = getOpt<double>(*t, "segmentCooldownMin", "talkers");
      o.poolRotationMin = getOpt<double>(*t, "poolRotationMin", "talkers");
      o.languageAware = getOpt<bool>(*t, "languageAware", "talkers");
      o.targetVoiceProfile = getOpt<std::string>(*t, "targetVoiceProfile", "talkers");
      o.multiVoiceK = getOpt<int>(*t, "multiVoiceK", "talkers");
      clampOpt(o.pool, 1, 64, "talkers.pool");
      clampOpt(o.meanActive, 1, 64, "talkers.meanActive");
      clampOpt(o.minActive, 0, 64, "talkers.minActive");
      clampOpt(o.maxActive, 1, 64, "talkers.maxActive");
      clampOpt(o.segmentMinS, 0.1, 120, "talkers.segmentMinS");
      clampOpt(o.segmentMaxS, 0.1, 120, "talkers.segmentMaxS");
      clampOpt(o.segmentMedianS, 0.1, 120, "talkers.segmentMedianS");
      clampOpt(o.maxInternalGapMs, 0, 5000, "talkers.maxInternalGapMs");
      clampOpt(o.gainVariationDb, 0, 12, "talkers.gainVariationDb");
      clampOpt(o.fadeInMs, 0, 2000, "talkers.fadeInMs");
      clampOpt(o.fadeOutMs, 0, 2000, "talkers.fadeOutMs");
      clampOpt(o.reEntryCooldownS, 0, 60, "talkers.reEntryCooldownS");
      clampOpt(o.segmentCooldownMin, 0, 1440, "talkers.segmentCooldownMin");
      clampOpt(o.poolRotationMin, 0, 1440, "talkers.poolRotationMin");
      clampOpt(o.multiVoiceK, 1, 16, "talkers.multiVoiceK");
      invariants(o);
    }

    if (const json* s = getObj(j, "stationary", "")) {
      p.stationary.enabled = getOpt<bool>(*s, "enabled", "stationary");
      p.stationary.spectrum = getOpt<std::string>(*s, "spectrum", "stationary");
      p.stationary.seedMode = getOpt<std::string>(*s, "seedMode", "stationary");
    }

    if (const json* s = getObj(j, "spectrum", "")) {
      auto& o = p.spectrum;
      o.target = getOpt<std::string>(*s, "target", "spectrum");
      o.speechMatchedProfile = getOpt<std::string>(*s, "speechMatchedProfile", "spectrum");
      if (const json* c = getObj(*s, "custom", "spectrum")) o.custom = target(*c, "spectrum.custom");
      o.lfLimitHz = getOpt<double>(*s, "lfLimitHz", "spectrum");
      o.hfLimitHz = getOpt<double>(*s, "hfLimitHz", "spectrum");
      o.lfTrimDb125 = getOpt<double>(*s, "lfTrimDb125", "spectrum");
      clampOpt(o.lfLimitHz, 20, 500, "spectrum.lfLimitHz");
      clampOpt(o.hfLimitHz, 2000, 20000, "spectrum.hfLimitHz");
      clampOpt(o.lfTrimDb125, -12, 6, "spectrum.lfTrimDb125");
      if (const json* c = getObj(*s, "correction", "spectrum")) {
        o.correction.enabled = getOpt<bool>(*c, "enabled", "spectrum.correction");
        o.correction.speed = getOpt<std::string>(*c, "speed", "spectrum.correction");
        o.correction.maxDb = getOpt<double>(*c, "maxDb", "spectrum.correction");
        o.correction.rememberCorrection = getOpt<bool>(*c, "rememberCorrection", "spectrum.correction");
        clampOpt(o.correction.maxDb, 0, 12, "spectrum.correction.maxDb");
      }
    }

    if (const json* s = getObj(j, "spatial", "")) {
      auto& o = p.spatial;
      o.algorithm = getOpt<std::string>(*s, "algorithm", "spatial");
      o.spread = getOpt<double>(*s, "spread", "spatial");
      o.motion = getOpt<double>(*s, "motion", "spatial");
      o.speakerVariation = getOpt<std::string>(*s, "speakerVariation", "spatial");
      o.neighborhoodSize = getOpt<int>(*s, "neighborhoodSize", "spatial");
      clampOpt(o.spread, 0, 1, "spatial.spread");
      clampOpt(o.motion, 0, 1, "spatial.motion");
      clampOpt(o.neighborhoodSize, 1, 16, "spatial.neighborhoodSize");
    }

    if (const json* s = getObj(j, "outputs", "")) {
      auto& o = p.outputs;
      o.rememberDevice = getDef<bool>(*s, "rememberDevice", "outputs", false);
      o.device = getOpt<std::string>(*s, "device", "outputs");
      o.layout = getOpt<std::string>(*s, "layout", "outputs");
      if (const json* arr = getArr(*s, "channels", "outputs")) {
        std::size_t i = 0;
        for (const auto& e : *arr) {
          std::string path = "outputs.channels[" + std::to_string(i++) + "]";
          if (!e.is_object()) throw ParseError(path + ": expected object");
          OutputChannel c;
          c.index = getDef<int>(e, "index", path, int(i - 1));
          c.deviceChannel = getDef<int>(e, "deviceChannel", path, c.index);
          c.label = getDef<std::string>(e, "label", path, "");
          c.azimuthDeg = getDef<double>(e, "azimuthDeg", path, 0.0);
          c.zone = getDef<int>(e, "zone", path, 0);
          c.enabled = getDef<bool>(e, "enabled", path, true);
          c.gainDb = getDef<double>(e, "gainDb", path, 0.0);
          c.mute = getDef<bool>(e, "mute", path, false);
          c.polarityInvert = getDef<bool>(e, "polarityInvert", path, false);
          c.delayMs = getDef<double>(e, "delayMs", path, 0.0);
          c.calEqProfile = getOpt<std::string>(e, "calEqProfile", path);
          clamp(c.azimuthDeg, -180, 180, path + ".azimuthDeg");
          clamp(c.gainDb, -40, 6, path + ".gainDb");
          clamp(c.delayMs, 0, 100, path + ".delayMs");
          o.channels.push_back(std::move(c));
        }
      }
      if (const json* arr = getArr(*s, "zones", "outputs")) {
        std::size_t i = 0;
        for (const auto& e : *arr) {
          std::string path = "outputs.zones[" + std::to_string(i++) + "]";
          if (!e.is_object()) throw ParseError(path + ": expected object");
          OutputZone z;
          z.id = getDef<int>(e, "id", path, int(i - 1));
          z.name = getDef<std::string>(e, "name", path, "");
          z.enabled = getDef<bool>(e, "enabled", path, true);
          z.levelDb = getDef<double>(e, "levelDb", path, 0.0);
          z.babbleFractionOffset = getDef<double>(e, "babbleFractionOffset", path, 0.0);
          clamp(z.levelDb, -24, 6, path + ".levelDb");
          clamp(z.babbleFractionOffset, -0.5, 0.5, path + ".babbleFractionOffset");
          o.zones.push_back(std::move(z));
        }
      }
      if (const json* l = getObj(*s, "limiter", "outputs")) {
        o.limiterEnabled = getOpt<bool>(*l, "enabled", "outputs.limiter");
        o.limiterCeilingDbtp = getOpt<double>(*l, "ceilingDbtp", "outputs.limiter");
        clampOpt(o.limiterCeilingDbtp, -6, -0.1, "outputs.limiter.ceilingDbtp");
      }
    }

    if (const json* r = getObj(j, "random", "")) {
      p.seed = getOpt<std::int64_t>(*r, "seed", "random");
      p.deterministic = getDef<bool>(*r, "deterministic", "random", false);
    }
    if (const json* r = getObj(j, "reliability", ""))
      p.fallbackPolicy = getOpt<std::string>(*r, "fallbackPolicy", "reliability");
  }
};

// ---- serialization ---------------------------------------------------------

json& sub(json& o, const char* key) {
  json& s = o[key];
  if (!s.is_object()) s = json::object();
  return s;
}

template <class T>
void setOpt(json& o, const char* key, const std::optional<T>& v) {
  if (v) o[key] = *v;
  else o[key] = nullptr;
}

void setStr(json& o, const char* key, const std::string& v) {
  if (!v.empty() || o.contains(key)) o[key] = v;
}

json targetToJson(const SpectrumTargetDef& t, json base) {
  if (!base.is_object()) base = json::object();
  if (!t.id.empty() || base.contains("id")) base["id"] = t.id;
  if (!t.displayName.empty() || base.contains("displayName")) base["displayName"] = t.displayName;
  if (!t.notes.empty() || base.contains("notes")) base["notes"] = t.notes;
  base["bandCentersHz"] = t.bandCentersHz;
  base["levelsDb"] = t.levelsDb;
  return base;
}

json toJson(const Preset& p) {
  json j = json::object();
  if (!p.rawJson.empty()) {
    json raw = json::parse(p.rawJson, nullptr, false);
    if (raw.is_object()) j = std::move(raw);
  }
  j["schema"] = p.schema;
  j["schemaVersion"] = p.schemaVersion;
  j["name"] = p.name;
  if (p.basedOn) {
    json& b = sub(j, "basedOn");
    setOpt(b, "area", p.basedOn->area);
    setOpt(b, "strategy", p.basedOn->strategy);
    setOpt(b, "factoryPresetVersion", p.basedOn->factoryPresetVersion);
  } else {
    j.erase("basedOn");
  }
  setStr(j, "created", p.created);
  setStr(j, "appVersion", p.appVersion);
  setStr(j, "notes", p.notes);
  j["area"] = p.area;
  j["strategy"] = p.strategy;

  {
    json& m = sub(j, "macros");
    setOpt(m, "strengthDb", p.macros.strengthDb);
    setOpt(m, "character", p.macros.character);
    setOpt(m, "voiceAmount", p.macros.voiceAmount);
    setOpt(m, "clearVoiceReduction", p.macros.clearVoiceReduction);
    setOpt(m, "voiceDiversity", p.macros.voiceDiversity);
    setOpt(sub(m, "mix"), "babbleFraction", p.macros.babbleFraction);
  }
  {
    json& t = sub(j, "talkers");
    const auto& o = p.talkers;
    setOpt(t, "pool", o.pool);
    setOpt(t, "meanActive", o.meanActive);
    setOpt(t, "minActive", o.minActive);
    setOpt(t, "maxActive", o.maxActive);
    setOpt(t, "segmentMinS", o.segmentMinS);
    setOpt(t, "segmentMaxS", o.segmentMaxS);
    setOpt(t, "segmentMedianS", o.segmentMedianS);
    setOpt(t, "maxInternalGapMs", o.maxInternalGapMs);
    setOpt(t, "gainVariationDb", o.gainVariationDb);
    setOpt(t, "fadeInMs", o.fadeInMs);
    setOpt(t, "fadeOutMs", o.fadeOutMs);
    setOpt(t, "reEntryCooldownS", o.reEntryCooldownS);
    setOpt(t, "segmentCooldownMin", o.segmentCooldownMin);
    setOpt(t, "poolRotationMin", o.poolRotationMin);
    setOpt(t, "languageAware", o.languageAware);
    setOpt(t, "targetVoiceProfile", o.targetVoiceProfile);
    setOpt(t, "multiVoiceK", o.multiVoiceK);
  }
  {
    json& s = sub(j, "stationary");
    setOpt(s, "enabled", p.stationary.enabled);
    setOpt(s, "spectrum", p.stationary.spectrum);
    setOpt(s, "seedMode", p.stationary.seedMode);
  }
  {
    json& s = sub(j, "spectrum");
    const auto& o = p.spectrum;
    setOpt(s, "target", o.target);
    setOpt(s, "speechMatchedProfile", o.speechMatchedProfile);
    if (o.custom) s["custom"] = targetToJson(*o.custom, s.contains("custom") ? s["custom"] : json::object());
    else s["custom"] = nullptr;
    setOpt(s, "lfLimitHz", o.lfLimitHz);
    setOpt(s, "hfLimitHz", o.hfLimitHz);
    setOpt(s, "lfTrimDb125", o.lfTrimDb125);
    json& c = sub(s, "correction");
    setOpt(c, "enabled", o.correction.enabled);
    setOpt(c, "speed", o.correction.speed);
    setOpt(c, "maxDb", o.correction.maxDb);
    setOpt(c, "rememberCorrection", o.correction.rememberCorrection);
  }
  {
    json& s = sub(j, "spatial");
    setOpt(s, "algorithm", p.spatial.algorithm);
    setOpt(s, "spread", p.spatial.spread);
    setOpt(s, "motion", p.spatial.motion);
    setOpt(s, "speakerVariation", p.spatial.speakerVariation);
    setOpt(s, "neighborhoodSize", p.spatial.neighborhoodSize);
  }
  {
    json& s = sub(j, "outputs");
    const auto& o = p.outputs;
    s["rememberDevice"] = o.rememberDevice;
    setOpt(s, "device", o.device);
    setOpt(s, "layout", o.layout);
    json oldCh = s.contains("channels") && s["channels"].is_array() ? s["channels"] : json::array();
    json ch = json::array();
    for (std::size_t i = 0; i < o.channels.size(); ++i) {
      const auto& c = o.channels[i];
      json e = i < oldCh.size() && oldCh[i].is_object() ? oldCh[i] : json::object();
      e["index"] = c.index;
      e["deviceChannel"] = c.deviceChannel;
      e["label"] = c.label;
      e["azimuthDeg"] = c.azimuthDeg;
      e["zone"] = c.zone;
      e["enabled"] = c.enabled;
      e["gainDb"] = c.gainDb;
      e["mute"] = c.mute;
      e["polarityInvert"] = c.polarityInvert;
      e["delayMs"] = c.delayMs;
      setOpt(e, "calEqProfile", c.calEqProfile);
      ch.push_back(std::move(e));
    }
    s["channels"] = std::move(ch);
    json oldZ = s.contains("zones") && s["zones"].is_array() ? s["zones"] : json::array();
    json zs = json::array();
    for (std::size_t i = 0; i < o.zones.size(); ++i) {
      const auto& z = o.zones[i];
      json e = i < oldZ.size() && oldZ[i].is_object() ? oldZ[i] : json::object();
      e["id"] = z.id;
      e["name"] = z.name;
      e["enabled"] = z.enabled;
      e["levelDb"] = z.levelDb;
      e["babbleFractionOffset"] = z.babbleFractionOffset;
      zs.push_back(std::move(e));
    }
    s["zones"] = std::move(zs);
    json& l = sub(s, "limiter");
    setOpt(l, "enabled", o.limiterEnabled);
    setOpt(l, "ceilingDbtp", o.limiterCeilingDbtp);
  }
  {
    json& r = sub(j, "random");
    setOpt(r, "seed", p.seed);
    r["deterministic"] = p.deterministic;
  }
  setOpt(sub(j, "reliability"), "fallbackPolicy", p.fallbackPolicy);
  if (!j.contains("overrides")) j["overrides"] = json::object();
  return j;
}

bool parseVersion(const std::string& s, int& major, int& minor) {
  auto dot = s.find('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= s.size()) return false;
  for (std::size_t i = 0; i < s.size(); ++i)
    if (i != dot && !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
  try {
    major = std::stoi(s.substr(0, dot));
    minor = std::stoi(s.substr(dot + 1));
  } catch (...) {
    return false;
  }
  return true;
}

}  // namespace

PresetParseResult parsePreset(std::string_view text) {
  PresetParseResult r;
  try {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) throw ParseError("invalid JSON");
    if (!j.is_object()) throw ParseError("expected JSON object at top level");
    std::string schema = getReq<std::string>(j, "schema", "");
    if (schema != "babbleforge.preset") throw ParseError("schema: expected \"babbleforge.preset\", got \"" + schema + "\"");
    std::string ver = getReq<std::string>(j, "schemaVersion", "");
    int major = 0, minor = 0;
    if (!parseVersion(ver, major, minor)) throw ParseError("schemaVersion: expected \"major.minor\", got \"" + ver + "\"");
    if (major > kPresetSchemaMajor)
      throw ParseError("Created by a newer BabbleForge (preset schema version " + ver + ")");
    if (major < 1) throw ParseError("schemaVersion: unsupported version " + ver);
    if (minor > kPresetSchemaMinor)
      r.warnings.push_back("Preset schema version " + ver + " is newer than supported " +
                           std::to_string(kPresetSchemaMajor) + "." + std::to_string(kPresetSchemaMinor) +
                           "; unknown fields are preserved but ignored");
    r.preset.schema = schema;
    r.preset.schemaVersion = ver;
    Parser{r.adjustments}.run(j, r.preset);
    r.preset.rawJson = j.dump();
    r.ok = true;
  } catch (const std::exception& e) {
    r.ok = false;
    r.error = e.what();
  }
  return r;
}

std::string canonicalPresetJson(const Preset& p) {
  json j = toJson(p);
  j.erase("contentHash");
  return j.dump(2);
}

std::string computePresetContentHash(const Preset& p) { return "sha256:" + sha256Hex(canonicalPresetJson(p)); }

std::string serializePreset(const Preset& p) {
  json j = toJson(p);
  j.erase("contentHash");
  std::string hash = "sha256:" + sha256Hex(j.dump(2));
  j["contentHash"] = hash;
  return j.dump(2);
}

}  // namespace bf
