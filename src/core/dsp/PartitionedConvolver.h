#pragma once
// Uniformly partitioned overlap-save FIR convolver (docs/SPECTRUM_ENGINE.md §4.4,
// docs/REALTIME_ARCHITECTURE.md §1, §5.2, §8.4).
//
// - Partition P = partitionForRate(fs): 256 samples up to 50 kHz, 512 above (the same 5.3 ms
//   at 48 and 96 kHz, so the per-sample cost of a kernel of fixed duration scales with the
//   rate, not with its square). FFT size 2P, planar spectra (PartitionedKernel::stride()),
//   frequency-domain delay line (FDL) of ceil(maxKernelLen/P) partitions. All memory is
//   allocated in prepare().
// - process(in, out, n) accepts any n. Input is re-blocked through a fixed P-sample FIFO
//   anchored to the absolute sample counter, so output is bit-identical for every host
//   block size. Fixed latency: latency() = P samples, i.e. out[t] = (h * in)[t - P].
// - Kernels are immutable PartitionedKernel objects built off-RT and published through an
//   RCU pending slot (std::atomic pointer). The RT thread picks a pending kernel up at the
//   first P-sample grid boundary at or after kernel->effectiveSample. If a kernel is
//   already active, old and new kernels run in parallel over the same FDL for 100 ms
//   (round(0.1 fs) samples) with an equal-power (cos/sin) crossfade; afterwards the old
//   kernel is handed back through a lock-free SPSC retire queue. The RT thread never
//   deletes. collectGarbage() (control thread) deletes retired kernels.
// - Kernel spectra are immutable and shared: clone() gives another kernel object (one per
//   convolver, for the RCU hand-over) over the same spectra, so N channels with the same
//   filter keep one copy in cache. The last owner releases them, always off-RT.
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
    static constexpr std::size_t kPartition = 256;  // partition at <= 50 kHz

    // Partition size for an engine rate: 256 at <= 50 kHz, 512 above.
    static std::size_t partitionForRate(double fs) noexcept { return fs > 50000.0 ? 2 * kPartition : kPartition; }
    // Planar spectrum stride for a partition: P + 1 bins rounded up to a multiple of 8 (zero
    // padding), so the complex multiply-accumulate runs over whole SIMD vectors.
    static constexpr std::size_t strideFor(std::size_t partition) noexcept { return (partition + 1 + 7) / 8 * 8; }

    // Off-RT: partitions h (len taps) into `partition`-sample blocks (power of two >= 4) and
    // transforms each (FFT size 2 * partition).
    static std::unique_ptr<PartitionedKernel> create(const float* h, std::size_t len,
                                                     std::size_t partition = kPartition);
    // Off-RT: another kernel object sharing these spectra (same effectiveSample).
    std::unique_ptr<PartitionedKernel> clone() const;

    std::size_t length() const noexcept { return length_; }
    std::size_t numPartitions() const noexcept { return numPartitions_; }
    std::size_t partitionSize() const noexcept { return partition_; }
    std::size_t stride() const noexcept { return stride_; }
    // Planar spectrum of partition p: re[stride()] followed by im[stride()] (bins > P are 0).
    const float* partition(std::size_t p) const noexcept { return spec_ + p * 2 * stride_; }

    // Absolute sample time from which this kernel may take effect (§8.4 stamping).
    std::int64_t effectiveSample = 0;

private:
    PartitionedKernel() = default;
    PartitionedKernel(const PartitionedKernel&) = default;
    std::size_t length_ = 0, numPartitions_ = 0, partition_ = kPartition, stride_ = strideFor(kPartition);
    std::shared_ptr<const std::vector<float>> data_;
    const float* spec_ = nullptr;  // data_->data()
};

class PartitionedConvolver {
public:
    PartitionedConvolver() = default;
    ~PartitionedConvolver();
    PartitionedConvolver(const PartitionedConvolver&) = delete;
    PartitionedConvolver& operator=(const PartitionedConvolver&) = delete;

    // Non-RT. Allocates everything and resets the stream to sample 0 (clears kernels too).
    void prepare(double fs, std::size_t maxKernelLen, std::size_t retireQueueCapacity = 8);

    // Control thread. Takes ownership. Returns false (kernel discarded) if the kernel is
    // longer than maxKernelLen, its partition size is not partitionSize(), or prepare() has
    // not been called.
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
    // Partition / block size P (PartitionedKernel::partitionForRate(fs) of prepare()).
    std::size_t partitionSize() const noexcept { return block_; }
    // Fixed latency in samples (= partitionSize()).
    std::size_t latency() const noexcept { return block_; }

private:
    void runBlock() noexcept;
    void pickUpPending(std::int64_t blockStart) noexcept;
    bool tryRetire(PartitionedKernel* k) noexcept;
    void accumulate(const PartitionedKernel& k, float* acc) noexcept;
    void releaseAll() noexcept;

    std::unique_ptr<FftF> fft_;
    std::size_t block_ = PartitionedKernel::kPartition;           // P
    std::size_t stride_ = PartitionedKernel::strideFor(block_);  // planar spectrum stride
    std::size_t maxParts_ = 0;
    std::vector<float> fdl_;     // maxParts_ * 2 * stride_ (planar re | im per partition)
    std::size_t head_ = 0;
    std::vector<float> frame_;   // 2P time samples: previous P | current P
    std::vector<float> accNew_, accOld_;  // 2 * stride_ (planar)
    std::vector<float> yNew_, yOld_;      // 2P
    std::vector<float> inFifo_, outFifo_; // P each
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
