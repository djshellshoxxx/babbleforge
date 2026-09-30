#pragma once
// Talker-count modes (docs/TALKER_ENGINE.md §5).
#include <optional>

namespace bf {

struct TalkerCountMode {
  int n = 0;             // GUI mode value (talker count)
  bool fixedK = true;    // true: MultiVoice K = n; false: stochastic planner
  int pool = 0;          // P (capped at the corpus speaker count when one is given)
  double mean = 0.0;     // m
  int minActive = 0;
  int maxActive = 0;     // V
  int labContinuousN = 0;
  bool tabulated = false;  // one of the table rows (1, 2, 3, 4, 5, 7, 8, 12, 16, 24)
};

// Table rows for 1, 2, 3, 4, 5, 7 (MultiVoice K) and 8, 12, 16, 24 (stochastic).
// Custom n (clamped to 1..64 as for Laboratory continuousN):
//  - n <= 7: MultiVoice K = n with P = n + 5 for n = 6, the tabulated values otherwise
//    (interpolation of the table; the table itself gives no rule for K = 6);
//  - n >= 8: the stochastic rule min = round(0.75 n), max = round(1.25 n),
//    P = max(2n + 2, max + 4), clamped to the Advanced ranges (P <= 64, max <= 48, m <= 32).
// availableSpeakers > 0 caps the pool (RELIABILITY.md §3 handles the rest).
TalkerCountMode talkerCountMode(int n, std::optional<int> availableSpeakers = std::nullopt);

// Pool used by MultiVoiceMask for K voices: the table value for K in 1..7, the stochastic-rule
// pool for K >= 8 (e.g. 20 for "Dense Multi-Voice" K = 9); always >= K + 3 (minCorpusSpeakers).
int multiVoicePool(int k);

}  // namespace bf
