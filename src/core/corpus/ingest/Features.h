#pragma once
// Per-recording feature extractors of the Corpus Analyzer (docs/CORPUS.md §1.3, §3.8-3.13).
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/corpus/CorpusSnapshot.h"
#include "core/corpus/ingest/IngestTypes.h"

namespace bf::ingest {

// ITU-T P.56 method B active speech level (envelope tau 0.03 s, hangover 0.2 s, margin
// 15.9 dB, thresholds 2^-15 ... 1 of full scale). Input is float full-scale +-1.
P56Result p56MethodB(const float* x, std::size_t n, double fs);

// Clipping rule of §1.3 on one channel at rate fs: |x| >= 0.999, or >= 3 consecutive samples
// within 0.01 dB of the file's max |x|; runs separated by < 1 ms are merged.
ClipResult detectClipping(const float* x, std::size_t n, std::size_t stride, double fs);

struct LtassResult {
    bool valid = false;
    std::array<double, kNumLtassBands> bandPower{};  // fractions, sum 1
    std::array<float, kNumLtassBands> bandDb{};      // 10 log10(fraction)
    double centroidHz = 0;
    double bandwidthHz = 0;                          // -30 dB point re LTASS peak
    std::size_t frames = 0;
};
// Welch PSD (4096 Hann, 50 % overlap) over frames whose centre is inside a speech region.
LtassResult analyzeLtass(const std::vector<float>& x48k, const std::vector<SampleSpan>& regions);

// YIN F0 (16 kHz, 10 ms hop, 40 ms window, 60-400 Hz, threshold 0.15). Only frames with
// speech10[t] != 0 are analysed. track has one entry per 10 ms frame (0 = unvoiced).
F0Stats yinF0(const std::vector<float>& x16k, const std::vector<std::uint8_t>& speech10);

// Syllable nuclei per second (de Jong & Wempe style) over voiced speech frames.
double speakingRate(const std::vector<float>& x16k, const std::vector<std::uint8_t>& speech10,
                    const std::vector<float>& f0Track);

// Haitsma-Kalker 32-bit sub-fingerprints (5512.5 Hz, 2048 frame, 64 hop, 33 bands 300-2000 Hz).
std::vector<std::uint32_t> computeFingerprint(const std::vector<float>& x16k);

struct FingerprintMatch {
    bool found = false;
    std::int64_t offset = 0;      // frame index in candidate = frame in query + offset
    double ber = 1.0;
    std::int64_t overlap = 0;     // informative frames compared
    std::int64_t frames = 0;
};
constexpr double kNearDupBer = 0.20;
constexpr std::int64_t kNearDupMinFrames = 862;  // ~10 s at 11.6 ms

// Index of fingerprints of accepted recordings; query() finds the best-aligned earlier one.
class FingerprintIndex {
public:
    void add(std::uint32_t recIdx, const std::vector<std::uint32_t>& fp);
    // Returns the best match (lowest BER over >= 10 s informative frames) and its recIdx.
    FingerprintMatch query(const std::vector<std::uint32_t>& fp, std::uint32_t* recIdx) const;

private:
    std::vector<std::vector<std::uint32_t>> fps_;
    std::vector<std::uint32_t> ids_;
    std::unordered_multimap<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> index_;  // value -> (slot, pos)
};

double percentile(std::vector<double> v, double p);

}  // namespace bf::ingest
