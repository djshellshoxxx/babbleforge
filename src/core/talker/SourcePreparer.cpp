#include "core/talker/SourcePreparer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bf {

namespace {
constexpr double kHalfPi = 1.57079632679489661923;
}

std::int64_t ProcessedLayout::sourcePosAt(std::int64_t p) const noexcept {
    if (pieces.empty()) return anchor;
    p = std::clamp<std::int64_t>(p, 0, length);
    // Last piece whose dstStart <= p.
    std::size_t i = pieces.size() - 1;
    while (i > 0 && pieces[i].dstStart > p) --i;
    return pieces[i].srcStart + (p - pieces[i].dstStart);
}

bool ProcessedLayout::speechAt(std::int64_t p) const noexcept {
    auto it = std::upper_bound(speech.begin(), speech.end(), p,
                               [](std::int64_t v, const SampleSpan& s) { return v < s.end; });
    return it != speech.end() && it->start <= p;
}

ProcessedLayout buildProcessedLayout(const CorpusSnapshot& snap, RecordingId rec,
                                     std::int64_t anchor, std::int64_t maxGapSamples,
                                     std::int64_t maxLen) {
    ProcessedLayout L;
    L.recording = rec;
    L.anchor = anchor;
    const std::int64_t recLen = snap.recording(rec).length;
    const std::int64_t maxGap = std::max(maxGapSamples, kSpliceSamples);
    const auto regions = snap.regionsOf(rec);
    const auto pauses = snap.pausesOf(rec);
    constexpr std::int64_t kHalf = kSpliceSamples / 2;

    std::int64_t removed = 0;  // samples removed before the current source position
    SourcePiece cur{anchor, 0, 0, false, false};
    std::int64_t limitSrc = recLen;  // source position where processed length reaches maxLen

    auto dstOf = [&](std::int64_t src) { return src - anchor - removed; };

    for (const auto& ps : pauses) {
        if (ps.end <= anchor) continue;
        if (dstOf(ps.start) >= maxLen) break;
        const std::int64_t lp = ps.end - ps.start;
        const std::int64_t d0 = dstOf(ps.start);
        if (lp > maxGap && ps.start >= anchor) {
            const std::int64_t R = lp - maxGap;
            const std::int64_t cut = ps.start + maxGap / 2;   // source cut point
            const std::int64_t dCut = dstOf(cut);
            cur.len = (cut + kHalf) - cur.srcStart;
            cur.fadeOut = true;
            L.pieces.push_back(cur);
            removed += R;
            const std::int64_t resume = cut + R;
            cur = SourcePiece{resume - kHalf, dCut - kHalf, 0, true, false};
            L.pauses.push_back({d0, d0 + maxGap, ps.phraseBoundary});
        } else if (ps.start >= anchor) {
            L.pauses.push_back({d0, d0 + lp, ps.phraseBoundary});
        }
    }
    // Final piece up to the recording end or maxLen.
    const std::int64_t endDst = std::min(maxLen, dstOf(limitSrc));
    cur.len = endDst - cur.dstStart;
    if (cur.len > 0) L.pieces.push_back(cur);
    L.length = std::max<std::int64_t>(0, endDst);
    // Pauses beyond the end are dropped.
    while (!L.pauses.empty() && L.pauses.back().start >= L.length) L.pauses.pop_back();

    // Speech regions in processed time: map each region through the piece that contains it.
    for (const auto& r : regions) {
        if (r.end <= anchor) continue;
        const std::int64_t s0 = std::max(r.start, anchor);
        // Find piece containing s0 (regions never straddle a cut).
        std::int64_t dst = -1;
        for (const auto& pc : L.pieces) {
            if (s0 >= pc.srcStart && s0 < pc.srcStart + pc.len) { dst = pc.dstStart + (s0 - pc.srcStart); }
        }
        if (dst < 0 || dst >= L.length) break;
        const std::int64_t e = std::min(L.length, dst + (r.end - s0));
        L.speech.push_back({dst, e});
    }
    return L;
}

// ---------------------------------------------------------------- BlockPool / BlockChain

BlockPool::BlockPool(std::size_t numBlocks)
    : numBlocks_(numBlocks), storage_(numBlocks * kBlockSize, 0.0f) {
    freeList_.reserve(numBlocks);
    for (std::size_t i = numBlocks; i-- > 0;) freeList_.push_back(static_cast<std::uint32_t>(i));
}

std::uint32_t BlockPool::allocate() noexcept {
    if (freeList_.empty()) return kInvalid;
    const std::uint32_t b = freeList_.back();
    freeList_.pop_back();
    return b;
}

void BlockPool::release(std::uint32_t idx) noexcept {
    if (idx != kInvalid && freeList_.size() < numBlocks_) freeList_.push_back(idx);
}

void BlockChain::reset(std::uint64_t length) noexcept {
    length_ = std::min<std::uint64_t>(length, kMaxBlocks * BlockPool::kBlockSize);
    writeLocal_ = 0;
    reclaimed_ = 0;
    blocks_.fill(BlockPool::kInvalid);
    readIndex_.store(0, std::memory_order_relaxed);
    finished_.store(false, std::memory_order_relaxed);
    writeIndex_.store(0, std::memory_order_release);
}

bool BlockChain::append(BlockPool& pool, const float* src, std::size_t n) noexcept {
    constexpr std::size_t B = BlockPool::kBlockSize;
    n = static_cast<std::size_t>(std::min<std::uint64_t>(n, length_ - writeLocal_));
    while (n > 0) {
        const std::size_t bi = static_cast<std::size_t>(writeLocal_ / B);
        const std::size_t off = static_cast<std::size_t>(writeLocal_ % B);
        if (off == 0) {
            const std::uint32_t b = pool.allocate();
            if (b == BlockPool::kInvalid) return false;
            blocks_[bi] = b;
        }
        const std::size_t k = std::min(n, B - off);
        std::memcpy(pool.data(blocks_[bi]) + off, src, k * sizeof(float));
        src += k;
        n -= k;
        writeLocal_ += k;
        writeIndex_.store(writeLocal_, std::memory_order_release);
    }
    return true;
}

void BlockChain::reclaim(BlockPool& pool) noexcept {
    const std::uint64_t r = readIndex_.load(std::memory_order_acquire);
    const std::size_t full = static_cast<std::size_t>(r / BlockPool::kBlockSize);
    while (reclaimed_ < full && reclaimed_ < kMaxBlocks) {
        pool.release(blocks_[reclaimed_]);
        blocks_[reclaimed_] = BlockPool::kInvalid;
        ++reclaimed_;
    }
}

void BlockChain::releaseAll(BlockPool& pool) noexcept {
    const std::size_t used =
        static_cast<std::size_t>((writeLocal_ + BlockPool::kBlockSize - 1) / BlockPool::kBlockSize);
    for (std::size_t i = reclaimed_; i < used && i < kMaxBlocks; ++i) {
        pool.release(blocks_[i]);
        blocks_[i] = BlockPool::kInvalid;
    }
    reclaimed_ = used;
}

bool BlockChain::read(const BlockPool& pool, std::uint64_t pos, float* dst, std::size_t n) const noexcept {
    constexpr std::size_t B = BlockPool::kBlockSize;
    const std::uint64_t avail = writeIndex_.load(std::memory_order_acquire);
    if (pos + n > avail) return false;
    while (n > 0) {
        const std::size_t bi = static_cast<std::size_t>(pos / B);
        const std::size_t off = static_cast<std::size_t>(pos % B);
        const std::size_t k = std::min(n, B - off);
        std::memcpy(dst, pool.data(blocks_[bi]) + off, k * sizeof(float));
        dst += k;
        n -= k;
        pos += k;
    }
    return true;
}

// ---------------------------------------------------------------- SourcePreparer

bool SourcePreparer::render(const ProcessedLayout& L, std::int64_t from, float* dst, std::size_t n) {
    std::memset(dst, 0, n * sizeof(float));
    const std::int64_t to = from + static_cast<std::int64_t>(n);
    bool ok = true;
    for (const auto& pc : L.pieces) {
        const std::int64_t a = std::max(from, pc.dstStart);
        const std::int64_t b = std::min({to, pc.dstStart + pc.len, L.length});
        if (b <= a) continue;
        const std::size_t k = static_cast<std::size_t>(b - a);
        piece_.resize(k);
        const std::int64_t src = pc.srcStart + (a - pc.dstStart);
        if (!src_.read(L.recording, static_cast<std::uint64_t>(src), piece_.data(), k)) ok = false;
        for (std::size_t i = 0; i < k; ++i) {
            const std::int64_t q = a + static_cast<std::int64_t>(i) - pc.dstStart;  // pos in piece
            double g = 1.0;
            if (pc.fadeIn && q < kSpliceSamples)
                g *= std::sin(kHalfPi * (static_cast<double>(q) + 0.5) / kSpliceSamples);
            if (pc.fadeOut && q >= pc.len - kSpliceSamples)
                g *= std::cos(kHalfPi * (static_cast<double>(q - (pc.len - kSpliceSamples)) + 0.5) /
                              kSpliceSamples);
            dst[static_cast<std::size_t>(a - from) + i] += static_cast<float>(g) * piece_[i];
        }
    }
    return ok;
}

bool SourcePreparer::fill(const ProcessedLayout& L, BlockChain& chain, BlockPool& pool, std::uint64_t upTo) {
    chain.reclaim(pool);
    const std::uint64_t target = std::min(upTo, chain.length());
    bool ok = true;
    constexpr std::size_t kChunk = 4096;
    scratch_.resize(kChunk);
    while (chain.written() < target) {
        const std::uint64_t w = chain.written();
        const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, target - w));
        if (!render(L, static_cast<std::int64_t>(w), scratch_.data(), k)) ok = false;
        if (!chain.append(pool, scratch_.data(), k)) return false;
    }
    return ok;
}

}  // namespace bf
