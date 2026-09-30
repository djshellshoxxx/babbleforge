#pragma once
// Uniformly partitioned overlap-save FIR convolver (docs/SPECTRUM_ENGINE.md §4.4,
// docs/REALTIME_ARCHITECTURE.md §1, §5.2, §8.4).
//
// - Partition 256 samples, FFT size 512, frequency-domain delay line (FDL) of
//   ceil(maxKernelLen/256) partitions. All memory is allocated in prepare().
// - process(in, out, n) accepts any n. Input is re-blocked through a fixed 256-sample FIFO
//   anchored to the absolute sample counter, so output is bit-identical for every host
//   block size. Fixed latency: kLatency = 256 samples, i.e. out[t] = (h * in)[t - 256].
// - Kernels are immutable PartitionedKernel objects built off-RT and published through an
//   RCU pending slot (std::atomic pointer). The RT thread picks a pending kernel up at the
//   first 256-sample grid boundary at or after kernel->effectiveSample. If a kernel is
//   already active, old and new kernels run in parallel over the same FDL for 100 ms
//   (round(0.1 fs) samples) with an equal-power (cos/sin) crossfade; afterwards the old
//   kernel is handed back through a lock-free SPSC retire queue. The RT thread never
//   deletes. collectGarbage() (control thread) deletes retired kernels.
// - At most one pending update: posting while one is pending replaces (and deletes) it.
//   While a crossfade runs, a pending kernel waits until the crossfade has finished.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/dsp/Fft.h"

namespace bf {

class PartitionedKernel {
public:
    static constexpr std::size_t kPartition = 256;
    static constexpr std::size_t kFftSize = 512;
    static constexpr std::size_t kBins = kFftSize / 2 + 1;

    // Off-RT: partitions h (len taps) and transforms each partition.
    static std::unique_ptr<PartitionedKernel> create(const float* h, std::size_t len);

    std::size_t length() const noexcept { return length_; }
    std::size_t numPartitions() const noexcept { return numPartitions_; }
    // Interleaved (re, im) spectrum of partition p: 2 * kBins floats.
    const float* partition(std::size_t p) const noexcept { return data_.data() + p * 2 * kBins; }

    // Absolute sample time from which this kernel may take effect (§8.4 stamping).
    std::int64_t effectiveSample = 0;

private:
    PartitionedKernel() = default;
    std::size_t length_ = 0, numPartitions_ = 0;
    std::vector<float> data_;
};

class PartitionedConvolver {
public:
    static constexpr std::size_t kBlock = PartitionedKernel::kPartition;
    static constexpr std::size_t kLatency = kBlock;

    PartitionedConvolver() = default;
    ~PartitionedConvolver();
    PartitionedConvolver(const PartitionedConvolver&) = delete;
    PartitionedConvolver& operator=(const PartitionedConvolver&) = delete;

    // Non-RT. Allocates everything and resets the stream to sample 0 (clears kernels too).
    void prepare(double fs, std::size_t maxKernelLen, std::size_t retireQueueCapacity = 8);

    // Control thread. Takes ownership. Returns false (kernel discarded) if the kernel is
    // longer than maxKernelLen or prepare() has not been called.
    bool postKernel(std::unique_ptr<PartitionedKernel> kernel);
    // Control thread: deletes kernels retired by the RT thread. Returns how many.
    std::size_t collectGarbage();

    // RT. No allocation, no locks, no exceptions. in and out may alias.
    void process(const float* in, float* out, std::size_t n) noexcept;

    bool isCrossfading() const noexcept { return old_ != nullptr; }
    bool hasKernel() const noexcept { return current_ != nullptr; }
    std::size_t crossfadeLength() const noexcept { return xfLen_; }
    std::int64_t samplePosition() const noexcept { return samplesIn_; }
    std::size_t maxPartitions() const noexcept { return maxParts_; }

private:
    void runBlock() noexcept;
    void pickUpPending(std::int64_t blockStart) noexcept;
    bool tryRetire(PartitionedKernel* k) noexcept;
    void accumulate(const PartitionedKernel& k, float* acc) noexcept;
    void releaseAll() noexcept;

    std::unique_ptr<FftF> fft_;
    std::size_t maxParts_ = 0;
    std::vector<float> fdl_;     // maxParts_ * 2 * kBins
    std::size_t head_ = 0;
    std::vector<float> frame_;   // 512 time samples: previous 256 | current 256
    std::vector<float> spec_;    // 2 * kBins scratch
    std::vector<float> accNew_, accOld_;  // 2 * kBins
    std::vector<float> yNew_, yOld_;      // 512
    std::vector<float> inFifo_, outFifo_; // 256 each
    std::size_t pos_ = 0;
    std::int64_t samplesIn_ = 0;

    std::vector<float> xfOld_, xfNew_;  // crossfade gain tables
    std::size_t xfLen_ = 0, xfPos_ = 0;

    PartitionedKernel* current_ = nullptr;  // RT-owned
    PartitionedKernel* old_ = nullptr;      // RT-owned, fading out
    PartitionedKernel* retireHold_ = nullptr;  // waiting for retire-queue space
    std::atomic<PartitionedKernel*> pending_{nullptr};
    static_assert(std::atomic<PartitionedKernel*>::is_always_lock_free);

    // SPSC retire ring (RT producer, control consumer).
    std::vector<PartitionedKernel*> ring_;
    std::atomic<std::size_t> ringHead_{0}, ringTail_{0};
};

}  // namespace bf
