#pragma once
// Stationary speech-shaped masker (docs/SPECTRUM_ENGINE.md §4.1, ENGINE.md §3.3).
//
// Per output channel c: an independent RngStream "noise.ch.<c>" (derived from the master
// seed), w = sqrt(3) * uniformPM1f() (unit variance), shaped by a unit-energy min-phase FIR
// through a PartitionedConvolver, then scaled by g_ref = 10^(L_ref/20) (default -26 dBFS).
// The gain is smoothed per sample (one-pole, tau = 200 ms) towards a target that is read
// from an atomic on a 32-sample grid anchored to the absolute sample counter, so the output
// is bit-identical for every host block size (REALTIME_ARCHITECTURE.md §8.4).
// Latency: PartitionedConvolver::latency() (256 samples at <= 50 kHz, 512 above); the output starts after it.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/dsp/PartitionedConvolver.h"
#include "core/random/Random.h"

namespace bf {

class StationaryMaskEngine {
public:
    static constexpr float kDefaultLevelDbfs = -26.0f;
    static constexpr std::size_t kGainGrid = 32;

    // Non-RT. Allocates all state.
    void prepare(double fs, std::size_t numChannels, std::uint64_t masterSeed, std::size_t maxKernelLen,
                 float levelDbfs = kDefaultLevelDbfs, std::uint64_t epoch = 0);

    // Control/worker thread: builds one partitioned kernel per channel and posts it
    // (crossfaded if a kernel is already active). effectiveSample: see PartitionedKernel.
    bool setFilter(const std::vector<float>& h, std::int64_t effectiveSample = 0);
    std::size_t collectGarbage();

    // Any thread: target output level (dBFS RMS per channel).
    void setLevelDbfs(float dbfs) noexcept { targetDb_.store(dbfs, std::memory_order_relaxed); }

    // RT. out[c] for c < numChannels, nFrames samples each. No allocation.
    void process(float* const* out, std::size_t nFrames) noexcept;

    std::size_t numChannels() const noexcept { return channels_.size(); }

private:
    struct Channel {
        explicit Channel(std::uint64_t streamSeed) : rng(streamSeed) {}
        RngStream rng;
        PartitionedConvolver conv;
    };
    std::vector<std::unique_ptr<Channel>> channels_;
    std::vector<float> noise_, gain_;  // kChunk scratch
    static constexpr std::size_t kChunk = 256;
    std::atomic<float> targetDb_{kDefaultLevelDbfs};
    static_assert(std::atomic<float>::is_always_lock_free);
    double gainCur_ = 0.0, gainTarget_ = 0.0, gainCoef_ = 0.0;
    float gainTargetDb_ = kDefaultLevelDbfs;
    std::int64_t sample_ = 0;
};

}  // namespace bf
