#pragma once
// Source preparation (docs/TALKER_ENGINE.md §8): processed event audio = source from the
// start anchor with every pause longer than maxGap shortened to maxGap by a 10 ms equal-power
// splice centred in the pause, plus the remapped speech mask (k_s ground truth).
//
// buildProcessedLayout() is a pure function of (snapshot, recording, anchor, maxGap, maxLen);
// the planner uses it for end snapping / k_s and the preparer uses the same layout to render.
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
#include <vector>

#include "core/corpus/CorpusSnapshot.h"

namespace bf {

inline constexpr std::int64_t kSpliceSamples = 480;  // 10 ms equal-power splice

struct SourcePiece {
    std::int64_t srcStart = 0;  // source sample of the first sample of the piece
    std::int64_t dstStart = 0;  // processed position of the first sample
    std::int64_t len = 0;
    bool fadeIn = false, fadeOut = false;  // splice fades over kSpliceSamples at the edges
};

struct ProcessedLayout {
    RecordingId recording = 0;
    std::int64_t anchor = 0;
    std::int64_t length = 0;                // processed samples available
    std::vector<SourcePiece> pieces;        // consecutive pieces overlap by kSpliceSamples
    std::vector<SampleSpan> speech;         // speech regions, processed time
    std::vector<PauseRec> pauses;           // pauses, processed time (flags from source)

    // Source position that corresponds to processed position p (clamped to the layout).
    std::int64_t sourcePosAt(std::int64_t p) const noexcept;
    bool speechAt(std::int64_t p) const noexcept;
};

ProcessedLayout buildProcessedLayout(const CorpusSnapshot& snap, RecordingId rec,
                                     std::int64_t anchor, std::int64_t maxGapSamples,
                                     std::int64_t maxLen);

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
    static constexpr std::size_t kMaxBlocks = 512;  // 8.4 M samples (174 s @ 48 kHz)

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

    // Synchronous: processed samples [from, from + n) of `layout` into dst (zeros past the
    // layout end). Returns false on a source read error.
    bool render(const ProcessedLayout& layout, std::int64_t from, float* dst, std::size_t n);

    // Appends processed audio to `chain` until chain.written() >= min(upTo, chain.length()).
    bool fill(const ProcessedLayout& layout, BlockChain& chain, BlockPool& pool, std::uint64_t upTo);

private:
    IAudioSource& src_;
    std::vector<float> scratch_, piece_;
};

}  // namespace bf
