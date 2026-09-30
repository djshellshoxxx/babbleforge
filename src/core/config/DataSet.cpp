#include "core/config/DataSet.h"

#include <algorithm>

#include "core/config/AtomicFile.h"
#include "core/config/JsonRead.h"
#include "core/config/Sha256.h"

namespace bf {
namespace fs = std::filesystem;
using namespace detail;

namespace {

json readJson(const fs::path& p) {
  std::string text;
  std::string err;
  if (!readFile(p, text, &err)) throw ParseError(err);
  json j = json::parse(text, nullptr, false);
  if (j.is_discarded()) throw ParseError("invalid JSON");
  if (!j.is_object()) throw ParseError("expected JSON object at top level");
  return j;
}

std::vector<fs::path> jsonFiles(const fs::path& dir) {
  std::vector<fs::path> v;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return v;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_regular_file() && e.path().extension() == ".json") v.push_back(e.path());
  std::sort(v.begin(), v.end());
  return v;
}

AreaModel parseArea(const json& j) {
  AreaModel a;
  a.id = getReq<std::string>(j, "id", "");
  a.displayName = getDef<std::string>(j, "displayName", "", a.id);
  a.description = getDef<std::string>(j, "description", "", "");
  a.evidenceLabel = getDef<std::string>(j, "evidenceLabel", "", "");
  a.notes = getDef<std::string>(j, "notes", "", "");
  const json* t = getObj(j, "talkers", "");
  if (!t) throw ParseError("talkers: missing required object");
  a.talkers.meanActive = getReq<double>(*t, "meanActive", "talkers");
  a.talkers.pool = getReq<int>(*t, "pool", "talkers");
  a.talkers.minActive = getReq<int>(*t, "minActive", "talkers");
  a.talkers.maxActive = getReq<int>(*t, "maxActive", "talkers");
  a.talkers.recommendedRange = getVec<double>(*t, "recommendedRange", "talkers");
  const json* mix = getObj(j, "mix", "");
  if (!mix) throw ParseError("mix: missing required object");
  a.balancedStationaryFraction = getReq<double>(*mix, "balancedStationaryFraction", "mix");
  if (const json* m = getObj(j, "macros", "")) {
    a.macros.character = getReq<double>(*m, "character", "macros");
    a.macros.voiceAmount = getReq<double>(*m, "voiceAmount", "macros");
    a.macros.clearVoiceReduction = getReq<double>(*m, "clearVoiceReduction", "macros");
    a.macros.voiceDiversity = getReq<std::string>(*m, "voiceDiversity", "macros");
  }
  if (const json* s = getObj(j, "spectrum", "")) {
    a.spectrum.target = getReq<std::string>(*s, "target", "spectrum");
    a.spectrum.lfLimitHz = getReq<double>(*s, "lfLimitHz", "spectrum");
    a.spectrum.lfTrimDb125 = getReq<double>(*s, "lfTrimDb125", "spectrum");
    a.spectrum.useSpeechMatchedIfAvailable = getDef<bool>(*s, "useSpeechMatchedIfAvailable", "spectrum", false);
  }
  if (const json* s = getObj(j, "spatial", "")) {
    a.spatial.algorithm = getReq<std::string>(*s, "algorithm", "spatial");
    a.spatial.spread = getReq<double>(*s, "spread", "spatial");
    a.spatial.motion = getReq<double>(*s, "motion", "spatial");
    a.spatial.speakerVariation = getReq<std::string>(*s, "speakerVariation", "spatial");
    a.spatial.neighborhoodSize = getReq<int>(*s, "neighborhoodSize", "spatial");
  }
  if (const json* o = getObj(j, "outputs", "")) {
    a.outputs.minRecommended = getReq<int>(*o, "minRecommended", "outputs");
    a.outputs.minWithoutAdvisory = getReq<int>(*o, "minWithoutAdvisory", "outputs");
  }
  if (const json* l = getObj(j, "level", "")) {
    a.level.defaultStrengthDb = getReq<double>(*l, "defaultStrengthDb", "level");
    a.level.maxSimpleStrengthDb = getReq<double>(*l, "maxSimpleStrengthDb", "level");
  }
  if (const json* adv = getArr(j, "advisories", "")) {
    std::size_t i = 0;
    for (const auto& e : *adv) {
      std::string p = "advisories[" + std::to_string(i++) + "]";
      a.advisories.push_back({getReq<std::string>(e, "when", p), getReq<std::string>(e, "text", p)});
    }
  }
  return a;
}

std::optional<std::pair<double, double>> getRange(const json& o, const char* key, const std::string& path) {
  auto r = getVec<double>(o, key, path);
  if (r.empty()) return std::nullopt;
  if (r.size() != 2) throw ParseError(join(path, key) + ": expected 2 elements");
  if (r[0] > r[1]) throw ParseError(join(path, key) + ": lower bound exceeds upper bound");
  return std::make_pair(r[0], r[1]);
}

std::map<std::string, std::string> getInterpMap(const json& o, const char* key) {
  std::map<std::string, std::string> m;
  if (const json* im = getObj(o, key, ""))
    for (auto it = im->begin(); it != im->end(); ++it) {
      std::string v = conv<std::string>(it.value(), std::string(key) + "." + it.key());
      if (v != "linear" && v != "log" && v != "step")
        throw ParseError(std::string(key) + "." + it.key() + ": expected linear|log|step");
      m[it.key()] = v;
    }
  return m;
}

StrategyDef parseStrategy(const json& j) {
  StrategyDef s;
  s.id = getReq<std::string>(j, "id", "");
  s.klass = getReq<std::string>(j, "class", "");
  s.displayName = getDef<std::string>(j, "displayName", "", s.id);
  s.descriptionSimple = getDef<std::string>(j, "descriptionSimple", "", "");
  s.evidenceLabel = getDef<std::string>(j, "evidenceLabel", "", "");
  if (const json* o = getObj(j, "overrides", "")) {
    auto& ov = s.overrides;
    const std::string P = "overrides";
    ov.babbleFraction = getOpt<double>(*o, "babbleFraction", P);
    ov.character = getOpt<double>(*o, "character", P);
    ov.babbleFractionDelta = getOpt<double>(*o, "babbleFractionDelta", P);
    ov.characterDefault = getOpt<double>(*o, "characterDefault", P);
    ov.motionScale = getOpt<double>(*o, "motionScale", P);
    ov.multiVoiceK = getOpt<int>(*o, "multiVoiceK", P);
    ov.segmentDurationMedianS = getOpt<double>(*o, "segmentDurationMedianS", P);
    ov.segmentDurationSigmaLn = getOpt<double>(*o, "segmentDurationSigmaLn", P);
    ov.handoverOverlapMs = getOpt<double>(*o, "handoverOverlapMs", P);
    ov.continuousNOverlapMs = getOpt<double>(*o, "continuousNOverlapMs", P);
    ov.continuousNMaxGapMs = getOpt<double>(*o, "continuousNMaxGapMs", P);
    ov.continuousNDefault = getOpt<int>(*o, "continuousNDefault", P);
  }
  if (const json* f = getObj(j, "forced", "")) {
    auto& fo = s.forced;
    const std::string P = "forced";
    fo.characterRange = getRange(*f, "characterRange", P);
    fo.babbleFractionRange = getRange(*f, "babbleFractionRange", P);
    fo.motionRange = getRange(*f, "motionRange", P);
    fo.stationaryFractionRange = getRange(*f, "stationaryFractionRange", P);
    fo.multiVoiceKRange = getRange(*f, "multiVoiceKRange", P);
    fo.multiVoiceKOptions = getVec<int>(*f, "multiVoiceKOptions", P);
    fo.fallbackPolicy = getOpt<std::string>(*f, "fallbackPolicy", P);
    fo.mixUserLocked = getDef<bool>(*f, "mixUserLocked", P, false);
    fo.cvrDisabledUnlessExplicit = getDef<bool>(*f, "cvrDisabledUnlessExplicit", P, false);
    fo.seedRequired = getDef<bool>(*f, "seedRequired", P, false);
    fo.poolRotation = getDef<bool>(*f, "poolRotation", P, true);
    fo.lockedForResearch = getDef<bool>(*f, "lockedForResearch", P, false);
  }
  return s;
}

SpectrumTargetDef parseTarget(const json& j) {
  SpectrumTargetDef t;
  t.id = getReq<std::string>(j, "id", "");
  t.displayName = getDef<std::string>(j, "displayName", "", t.id);
  t.notes = getDef<std::string>(j, "notes", "", "");
  t.bandCentersHz = getVec<double>(j, "bandCentersHz", "");
  t.levelsDb = getVec<double>(j, "levelsDb", "");
  if (t.bandCentersHz.empty() || t.bandCentersHz.size() != t.levelsDb.size())
    throw ParseError("bandCentersHz/levelsDb: must be non-empty and of equal length");
  return t;
}

EngineDefaults parseEngineDefaults(const json& j) {
  EngineDefaults e;
  e.lRefDbfs = getDef<double>(j, "lRefDbfs", "", e.lRefDbfs);
  if (const json* s = getObj(j, "strength", "")) {
    if (const json* sim = getObj(*s, "simple", "strength")) {
      e.simpleMinDb = getDef<double>(*sim, "minDb", "strength.simple", e.simpleMinDb);
      e.simpleMaxDb = getDef<double>(*sim, "maxDb", "strength.simple", e.simpleMaxDb);
      if (const json* l = getObj(*sim, "labels", "strength.simple"))
        for (auto it = l->begin(); it != l->end(); ++it)
          e.strengthLabels[it.key()] = conv<double>(it.value(), "strength.simple.labels." + it.key());
    }
    if (const json* adv = getObj(*s, "advanced", "strength")) {
      e.advancedMinDb = getDef<double>(*adv, "minDb", "strength.advanced", e.advancedMinDb);
      e.advancedMaxDb = getDef<double>(*adv, "maxDb", "strength.advanced", e.advancedMaxDb);
    }
  }
  if (const json* l = getObj(j, "limiter", ""))
    e.limiterCeilingDbtp = getDef<double>(*l, "ceilingDbtp", "limiter", e.limiterCeilingDbtp);
  e.fallbackPolicy = getDef<std::string>(j, "fallbackPolicy", "", e.fallbackPolicy);
  if (const json* c = getObj(j, "correctionSpeeds", ""))
    for (auto it = c->begin(); it != c->end(); ++it)
      e.correctionTimeConstantS[it.key()] = getReq<double>(it.value(), "timeConstantS", "correctionSpeeds." + it.key());
  if (const json* s = getObj(j, "spectrum", "")) {
    e.maxCorrectionDb = getDef<double>(*s, "maxCorrectionDb", "spectrum", e.maxCorrectionDb);
    e.lfLimitHz = getDef<double>(*s, "lfLimitHz", "spectrum", e.lfLimitHz);
  }
  return e;
}

std::string computeHash(const fs::path& dir) {
  std::vector<std::pair<std::string, fs::path>> files;
  for (const auto& e : fs::recursive_directory_iterator(dir))
    if (e.is_regular_file()) files.emplace_back(fs::relative(e.path(), dir).generic_string(), e.path());
  std::sort(files.begin(), files.end());
  Sha256 h;
  for (const auto& [rel, path] : files) {
    std::string bytes;
    std::string err;
    if (!readFile(path, bytes, &err)) throw ParseError(err);
    h.update(rel);
    const std::uint8_t zero = 0;
    h.update(&zero, 1);
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<std::uint8_t>(std::uint64_t(bytes.size()) >> (8 * i));
    h.update(len, 8);
    h.update(bytes);
  }
  return h.finishHex();
}

}  // namespace

DataSetResult loadDataSet(const fs::path& dir) {
  DataSetResult r;
  fs::path current;
  try {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) throw ParseError("data directory not found: " + dir.string());
    current = dir / "engine_defaults.json";
    r.data.engineDefaults = parseEngineDefaults(readJson(current));

    for (const auto& p : jsonFiles(dir / "areas")) {
      current = p;
      AreaModel a = parseArea(readJson(p));
      std::string id = a.id;
      r.data.areas[id] = std::move(a);
    }
    for (const auto& p : jsonFiles(dir / "strategies")) {
      current = p;
      StrategyDef s = parseStrategy(readJson(p));
      std::string id = s.id;
      r.data.strategies[id] = std::move(s);
    }
    for (const auto& p : jsonFiles(dir / "targets")) {
      current = p;
      SpectrumTargetDef t = parseTarget(readJson(p));
      std::string id = t.id;
      r.data.targets[id] = std::move(t);
    }

    current = dir / "macros" / "character_anchors.json";
    const json anchorsDoc = readJson(current);
    const json noAnchors = json::array();
    const json* anchorsArr = getArr(anchorsDoc, "anchors", "");
    for (const auto& e : anchorsArr ? *anchorsArr : noAnchors) {
      CharacterAnchor a;
      a.c = getReq<double>(e, "c", "anchors");
      a.label = getDef<std::string>(e, "label", "anchors", "");
      a.meanActiveFactor = getReq<double>(e, "meanActiveFactor", "anchors");
      a.maxInternalGapMs = getReq<double>(e, "maxInternalGapMs", "anchors");
      a.segmentDurationMedianS = getReq<double>(e, "segmentDurationMedianS", "anchors");
      a.segmentDurationSigmaLn = getReq<double>(e, "segmentDurationSigmaLn", "anchors");
      a.perSegmentLevelSigmaDb = getReq<double>(e, "perSegmentLevelSigmaDb", "anchors");
      a.reEntryCooldownS = getReq<double>(e, "reEntryCooldownS", "anchors");
      a.overlapOnHandoverMs = getReq<double>(e, "overlapOnHandoverMs", "anchors");
      a.stationaryFractionOffset = getReq<double>(e, "stationaryFractionOffset", "anchors");
      a.spatialMotionRate = getReq<double>(e, "spatialMotionRate", "anchors");
      a.fadeInMs = getReq<double>(e, "fadeInMs", "anchors");
      a.fadeOutMs = getReq<double>(e, "fadeOutMs", "anchors");
      const json* mn = getObj(e, "minActive", "anchors");
      const json* mx = getObj(e, "maxActive", "anchors");
      if (!mn || !mx) throw ParseError("anchors: minActive/maxActive formula objects required");
      a.minActiveFloor = getReq<double>(*mn, "floor", "anchors.minActive");
      a.minActiveFactor = getReq<double>(*mn, "factor", "anchors.minActive");
      a.maxActiveFactor = getReq<double>(*mx, "factor", "anchors.maxActive");
      r.data.characterAnchors.push_back(a);
    }
    if (r.data.characterAnchors.empty()) throw ParseError("anchors: empty");
    r.data.characterInterp = getInterpMap(anchorsDoc, "interpolation");
    r.data.characterStepThresholds = getVec<double>(anchorsDoc, "stepThresholds", "");
    r.data.levelVarTruncationSigma = getDef<double>(anchorsDoc, "levelVarTruncationSigma", "", 2.0);

    current = dir / "macros" / "cvr_mapping.json";
    {
      json j = readJson(current);
      if (const json* arr = getArr(j, "mappings", ""))
        for (const auto& e : *arr) {
          CvrMapping m;
          m.r = getReq<double>(e, "r", "mappings");
          m.label = getDef<std::string>(e, "label", "mappings", "");
          m.minActiveFloor = getReq<double>(e, "minActiveFloor", "mappings");
          m.dominanceCapDb = getReq<double>(e, "dominanceCapDb", "mappings");
          m.levelSigmaMultiplier = getReq<double>(e, "levelSigmaMultiplier", "mappings");
          m.forcedOverlapAtHandoverMs = getReq<double>(e, "forcedOverlapAtHandoverMs", "mappings");
          m.onsetMaskingWindowMs = getReq<double>(e, "onsetMaskingWindowMs", "mappings");
          m.segmentSelectionWeightForSoloRisk = getReq<double>(e, "segmentSelectionWeightForSoloRisk", "mappings");
          m.minimumStationaryFraction = getReq<double>(e, "minimumStationaryFraction", "mappings");
          m.maxPhraseContinuityS = getReq<double>(e, "maxPhraseContinuityS", "mappings");
          r.data.cvrMappings.push_back(m);
        }
      if (r.data.cvrMappings.empty()) throw ParseError("mappings: empty");
      r.data.cvrInterp = getInterpMap(j, "interpolation");
      r.data.cvrStepThresholds = getVec<double>(j, "stepThresholds", "");
    }

    current = dir / "macros" / "voice_amount.json";
    {
      json j = readJson(current);
      if (const json* arr = getArr(j, "anchors", ""))
        for (const auto& e : *arr) {
          VoiceAmountAnchor a;
          a.v = getReq<double>(e, "v", "anchors");
          a.label = getDef<std::string>(e, "label", "anchors", "");
          a.meanActiveTalkers = getOpt<double>(e, "meanActiveTalkers", "anchors");
          a.fromArea = getDef<bool>(e, "area", "anchors", false);
          a.interp = getDef<std::string>(e, "interp", "anchors", "log");
          r.data.voiceAmountAnchors.push_back(a);
        }
      if (r.data.voiceAmountAnchors.empty()) throw ParseError("anchors: empty");
      auto& vr = r.data.voiceAmountRules;
      vr.interp = getDef<std::string>(j, "interp", "", vr.interp);
      if (auto rg = getRange(j, "meanActiveRange", "")) {
        vr.meanMin = rg->first;
        vr.meanMax = rg->second;
      }
      vr.availableSpeakerMargin = getDef<int>(j, "availableSpeakerMargin", "", vr.availableSpeakerMargin);
      if (const json* pl = getObj(j, "pool", "")) {
        vr.poolFactor = getDef<double>(*pl, "factor", "pool", vr.poolFactor);
        vr.poolOffset = getDef<double>(*pl, "offset", "pool", vr.poolOffset);
      }
    }

    if (r.data.areas.empty()) throw ParseError("no areas found");
    if (r.data.strategies.empty()) throw ParseError("no strategies found");

    current = dir;
    r.data.dataSetHash = computeHash(dir);
    r.ok = true;
  } catch (const std::exception& e) {
    r.ok = false;
    r.error = current.string() + ": " + e.what();
  }
  return r;
}

}  // namespace bf
