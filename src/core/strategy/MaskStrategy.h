#pragma once
// MaskStrategy interface and the six strategy classes (docs/MASK_STRATEGIES.md §2).
// Strategies are non-RT policy objects: validate() clamps user parameters (never rejects),
// buildPlan() is a pure function (no I/O, no randomness) producing a MaskRenderPlan, and
// degrade() applies the fallback policy transformations of RELIABILITY.md §3-4.
//
// Strategy-specific rules (PRESETS.md §5) are data: resources/data/strategies/*.json
// ("overrides": babbleFraction, babbleFractionDelta, characterDefault, motionScale,
// multiVoiceK, ...; "forced": characterRange, babbleFractionRange, motionRange,
// stationaryFractionRange, fallbackPolicy, mixUserLocked, ...).
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/config/DataSet.h"
#include "core/spatial/OutputLayout.h"
#include "core/strategy/MacroTables.h"
#include "core/strategy/StrategyTypes.h"

namespace bf {

// Shared, immutable inputs of every strategy (copied from a DataSet).
struct StrategyEnv {
  EngineDefaults engineDefaults;
  MacroTables macros;
  std::map<std::string, SpectrumTargetDef> targets;
  static std::shared_ptr<const StrategyEnv> fromDataSet(const DataSet& ds);
};

class MaskStrategy {  // control thread only
public:
  MaskStrategy(StrategyDef def, std::shared_ptr<const StrategyEnv> env);
  virtual ~MaskStrategy() = default;

  virtual StrategyId id() const;
  virtual StrategyClass strategyClass() const = 0;
  virtual StrategyCaps capabilities() const = 0;
  // Clamp user parameters and macros to the §10 ranges, the strategy's forced ranges and the
  // invariants min <= mean <= max <= pool <= available; never rejects.
  virtual ValidationResult validate(StrategyParams& params, MacroState& macros, const CorpusSummary& corpus) const;
  // Pure: same inputs -> same plan (and planHash).
  virtual MaskRenderPlan buildPlan(const AreaModel& area, const StrategyParams& params, const MacroState& macros,
                                   const CorpusSummary& corpus, const OutputLayout& layout) const = 0;
  // Degraded-mode transformation (fallback policy); pure.
  virtual MaskRenderPlan degrade(const MaskRenderPlan& plan, const DegradeReason& reason) const;

  const StrategyDef& definition() const noexcept { return def_; }
  const StrategyEnv& env() const noexcept { return *env_; }

protected:
  // Common plan skeleton: identity, spectrum target, spatial, level, fallback, selector hints.
  MaskRenderPlan basePlan(const AreaModel& area, const StrategyParams& params, const OutputLayout& layout) const;
  // Stochastic babble talker model: Voice Amount -> Character -> CVR -> user overrides.
  void buildStochasticTalkers(MaskRenderPlan& plan, const AreaModel& area, const StrategyParams& params,
                              double v, double c, double r, const CorpusSummary& corpus) const;
  // Base b per strategy data (absolute override or area b + delta, forced range), user b.
  double baseBabbleFraction(const AreaModel& area, const StrategyParams& params, bool& locked) const;
  // Final mix: Δs (Character) and CVR minimum stationary fraction when not locked.
  void applyMix(MaskRenderPlan& plan, const StrategyParams& params, double baseB, bool locked, double stationaryOffset,
                double cvrMinStationary) const;
  // Spatial motion from area, strategy scale, Character rate, user override.
  void applyMotion(MaskRenderPlan& plan, const AreaModel& area, const StrategyParams& params, double c) const;
  // Speakers needed, stationary hot/enabled flags, final invariants, planHash.
  void finalize(MaskRenderPlan& plan) const;

  double resolveCharacter(const AreaModel& area, const MacroState& m) const;
  bool allowsStationary() const;

  StrategyDef def_;
  std::shared_ptr<const StrategyEnv> env_;
};

// Stochastic babble strategies (planner §4.1-4.6) sharing one policy; they differ only in
// their data (PRESETS.md §5) and capability flags.
class StochasticBabbleStrategy : public MaskStrategy {
public:
  using MaskStrategy::MaskStrategy;
  MaskRenderPlan buildPlan(const AreaModel& area, const StrategyParams& params, const MacroState& macros,
                           const CorpusSummary& corpus, const OutputLayout& layout) const override;
};

class HybridMask final : public StochasticBabbleStrategy {  // Balanced, Hybrid
public:
  using StochasticBabbleStrategy::StochasticBabbleStrategy;
  StrategyClass strategyClass() const override { return StrategyClass::HybridMask; }
  StrategyCaps capabilities() const override;
};

class BabbleMask final : public StochasticBabbleStrategy {  // Dense
public:
  using StochasticBabbleStrategy::StochasticBabbleStrategy;
  StrategyClass strategyClass() const override { return StrategyClass::BabbleMask; }
  StrategyCaps capabilities() const override;
};

class NaturalBabbleMask final : public StochasticBabbleStrategy {  // Natural
public:
  using StochasticBabbleStrategy::StochasticBabbleStrategy;
  StrategyClass strategyClass() const override { return StrategyClass::NaturalBabbleMask; }
  StrategyCaps capabilities() const override;
};

class StationarySpeechMask final : public MaskStrategy {  // Speech Noise
public:
  using MaskStrategy::MaskStrategy;
  StrategyClass strategyClass() const override { return StrategyClass::StationarySpeechMask; }
  StrategyCaps capabilities() const override;
  MaskRenderPlan buildPlan(const AreaModel& area, const StrategyParams& params, const MacroState& macros,
                           const CorpusSummary& corpus, const OutputLayout& layout) const override;
};

class MultiVoiceMask final : public MaskStrategy {  // Multi-Voice 3/5/7, Dense Multi-Voice 9
public:
  using MaskStrategy::MaskStrategy;
  StrategyClass strategyClass() const override { return StrategyClass::MultiVoiceMask; }
  StrategyCaps capabilities() const override;
  MaskRenderPlan buildPlan(const AreaModel& area, const StrategyParams& params, const MacroState& macros,
                           const CorpusSummary& corpus, const OutputLayout& layout) const override;
  int defaultK() const;
};

class LaboratoryMask final : public MaskStrategy {  // Research
public:
  using MaskStrategy::MaskStrategy;
  StrategyClass strategyClass() const override { return StrategyClass::LaboratoryMask; }
  StrategyCaps capabilities() const override;
  ValidationResult validate(StrategyParams& params, MacroState& macros, const CorpusSummary& corpus) const override;
  MaskRenderPlan buildPlan(const AreaModel& area, const StrategyParams& params, const MacroState& macros,
                           const CorpusSummary& corpus, const OutputLayout& layout) const override;
};

// Minimum corpus speakers for a plan (MASK_STRATEGIES.md §2.3).
int minCorpusSpeakersFor(const MaskRenderPlan& plan);
// RELIABILITY.md §3: max(P_plan, maxActive + 4); LaboratoryMask continuousN: exactly N.
int speakersNeededFor(const MaskRenderPlan& plan);

// Factory: nullptr for an unknown strategy id or class.
std::unique_ptr<MaskStrategy> makeStrategy(const StrategyDef& def, std::shared_ptr<const StrategyEnv> env);
std::unique_ptr<MaskStrategy> makeStrategy(const DataSet& ds, const std::string& strategyId);

// All strategies of a DataSet, sharing one StrategyEnv.
class StrategyLibrary {
public:
  explicit StrategyLibrary(const DataSet& ds);
  const MaskStrategy* find(const std::string& strategyId) const;
  std::vector<std::string> ids() const;

private:
  std::shared_ptr<const StrategyEnv> env_;
  std::map<std::string, std::unique_ptr<MaskStrategy>> strategies_;
};

}  // namespace bf
