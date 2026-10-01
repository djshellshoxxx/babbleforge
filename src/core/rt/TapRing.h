#pragma once
// RT -> Analysis tap ring (docs/REALTIME_ARCHITECTURE.md §2.1, §4): single producer (RT),
// single consumer (Analysis). Audio chunks (planar in, stored interleaved) with their absolute
// start sample. On overflow the RT side drops the newest chunk and counts the dropped frames
// (the analysis thread is late); the consumer sees the gap through the chunk start samples and
// marks its window incomplete. Audio is never affected.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/rt/SpscFifo.h"

namespace bf::rt {

class TapRing {
public:
    struct Chunk {
        std::int64_t start = 0;
        std::uint32_t frames = 0;
    };

    // Non-RT. capacityFrames is rounded up to a power of two.
    void prepare(int numChannels, std::size_t capacityFrames) {
        nCh_ = static_cast<std::size_t>(std::max(numChannels, 1));
        std::size_t cap = 1;
        while (cap < capacityFrames) cap <<= 1;
        capFrames_ = cap;
        data_.assign(cap * nCh_, 0.0f);
        chunks_ = std::make_unique<SpscFifo<Chunk, 4096>>();
        writeFrame_.store(0, std::memory_order_relaxed);
        readFrame_.store(0, std::memory_order_relaxed);
        droppedFrames_.store(0, std::memory_order_relaxed);
        droppedChunks_.store(0, std::memory_order_relaxed);
    }

    // RT producer. False (and counted) when the chunk did not fit.
    bool write(std::int64_t start, const float* const* ch, int numChannels, int frames) noexcept {
        if (!chunks_ || frames <= 0) return false;
        const auto n = static_cast<std::size_t>(frames);
        const std::size_t w = writeFrame_.load(std::memory_order_relaxed);
        const std::size_t r = readFrame_.load(std::memory_order_acquire);
        if (capFrames_ - (w - r) < n || chunks_->size() >= chunks_->capacity()) {
            droppedFrames_.fetch_add(n, std::memory_order_relaxed);
            droppedChunks_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const std::size_t nc = std::min(nCh_, static_cast<std::size_t>(numChannels));
        for (std::size_t i = 0; i < n; ++i) {
            float* dst = data_.data() + ((w + i) & (capFrames_ - 1)) * nCh_;
            for (std::size_t c = 0; c < nc; ++c) dst[c] = ch[c][i];
            for (std::size_t c = nc; c < nCh_; ++c) dst[c] = 0.0f;
        }
        writeFrame_.store(w + n, std::memory_order_release);
        chunks_->push(Chunk{start, static_cast<std::uint32_t>(n)});
        return true;
    }

    // Consumer: next chunk header (does not consume).
    bool peek(Chunk& c) const noexcept { return chunks_ && chunks_->peek(c); }
    // Consumer: pops the next chunk into planar dst[c] (capacity >= chunk frames).
    bool read(Chunk& c, float* const* dst) noexcept {
        if (!chunks_ || !chunks_->pop(c)) return false;
        const std::size_t r = readFrame_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < c.frames; ++i) {
            const float* src = data_.data() + ((r + i) & (capFrames_ - 1)) * nCh_;
            for (std::size_t ch = 0; ch < nCh_; ++ch) dst[ch][i] = src[ch];
        }
        readFrame_.store(r + c.frames, std::memory_order_release);
        return true;
    }

    int numChannels() const noexcept { return static_cast<int>(nCh_); }
    std::size_t capacityFrames() const noexcept { return capFrames_; }
    std::size_t bufferedFrames() const noexcept {
        return writeFrame_.load(std::memory_order_acquire) - readFrame_.load(std::memory_order_acquire);
    }
    std::uint64_t droppedFrames() const noexcept { return droppedFrames_.load(std::memory_order_relaxed); }
    std::uint64_t droppedChunks() const noexcept { return droppedChunks_.load(std::memory_order_relaxed); }

private:
    std::size_t nCh_ = 1, capFrames_ = 0;
    std::vector<float> data_;
    std::unique_ptr<SpscFifo<Chunk, 4096>> chunks_;
    std::atomic<std::size_t> writeFrame_{0}, readFrame_{0};
    std::atomic<std::uint64_t> droppedFrames_{0}, droppedChunks_{0};
};

}  // namespace bf::rt
