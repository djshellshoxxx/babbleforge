#pragma once
// Whole-file offline analysis (docs/VALIDATION.md §2.2, `bfanalyze`): the real-time metric
// definitions of Meters / SpectrumAnalyzer / ModulationAnalyzer computed over an entire file.
//
// Output JSON:
//   level      : RMS per channel and energy mean, sample/true peak, crest, LUFS-S (max/final), LUFS-I
//   spectrum   : 1/3-octave (26 bands) and octave (7) levels; with a target: shape deviation
//   temporal   : envelope L10/L90/L10-L90 (10 ms frames), modulation depth, modulation spectrum
//                (mean of 60 s averages), gap statistics and histogram
//   correlation: whole-file Pearson matrix, max |rho| over 10 s windows per pair
//   talkers    : (with an events.json timeline) event count, speakers, mean/min/max active
#include <cstddef>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/spectrum/SpectrumTarget.h"

namespace bf {

struct OfflineAnalysisOptions {
    std::optional<ThirdOctArray> targetDb;  // reference band levels (e.g. idealBandLevelsDb of a target)
    std::string targetId;
    const nlohmann::json* events = nullptr;  // babbleforge.events/1 document
};

nlohmann::json analyzeAudio(const float* const* planar, std::size_t numChannels, std::size_t numFrames, double fs,
                            const OfflineAnalysisOptions& opt = {});

}  // namespace bf
