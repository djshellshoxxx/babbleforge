#include "core/strategy/StrategyTypes.h"

#include <nlohmann/json.hpp>

#include "core/corpus/CorpusSnapshot.h"
#include "core/random/Random.h"

namespace bf {

std::string_view toString(StrategyId id) noexcept {
  switch (id) {
    case StrategyId::Balanced: return "balanced";
    case StrategyId::Natural: return "natural";
    case StrategyId::Dense: return "dense";
    case StrategyId::SpeechNoise: return "speech_noise";
    case StrategyId::MultiVoice: return "multi_voice";
    case StrategyId::Hybrid: return "hybrid";
    case StrategyId::Research: return "research";
  }
  return "?";
}

std::string_view toString(StrategyClass c) noexcept {
  switch (c) {
    case StrategyClass::StationarySpeechMask: return "StationarySpeechMask";
    case StrategyClass::MultiVoiceMask: return "MultiVoiceMask";
    case StrategyClass::BabbleMask: return "BabbleMask";
    case StrategyClass::HybridMask: return "HybridMask";
    case StrategyClass::NaturalBabbleMask: return "NaturalBabbleMask";
    case StrategyClass::LaboratoryMask: return "LaboratoryMask";
  }
  return "?";
}

std::string_view toString(FallbackPolicy p) noexcept {
  switch (p) {
    case FallbackPolicy::Strict: return "strict";
    case FallbackPolicy::Safe: return "safe";
    case FallbackPolicy::Continuous: return "continuous";
  }
  return "?";
}

std::string_view toString(LabSubMode m) noexcept {
  switch (m) {
    case LabSubMode::ContinuousN: return "continuousN";
    case LabSubMode::Stochastic: return "stochastic";
    case LabSubMode::Ssn: return "ssn";
    case LabSubMode::Pink: return "pink";
    case LabSubMode::Hybrid: return "hybrid";
    case LabSubMode::LtassMatchedBabble: return "ltassMatchedBabble";
  }
  return "?";
}

std::optional<StrategyId> strategyIdFromString(std::string_view s) noexcept {
  for (auto id : {StrategyId::Balanced, StrategyId::Natural, StrategyId::Dense, StrategyId::SpeechNoise,
                  StrategyId::MultiVoice, StrategyId::Hybrid, StrategyId::Research})
    if (toString(id) == s) return id;
  return std::nullopt;
}

std::optional<StrategyClass> strategyClassFromString(std::string_view s) noexcept {
  for (auto c : {StrategyClass::StationarySpeechMask, StrategyClass::MultiVoiceMask, StrategyClass::BabbleMask,
                 StrategyClass::HybridMask, StrategyClass::NaturalBabbleMask, StrategyClass::LaboratoryMask})
    if (toString(c) == s) return c;
  return std::nullopt;
}

std::optional<FallbackPolicy> fallbackPolicyFromString(std::string_view s) noexcept {
  for (auto p : {FallbackPolicy::Strict, FallbackPolicy::Safe, FallbackPolicy::Continuous})
    if (toString(p) == s) return p;
  return std::nullopt;
}

std::optional<SpeakerVariation> speakerVariationFromString(std::string_view s) noexcept {
  if (s == "low") return SpeakerVariation::Low;
  if (s == "medium") return SpeakerVariation::Medium;
  if (s == "high") return SpeakerVariation::High;
  return std::nullopt;
}

CorpusSummary CorpusSummary::from(const CorpusSnapshot& snap) {
  CorpusSummary c;
  c.corpusVersion = snap.corpusVersion();
  for (std::size_t s = 0; s < snap.numSpeakers(); ++s)
    if (snap.speakerHealthy(static_cast<SpeakerId>(s))) ++c.availableSpeakers;
  return c;
}

std::string MaskRenderPlan::canonicalJson() const {
  nlohmann::ordered_json j;
  j["strategyId"] = std::string(toString(strategyId));
  j["strategyClass"] = std::string(toString(strategyClass));
  j["areaId"] = areaId;
  j["macros"] = {{"voiceAmount", voiceAmount}, {"character", character}, {"cvr", clearVoiceReduction}};
  j["babbleEnabled"] = babbleEnabled;
  TalkerPlanParams t = talkers;
  t.seed = 0;  // the seed changes per start; it does not change the plan's content class
  j["talkers"] = nlohmann::ordered_json::parse(t.toJsonString());
  j["levelVarTruncationSigma"] = levelVarTruncationSigma;
  j["labMode"] = std::string(toString(labMode));
  j["strictSpectralMatch"] = strictSpectralMatch;
  j["selector"] = {{"diversity", selector.diversity},
                   {"languageAware", selector.languageAware},
                   {"poolRotation", selector.poolRotation},
                   {"relaxRecentSpeakerRule", selector.relaxRecentSpeakerRule},
                   {"soloRiskWeight", selector.soloRiskWeight}};
  j["speakersNeeded"] = speakersNeeded;
  j["stationary"] = {{"enabled", stationary.enabled},
                     {"keepHot", stationary.keepHot},
                     {"spectrumId", stationary.spectrumId},
                     {"seedStream", stationary.seedStream}};
  nlohmann::ordered_json bands = nlohmann::ordered_json::array();
  for (float v : target.thirdOctDb) bands.push_back(v);
  j["target"] = {{"id", target.id},
                 {"thirdOctDb", bands},
                 {"lfLimitHz", target.lfLimitHz},
                 {"hfLimitHz", target.hfLimitHz},
                 {"lfTrimDb125", target.lfTrimDb125}};
  nlohmann::ordered_json zones = nlohmann::ordered_json::array();
  for (double z : mix.zoneBabbleFraction) zones.push_back(z);
  j["mix"] = {{"babbleFraction", mix.babbleFraction},
              {"baseBabbleFraction", mix.baseBabbleFraction},
              {"zones", zones},
              {"userLocked", mix.userLocked},
              {"stationaryCompensation", mix.stationaryCompensation}};
  j["spatial"] = {{"request", spatial.algorithmRequest},
                  {"algorithm", static_cast<int>(spatial.algorithm)},
                  {"spread", spatial.params.spread},
                  {"motion", spatial.params.motion},
                  {"variation", static_cast<int>(spatial.params.variation)},
                  {"kn", spatial.params.neighbourhoodSize},
                  {"activeOutputs", spatial.activeOutputs}};
  j["level"] = {{"lRefDbfs", level.lRefDbfs},
                {"strengthDb", level.strengthDb},
                {"limiterEnabled", level.limiterEnabled},
                {"limiterCeilingDbtp", level.limiterCeilingDbtp}};
  j["fallback"] = std::string(toString(fallback));
  return j.dump();
}

std::uint64_t MaskRenderPlan::computeHash() const { return fnv1a64(canonicalJson()); }

}  // namespace bf
