#pragma once
// End-to-end configuration pipeline (PRESETS.md §2): compose() (steps 1-3, 5, 6 and id
// checks) -> MaskStrategy::validate() (step 6) -> MaskStrategy::buildPlan() (step 4 macros
// Voice Amount -> Character -> CVR, then the preset's step-5 overrides on top).
// Step 7 (degrade) is applied at runtime via MaskStrategy::degrade().
#include <string>
#include <vector>

#include "core/config/Compose.h"
#include "core/strategy/MaskStrategy.h"

namespace bf {

struct ComposedPlan {
  bool ok = false;
  std::string error;
  MaskRenderPlan plan;
  StrategyParams params;  // validated user parameters
  MacroState macros;      // validated macro state
  std::vector<Adjustment> adjustments;  // validate() + plan-time adjustments
  std::vector<std::string> conflicts;
};

// Maps the preset's explicit (non-null) fields to strategy parameters / macro state.
StrategyParams strategyParamsFromPreset(const Preset& preset);
MacroState macroStateFromPreset(const Preset& preset);

ComposedPlan composePlan(const DataSet& ds, const Preset& preset, const CorpusSummary& corpus,
                         const OutputLayout& layout);

}  // namespace bf
