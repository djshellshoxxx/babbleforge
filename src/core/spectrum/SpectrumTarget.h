#pragma once
// Spectrum targets (docs/SPECTRUM_ENGINE.md §2).
//
// Canonical representation: 1/3-octave BAND LEVELS (power in band, relative dB) at the
// 26 IEC 61260 nominal centres 50 Hz ... 16 kHz. Exact centres are base-10:
// f_m = 1000 * 10^(n/10), n = -13..12; band edges f_m * 10^(+-1/20).
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/config/Types.h"

namespace bf {

inline constexpr std::size_t kNumThirdOctBands = 26;   // 50 Hz ... 16 kHz
inline constexpr std::size_t kFirstOperatingBand = 3;  // 100 Hz
inline constexpr std::size_t kLastOperatingBand = 23;  // 10 kHz (inclusive)
inline constexpr std::size_t kNumOperatingBands = 21;
inline constexpr std::size_t kNumOctaveBands = 7;      // 125 Hz ... 8 kHz
inline constexpr std::size_t kBand1k = 13;

using ThirdOctArray = std::array<double, kNumThirdOctBands>;
using OctaveArray = std::array<double, kNumOctaveBands>;

// Nominal labels (50, 63, 80, 100, ..., 16000).
const std::array<double, kNumThirdOctBands>& thirdOctNominalHz() noexcept;
// Exact base-10 centres 1000*10^(n/10).
double thirdOctCentreHz(std::size_t band) noexcept;
double thirdOctLowerEdgeHz(std::size_t band) noexcept;
double thirdOctUpperEdgeHz(std::size_t band) noexcept;
// Nominal octave labels 125 ... 8000.
const std::array<double, kNumOctaveBands>& octaveNominalHz() noexcept;
// Index of the nominal 1/3-octave band whose nominal centre matches f within 3 %, or -1.
int thirdOctBandIndexForHz(double f) noexcept;

// Octave levels by power-summing the three constituent 1/3-octave bands (125 Hz ... 8 kHz).
OctaveArray octaveFromThirdOct(const ThirdOctArray& thirdDb) noexcept;

struct SpectrumTarget {
    std::string id;
    std::array<float, kNumThirdOctBands> thirdOctDb{};  // relative band levels, 1 kHz = 0 dB
    float lfLimitHz = 80.f;
    float hfLimitHz = 12500.f;
    float lfTrimDb125 = 0.f;  // added to the 100/125/160 Hz bands
    std::uint32_t revision = 0;

    // Band levels with lfTrimDb125 applied (what the filter designer consumes).
    ThirdOctArray effectiveThirdOctDb() const noexcept;
    OctaveArray octaveDb() const noexcept { return octaveFromThirdOct(effectiveThirdOctDb()); }
};

struct SpectrumTargetBuild {
    std::optional<SpectrumTarget> target;
    std::string error;
};

// Builds a 26-band target from a data-set definition. Accepted inputs:
//  - any subset of the 26 nominal 1/3-octave centres (typically 21 bands 100..10k, or all
//    26), matched to IEC nominal values within 3 %; missing bands outside the given range
//    are filled by flat extrapolation from the nearest given band; interior gaps by PCHIP.
//  - 7 octave points 125..8k, PCHIP-interpolated to 1/3-octave in log2(f), flat beyond.
// Values must be finite and within +-40 dB. The result is renormalised to 1 kHz = 0 dB.
SpectrumTargetBuild buildSpectrumTarget(const SpectrumTargetDef& def, float lfTrimDb125 = 0.f);

// Monotone piecewise-cubic Hermite interpolation (Fritsch-Carlson / scipy PCHIP), with
// flat extrapolation beyond the end nodes. x must be strictly increasing, size >= 2.
double pchipEval(const std::vector<double>& x, const std::vector<double>& y, double xq) noexcept;

}  // namespace bf
