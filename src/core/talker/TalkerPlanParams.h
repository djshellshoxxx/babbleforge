#pragma once
// Talker planner parameters (docs/TALKER_ENGINE.md §4, MASK_STRATEGIES.md §5.1, §6.2, §8.1).
#include <cstdint>
#include <string>

namespace bf {

enum class PlanMode : std::uint8_t {
    Stochastic,   // alternating-renewal ON/OFF slots (TALKER §4.1-4.6)
    FixedK,       // MultiVoiceMask: K always-on slots, speaker change, 150 ms overlap (§4.7)
    ContinuousN,  // LaboratoryMask.continuousN: N always-on slots, same speaker, 20 ms overlap
};

struct TalkerPlanParams {
    PlanMode mode = PlanMode::Stochastic;
    std::uint32_t pool = 12;        // P (selector pool size)
    double mean = 6.5;              // m (target mean k_a); FixedK/ContinuousN use maxActive
    std::uint32_t minActive = 4;    // min
    std::uint32_t maxActive = 9;    // max = V (number of slots); K / N in fixed modes
    double segMinS = 1.5, segMaxS = 20.0;
    double medianS = 5.5, sigmaLn = 0.45;
    double maxGapMs = 250.0;
    double gainSigmaDb = 2.0;       // per-segment level sigma (before the CVR multiplier)
    double fadeInMs = 150.0, fadeOutMs = 250.0;
    double reEntryCooldownMs = 800.0;
    double overlapMinMs = 150.0;    // handover overlap floor (Character / CVR)
    double talkerRefDbfs = -26.0;   // L_ref_talker: level of one speaking talker

    // CVR (MASK_STRATEGIES §6.2). Disabled values: cap <= 0, window 0, weight 1, phrase 0.
    std::uint32_t cvrMinFloor = 0;
    double cvrDominanceCapDb = 0.0;
    double cvrLevelSigmaMult = 1.0;
    double cvrOnsetWindowMs = 0.0;
    double cvrSoloRiskWeight = 1.0;
    double cvrMaxPhraseS = 0.0;

    std::uint64_t seed = 1;

    // Applies the CVR amount r in [0, 1] per MASK_STRATEGIES §6.2 (floor, cap, sigma
    // multiplier, forced overlap, onset window, solo-risk weight, max phrase continuity).
    void applyCvr(double r);

    std::uint32_t slots() const noexcept { return maxActive; }
    double targetMean() const noexcept {
        return mode == PlanMode::Stochastic ? mean : static_cast<double>(maxActive);
    }
    std::uint32_t effectiveMin() const noexcept;
    std::string toJsonString() const;  // canonical, for the plan hash
    std::uint64_t hash() const;
};

}  // namespace bf
