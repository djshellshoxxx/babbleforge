#pragma once
// Source preparation (docs/TALKER_ENGINE.md §8): processed event audio = source from the
// start anchor with every pause longer than maxGap shortened to maxGap by a 10 ms equal-power
// splice centred in the pause, plus the remapped speech mask (k_s ground truth).
//
// buildProcessedLayout() is a pure function of (snapshot, recording, anchor, maxGap, maxLen,
// engine rate); the planner uses it for end snapping / k_s and the preparer uses the same
// layout to render.
//
// Engine rates (TALKER_ENGINE.md §8.1, CORPUS.md §3.2): the corpus cache is 48 kHz. The
// pieces / splices are built in the 48 kHz source domain; `length`, `speech` and `pauses` are
// in engine samples (positions mapped by round(p * fs / 48000), integer arithmetic). At
// fs != 48 kHz the preparer renders the whole processed event at 48 kHz once and resamples it
// to fs with r8brain (non-RT preload / offline preparation, never in the RT path); the
// resampled event is cached in the layout, so render() stays a pure function of the layout
// and the output is identical for any request granularity.
//
// BlockPool / BlockChain: fixed 16384-sample float blocks preallocated at construction.
// A chain is written by exactly one writer (preloader / offline engine) and read by exactly
// one reader (RT renderer). The writer publishes samples with a release store of writeIndex;
// the reader acquires it and publishes its consumption through readIndex, which the writer
// uses to recycle consumed blocks. The pool free list is touched by the writer only.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "core/corpus/CorpusSnapshot.h"

namespace bf {

inline constexpr std::int64_t kSpliceSamples = 480;  // 10 ms equal-power splice (48 kHz source domain)

// Engine rates the babble path supports (44.1 / 48 / 88.2 / 96 kHz).
bool isSupportedBabbleRate(double fs) noexcept;

// Deterministic time conversions for the planner clock. A supported rate is base x mult with
// base 44100 or 48000 and mult 1 or 2: durations are rounded at the base rate and then
// multiplied, so a 96 kHz (88.2 kHz) timeline is exactly twice the 48 kHz (44.1 kHz) one.
struct RateFamily {
    std::int64_t base = 48000, mult = 1;
};
RateFamily rateFamily(std::int64_t engineRate) noexcept;
std::int64_t secondsToEngine(double s, std::int64_t engineRate) noexcept;  // llround(s * base) * mult
std::int64_t msToEngine(double ms, std::int64_t engineRate) noexcept;      // llround(ms * base / 1000) * mult
// Position mapping between the 48 kHz corpus domain and engine samples (round half up,
// integer arithmetic; corpusToEngine also rounds at the base rate).
std::int64_t corpusToEngine(std::int64_t p48, std::int64_t engineRate) noexcept;
std::int64_t engineToCorpus(std::int64_t pEngine, std::int64_t engineRate) noexcept;

struct SourcePiece {
    std::int64_t srcStart = 0;  // source sample of the first sample of the piece
    std::int64_t dstStart = 0;  // processed position of the first sample
    std::int64_t len = 0;
    bool fadeIn = false, fadeOut = false;  // splice fades over kSpliceSamples at the edges
};

// Whole-event resampled audio (fs != 48 kHz), computed on first use by a preparer.
struct ResampledEventCache {
    std::mutex m;
    std::vector<float> audio;
    bool ready = false, ok = true;
};

struct ProcessedLayout {
    RecordingId recording = 0;
    std::int64_t anchor = 0;
    std::int64_t rate = kCorpusRate;        // engine rate of length / speech / pauses
    std::int64_t length = 0;                // processed samples available (engine samples)
    std::int64_t sourceLength = 0;          // processed samples available (48 kHz domain)
    std::vector<SourcePiece> pieces;        // 48 kHz domain; consecutive pieces overlap by kSpliceSamples
    std::vector<SampleSpan> speech;         // speech regions, processed time (engine samples)
    std::vector<PauseRec> pauses;           // pauses, processed time (engine samples; flags from source)
    std::shared_ptr<ResampledEventCache> resampled;  // fs != 48 kHz only

    // Source position (48 kHz recording sample) that corresponds to processed engine position
    // p (clamped to the layout).
    std::int64_t sourcePosAt(std::int64_t p) const noexcept;
    bool speechAt(std::int64_t p) const noexcept;
};

// maxGapSamples / maxLen are in the 48 kHz source domain; engineRate selects the unit of the
// layout's length / speech / pauses (isSupportedBabbleRate()).
ProcessedLayout buildProcessedLayout(const CorpusSnapshot& snap, RecordingId rec,
                                     std::int64_t anchor, std::int64_t maxGapSamples,
                                     std::int64_t maxLen, std::int64_t engineRate = kCorpusRate);

class BlockPool {
public:
    static constexpr std::size_t kBlockSize = 16384;
    static constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;

    explicit BlockPool(std::size_t numBlocks);
    std::uint32_t allocate() noexcept;           // writer thread; kInvalid if exhausted
    void release(std::uint32_t idx) noexcept;    // writer thread
    float* data(std::uint32_t idx) noexcept { return storage_.data() + idx * kBlockSize; }
    const float* data(std::uint32_t idx) const noexcept { return storage_.data() + idx * kBlockSize; }
    std::size_t capacity() const noexcept { return numBlocks_; }
    std::size_t freeBlocks() const noexcept { return freeList_.size(); }

private:
    std::size_t numBlocks_;
    std::vector<float> storage_;
    std::vector<std::uint32_t> freeList_;  // reserved to capacity; never reallocates
};

class BlockChain {
public:
    static constexpr std::size_t kMaxBlocks = 512;  // 8.4 M samples (87 s @ 96 kHz)

    // Writer side.
    void reset(std::uint64_t length) noexcept;
    // Appends samples (n <= length - written). Returns false if the pool is exhausted.
    bool append(BlockPool& pool, const float* src, std::size_t n) noexcept;
    // Releases blocks the reader has fully consumed; releaseAll frees everything.
    void reclaim(BlockPool& pool) noexcept;
    void releaseAll(BlockPool& pool) noexcept;
    std::uint64_t written() const noexcept { return writeIndex_.load(std::memory_order_relaxed); }
    std::uint64_t length() const noexcept { return length_; }
    bool finished() const noexcept { return finished_.load(std::memory_order_acquire); }
    // Reader side: samples published so far / whole event written.
    std::uint64_t writtenAcquire() const noexcept { return writeIndex_.load(std::memory_order_acquire); }
    bool complete() const noexcept { return writtenAcquire() >= length_; }
    // Writer side: the reader's consumption point.
    std::uint64_t readPosition() const noexcept { return readIndex_.load(std::memory_order_acquire); }

    // Reader side (RT). Copies [pos, pos + n); false if not yet written (underflow).
    bool read(const BlockPool& pool, std::uint64_t pos, float* dst, std::size_t n) const noexcept;
    void setReadIndex(std::uint64_t pos) noexcept { readIndex_.store(pos, std::memory_order_release); }
    void markFinished() noexcept { finished_.store(true, std::memory_order_release); }

private:
    std::array<std::uint32_t, kMaxBlocks> blocks_{};
    std::uint64_t length_ = 0;
    std::uint64_t writeLocal_ = 0;   // writer's private count
    std::size_t reclaimed_ = 0;      // blocks released so far (writer)
    std::atomic<std::uint64_t> writeIndex_{0};
    std::atomic<std::uint64_t> readIndex_{0};
    std::atomic<bool> finished_{false};
};

class SourcePreparer {
public:
    explicit SourcePreparer(IAudioSource& src) : src_(src) {}

    // Synchronous: processed samples [from, from + n) (engine samples) of `layout` into dst
    // (zeros past the layout end). Returns false on a source read error. Not RT-safe at
    // fs != 48 kHz (whole-event resampling on first use).
    bool render(const ProcessedLayout& layout, std::int64_t from, float* dst, std::size_t n);

    // Appends processed audio to `chain` until chain.written() >= min(upTo, chain.length()).
    bool fill(const ProcessedLayout& layout, BlockChain& chain, BlockPool& pool, std::uint64_t upTo);

private:
    bool renderSource(const ProcessedLayout& layout, std::int64_t from, float* dst, std::size_t n);

    IAudioSource& src_;
    std::vector<float> scratch_, piece_;
};

}  // namespace bf
