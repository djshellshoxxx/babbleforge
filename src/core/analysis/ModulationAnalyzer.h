#pragma once
// Temporal / modulation analyzer (docs/SPECTRUM_ENGINE.md section 8, MASK_STRATEGIES.md 6.1).
// Input: one call per 10 ms frame with the frame power (mean square, linear) of the mix (T3) or
// babble (T1), optionally the ground truth k_a / k_s and per-talker frame powers, and optionally
// the 7 octave-band frame powers (125 Hz..8 kHz) for the per-carrier modulation matrix.
// Analysis thread only; allocates in the constructor / on construction of the ring buffers.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/Fft.h"

namespace bf {

inline constexpr std::size_t kNumModBands = 16;  // 0.5 ... 16 Hz, 1/3 octave (the list in 8.3 has 16 entries)
inline constexpr std::size_t kNumModCarriers = 7;

using ModBandArray = std::array<double, kNumModBands>;
const std::array<double, kNumModBands>& modulationBandCentresHz() noexcept;  // 0.5, 0.63, ... 16 (nominal)

enum class TemporalDensity { Low, Medium, High };

struct GapStats {
    std::size_t count = 0;      // gaps that ended in the last 60 s
    double medianSec = 0.0;
    double p95Sec = 0.0;
    double maxSec = 0.0;
    double ratePerSec = 0.0;    // count / min(60 s, elapsed)
};

struct ModulationSnapshot {
    double leq10sDb = -200.0;
    double occupancy60s = 0.0, occupancy10min = 0.0;  // fraction of frames with k_s >= 1
    double meanKa60s = 0.0, meanKs60s = 0.0;
    bool haveK = false;
    double soloExposurePct = 0.0;   // 60 s; k_s == 1 frames, or dominant talker >= 6 dB over the rest
    GapStats gaps;
    double crest10sDb = 0.0, crest60sDb = 0.0;  // L10 - L90
    double modulationDepth10s = 0.0;            // std/mean of the 16 Hz low-passed power envelope
    TemporalDensity density = TemporalDensity::Medium;
    bool haveModSpectrum = false;
    ModBandArray modBroadband{};                 // 60 s average of m(f) per band
    std::array<ModBandArray, kNumModCarriers> modOctave{};
    std::size_t modBlocks = 0;                   // blocks in the average
    std::size_t frames = 0;
};

// Streaming modulation spectrum: 20 s blocks, Hann, 50 % overlap, m(f) = 2|E(f)|/E(0),
// band value = sqrt(sum of m^2 over the band's bins / ENBW) (energy-averaged; a sinusoidal
// modulation of index m gives m), 60 s average of the last blocks.
class ModulationSpectrumTracker {
public:
    explicit ModulationSpectrumTracker(double frameRateHz = 100.0);
    void reset();
    void push(double framePower);
    bool has() const noexcept { return !recent_.empty(); }
    ModBandArray average() const noexcept;  // mean of the blocks that ended within the last 60 s
    std::size_t numBlocks() const noexcept { return recent_.size(); }
private:
    void computeBlock();
    double rate_;
    std::size_t blockLen_, hop_;
    FftD fft_;
    std::vector<double> window_, ring_, work_;
    std::vector<std::complex<double>> spec_;
    std::size_t sinceBlock_ = 0, total_ = 0;
    std::array<std::pair<std::size_t, std::size_t>, kNumModBands> binRange_{};
    struct Rec { std::size_t endFrame; ModBandArray m; };
    std::vector<Rec> recent_;
    double enbw_ = 1.5;
};

class ModulationAnalyzer {
public:
    static constexpr double kGapBelowLeqDb = 12.0;
    static constexpr double kSoloDominanceDb = 6.0;

    explicit ModulationAnalyzer(bool octaveCarriers = false, double frameRateHz = 100.0);
    void reset();

    // One 10 ms frame. ka/ks < 0: unknown. talkerPowers: optional per-talker frame powers
    // (linear) used for the dominance part of solo exposure.
    void pushFrame(double power, int ka = -1, int ks = -1, std::span<const double> talkerPowers = {},
                   std::span<const double> octavePowers = {});

    ModulationSnapshot snapshot() const;
    // Gaps (durations in seconds, with the frame index at which each ended) in stream order.
    struct Gap { double seconds; std::size_t endFrame; };
    const std::vector<Gap>& allGaps() const noexcept { return gaps_; }
    std::size_t frames() const noexcept { return count_; }

    static TemporalDensity classify(double crestDb) noexcept;

private:
    struct Frame { double power; float ka, ks; bool solo; };
    const Frame& at(std::size_t back) const noexcept;  // back = 0 -> most recent
    double rate_;
    std::size_t cap_, w10_, w60_;
    std::vector<Frame> ring_;
    std::vector<double> lpRing_;  // 16 Hz low-passed power envelope
    std::size_t count_ = 0;
    double sum10_ = 0.0;  // running sum of power over the last 10 s
    // gap detector
    bool inGap_ = false;
    std::size_t gapLen_ = 0;
    std::vector<Gap> gaps_;
    // low-pass biquad state
    double b0_, b1_, b2_, a1_, a2_, z1_ = 0.0, z2_ = 0.0;
    ModulationSpectrumTracker broadband_;
    std::vector<ModulationSpectrumTracker> octave_;
};

// Offline convenience (bfanalyze): planar buffers, per-channel mean squares over 10 ms frames are
// power-summed, then fed through a ModulationAnalyzer. Returns the final snapshot; k_a/k_s unknown.
ModulationSnapshot analyzeModulation(const float* const* planar, std::size_t numChannels, std::size_t nFrames,
                                     double fs);

}  // namespace bf
