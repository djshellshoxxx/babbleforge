#pragma once
// Slow spectral correction of the babble component (docs/SPECTRUM_ENGINE.md section 6.3).
// Pure logic, analysis thread, one step() per 5 s block. All band arrays are the 21 operating
// bands (100 Hz..10 kHz). Level offsets are removed with the target-power-weighted mean, so the
// loop only corrects spectral SHAPE.
#include <array>
#include <cstdint>

#include "core/analysis/SpectrumAnalyzer.h"

namespace bf {

enum class CorrectionSpeed { Slow, Normal, Fast };

struct CorrectionConfig {
    CorrectionSpeed speed = CorrectionSpeed::Normal;
    bool strict = false;            // LaboratoryMask Strict pre-roll: tau_c = 20 s (slew 3 dB/min)
    double blockSeconds = 5.0;      // T_block
    double deadbandDb = 0.3;
    double clampDb = 6.0;
    double redesignThresholdDb = 0.1;
    double redesignMinSpacingSeconds = 15.0;
    double holdoffSeconds = 120.0;  // freeze after notifyPlanChange() (2 x 60 s)
    int oscillationFlips = 4;       // more than this many sign alternations ...
    double oscillationWindowSeconds = 600.0;   // ... in 10 min
    double oscillationHalveSeconds = 1800.0;   // halve the band's gain for 30 min
    double tauSeconds() const noexcept;        // closed-loop time constant
    double slewDbPerMin() const noexcept;
};

// Freeze conditions evaluated by the caller (section 6.3 "stability protection").
struct CorrectionFreeze {
    bool lowLevel = false;         // babble RMS over the block < L_ref - 20 dB
    bool lowBabbleFraction = false;  // babble fraction b < 0.05
    bool crossfading = false;      // strategy/plan crossfade in progress
    bool any() const noexcept { return lowLevel || lowBabbleFraction || crossfading; }
};

struct CorrectionStepResult {
    OperatingBands correction{};   // C[b], dB (to be added to the babble EQ)
    bool frozen = false;           // integrator not updated (flags or hold-off)
    bool redesignNeeded = false;   // accumulated change >= 0.1 dB and >= 15 s since last redesign
    bool large = false;            // any |C| > 4 dB ("spectrum.correctionLarge")
    std::uint32_t guardTriggered = 0;  // bit mask of bands whose oscillation guard tripped this step
    std::uint32_t guardActive = 0;     // bit mask of bands currently at halved gain
    double maxDeviationDb = 0.0;   // max |smoothed deviation| this step (diagnostic)
};

class SpectralCorrection {
public:
    explicit SpectralCorrection(const CorrectionConfig& cfg = {}) noexcept : cfg_(cfg) {}

    void setConfig(const CorrectionConfig& cfg) noexcept { cfg_ = cfg; }
    const CorrectionConfig& config() const noexcept { return cfg_; }

    // C = 0 and all timers/histories cleared (Start, preset/strategy/corpus/target/fs change).
    void reset() noexcept;
    // Restore a remembered correction (clamped) as initial value; timers cleared.
    void setInitial(const OperatingBands& c) noexcept;
    // Plan or pool change: freeze for cfg.holdoffSeconds while the estimator refills.
    void notifyPlanChange() noexcept { holdoffRemaining_ = cfg_.holdoffSeconds; }

    // measuredDb: 60 s long-term band levels of the babble (T1); targetDb: babble target shape.
    CorrectionStepResult step(const OperatingBands& measuredDb, const OperatingBands& targetDb,
                              const CorrectionFreeze& freeze = {}) noexcept;

    const OperatingBands& correction() const noexcept { return c_; }
    double timeSeconds() const noexcept { return t_; }

private:
    CorrectionConfig cfg_;
    OperatingBands c_{}, cDesigned_{};
    double t_ = 0.0, lastDesignT_ = -1e30, holdoffRemaining_ = 0.0;
    struct BandState {
        int lastSign = 0;
        double flipTimes[8]{};
        int nFlips = 0;
        double halvedUntil = -1.0;
    };
    std::array<BandState, kNumOperatingBands> band_{};
};

}  // namespace bf
