#pragma once
// Minimum-phase FIR design for the stationary and babble shapers
// (docs/SPECTRUM_ENGINE.md §4.3). Runs on a worker thread; allocates freely. Never RT.
//
// Band-level convention: the 26 target values are 1/3-octave BAND LEVELS (power in the
// band, as measured by the §5.1 analyzer). §4.3 step 2 interpolates B[k] per FFT bin; we
// interpolate the equivalent per-Hz density (B[k] - 10log10(bandwidth_k)), with the PCHIP
// node values refined (fixed-point iteration) so that the band integral of the density
// reproduces B[k] exactly. The response of the filter to white noise then matches B[k]
// band-by-band, which is what §4.3 step 7 and the analyzer verify.
#include <cstddef>
#include <vector>

#include "core/spectrum/SpectrumTarget.h"

namespace bf {

struct FirDesignParams {
    double fs = 48000.0;
    std::size_t taps = 0;         // 0 -> defaultStationaryTaps(fs)
    std::size_t maxTaps = 16384;  // auto-doubling cap (step 7)
    double lfLimitHz = 80.0;      // 2nd-order Butterworth HP corner
    double hfLimitHz = 12500.0;   // 4th-order Butterworth LP corner, clamped to 0.45 fs
    double toleranceDb = 0.5;     // max mean-removed deviation 100 Hz..10 kHz
    bool unitEnergy = true;       // step 6 (stationary kernel)
};

struct FirDesignResult {
    std::vector<double> h;       // taps (length = final L)
    std::size_t taps = 0;
    int doublings = 0;
    bool degraded = false;       // verification failed at maxTaps (`spectrum.designDegraded`)
    double maxDeviationDb = 0.0; // operating range, mean-removed
    ThirdOctArray achievedDb{};  // analytic band levels of h against unit white noise
    ThirdOctArray referenceDb{}; // ideal band levels: target with HP/LP roll-off applied
    std::vector<float> tapsFloat() const;
};

std::size_t defaultStationaryTaps(double fs) noexcept;  // 4096 (<= 48k), 8192 (> 48k)
std::size_t defaultBabbleTaps(double fs) noexcept;      // 2048 (<= 48k), 4096 (> 48k)

// Full §4.3 procedure (steps 1-7). bandsDb: 26 band levels (e.g. target.effectiveThirdOctDb()).
FirDesignResult designMinPhaseFir(const ThirdOctArray& bandsDb, const FirDesignParams& p);

// Ideal band levels: power of (target density x |H_hp H_lp|^2) integrated over each band.
ThirdOctArray idealBandLevelsDb(const ThirdOctArray& bandsDb, const FirDesignParams& p);

// Analytic 1/3-octave response of h to unit-variance white noise: 10log10 of
// integral_band |H(f)|^2 df (fractional-bin weighting at IEC band edges), in dB.
ThirdOctArray firThirdOctResponseDb(const std::vector<double>& h, double fs);

// max |(a-b) - mean(a-b)| over the 21 operating bands (100 Hz..10 kHz).
double maxShapeDeviationDb(const ThirdOctArray& a, const ThirdOctArray& b) noexcept;

// Babble-mode normalisation (§4.3 last bullet): scale h so that its gain for an input with
// the pool LTASS equals 1:  sum_b G_b P_b = sum_b P_b, with G_b = mean |H|^2 in band b
// (relative to a unit-gain filter) and P_b = 10^(poolLtassDb[b]/10). poolLtassDb holds 26
// bands (50..16k) or 21 bands (100..10k). Returns the linear gain applied.
double normalizeForPoolLtass(std::vector<double>& h, double fs, const std::vector<double>& poolLtassDb);

// Babble static EQ target (§2.3): T - LTASS_pool + C, clamped to +-12 dB per band.
// Sets *clamped (if non-null) when any band hit the clamp (`spectrum.eqClamped`).
ThirdOctArray babbleEqBandsDb(const ThirdOctArray& target, const ThirdOctArray& poolLtass,
                              const ThirdOctArray& correction, bool* clamped = nullptr) noexcept;

}  // namespace bf
