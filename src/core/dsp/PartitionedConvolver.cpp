#include "core/dsp/PartitionedConvolver.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/math/DetMath.h"
#include "core/math/Restrict.h"

namespace bf {

std::unique_ptr<PartitionedKernel> PartitionedKernel::create(const float* h, std::size_t len, std::size_t partition) {
    std::unique_ptr<PartitionedKernel> k(new PartitionedKernel());
    const std::size_t P = partition;
    const std::size_t S = strideFor(P);
    k->length_ = len;
    k->partition_ = P;
    k->stride_ = S;
    k->numPartitions_ = std::max<std::size_t>(1, (len + P - 1) / P);
    auto data = std::make_shared<std::vector<float>>(k->numPartitions_ * 2 * S, 0.0f);
    FftF fft(2 * P);
    std::vector<float> buf(2 * P);
    for (std::size_t p = 0; p < k->numPartitions_; ++p) {
        std::fill(buf.begin(), buf.end(), 0.0f);
        for (std::size_t i = 0; i < P && p * P + i < len; ++i) buf[i] = h[p * P + i];
        float* d = data->data() + p * 2 * S;
        fft.forwardSplit(buf.data(), d, d + S);
    }
    k->spec_ = data->data();
    k->data_ = std::move(data);
    return k;
}

std::unique_ptr<PartitionedKernel> PartitionedKernel::clone() const {
    return std::unique_ptr<PartitionedKernel>(new PartitionedKernel(*this));
}

PartitionedConvolver::~PartitionedConvolver() { releaseAll(); }

void PartitionedConvolver::releaseAll() noexcept {
    collectGarbage();
    delete current_;
    delete old_;
    delete retireHold_;
    delete pending_.exchange(nullptr);
    current_ = old_ = retireHold_ = nullptr;
}

void PartitionedConvolver::prepare(double fs, std::size_t maxKernelLen, std::size_t retireQueueCapacity) {
    releaseAll();
    block_ = PartitionedKernel::partitionForRate(fs);
    stride_ = PartitionedKernel::strideFor(block_);
    const std::size_t S = stride_;
    fft_ = std::make_unique<FftF>(2 * block_);
    maxParts_ = std::max<std::size_t>(1, (maxKernelLen + block_ - 1) / block_);
    fdl_.assign(maxParts_ * 2 * S, 0.0f);
    head_ = 0;
    frame_.assign(2 * block_, 0.0f);
    accNew_.assign(2 * S, 0.0f);
    accOld_.assign(2 * S, 0.0f);
    yNew_.assign(2 * block_, 0.0f);
    yOld_.assign(2 * block_, 0.0f);
    inFifo_.assign(block_, 0.0f);
    outFifo_.assign(block_, 0.0f);
    pos_ = 0;
    samplesIn_ = 0;
    xfLen_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(0.1 * fs)));
    xfPos_ = 0;
    xfOld_.resize(xfLen_);
    xfNew_.resize(xfLen_);
    constexpr double kHalfPi = 1.57079632679489661923;
    for (std::size_t i = 0; i < xfLen_; ++i) {
        double s = 0.0, c = 1.0;
        detsincos(kHalfPi * (static_cast<double>(i) + 1.0) / static_cast<double>(xfLen_), s, c);
        xfOld_[i] = static_cast<float>(c);
        xfNew_[i] = static_cast<float>(s);
    }
    std::size_t cap = 2;
    while (cap < retireQueueCapacity) cap <<= 1;
    ring_.assign(cap, nullptr);
    ringHead_.store(0);
    ringTail_.store(0);
}

bool PartitionedConvolver::postKernel(std::unique_ptr<PartitionedKernel> kernel) {
    if (!kernel || !fft_ || kernel->partitionSize() != block_ || kernel->numPartitions() > maxParts_) return false;
    PartitionedKernel* superseded = pending_.exchange(kernel.release(), std::memory_order_acq_rel);
    delete superseded;  // never seen by RT
    return true;
}

std::size_t PartitionedConvolver::collectGarbage() {
    std::size_t n = 0;
    if (ring_.empty()) return 0;
    std::size_t tail = ringTail_.load(std::memory_order_relaxed);
    const std::size_t head = ringHead_.load(std::memory_order_acquire);
    while (tail != head) {
        delete ring_[tail & (ring_.size() - 1)];
        ring_[tail & (ring_.size() - 1)] = nullptr;
        ++tail;
        ++n;
    }
    ringTail_.store(tail, std::memory_order_release);
    return n;
}

bool PartitionedConvolver::tryRetire(PartitionedKernel* k) noexcept {
    const std::size_t head = ringHead_.load(std::memory_order_relaxed);
    const std::size_t tail = ringTail_.load(std::memory_order_acquire);
    if (head - tail >= ring_.size()) return false;
    ring_[head & (ring_.size() - 1)] = k;
    ringHead_.store(head + 1, std::memory_order_release);
    return true;
}

void PartitionedConvolver::pickUpPending(std::int64_t blockStart) noexcept {
    if (retireHold_ && tryRetire(retireHold_)) retireHold_ = nullptr;
    if (old_ || retireHold_) return;  // one crossfade at a time
    PartitionedKernel* p = pending_.load(std::memory_order_acquire);
    if (!p || p->effectiveSample > blockStart) return;
    PartitionedKernel* n = pending_.exchange(nullptr, std::memory_order_acq_rel);
    if (!n) return;
    if (n->effectiveSample > blockStart) {
        // Control replaced the slot between load and exchange with a not-yet-due kernel:
        // put it back; if the slot was refilled meanwhile, n is superseded -> retire.
        PartitionedKernel* expected = nullptr;
        if (!pending_.compare_exchange_strong(expected, n, std::memory_order_acq_rel)) {
            if (!tryRetire(n)) retireHold_ = n;
        }
        return;
    }
    if (!current_) {
        current_ = n;
    } else {
        old_ = current_;
        current_ = n;
        xfPos_ = 0;
    }
}

void PartitionedConvolver::accumulate(const PartitionedKernel& k, float* acc) noexcept {
    // Planar complex MAC; per bin the same operations in the same order as the interleaved
    // form (bit-identical), but contiguous so the compiler vectorises it.
    const std::size_t S = stride_;
    float* BF_RESTRICT ar = acc;
    float* BF_RESTRICT ai = acc + S;
    std::fill(acc, acc + 2 * S, 0.0f);
    const std::size_t np = std::min(k.numPartitions(), maxParts_);
    std::size_t slot = head_;
    for (std::size_t p = 0; p < np; ++p) {
        const float* BF_RESTRICT kr = k.partition(p);
        const float* BF_RESTRICT ki = kr + S;
        const float* BF_RESTRICT xr = fdl_.data() + slot * 2 * S;
        const float* BF_RESTRICT xi = xr + S;
        for (std::size_t i = 0; i < S; ++i) {
            ar[i] += xr[i] * kr[i] - xi[i] * ki[i];
            ai[i] += xr[i] * ki[i] + xi[i] * kr[i];
        }
        slot = slot == 0 ? maxParts_ - 1 : slot - 1;
    }
}

void PartitionedConvolver::runBlock() noexcept {
    const std::size_t S = stride_, B = block_;
    const auto Bd = static_cast<std::ptrdiff_t>(B);
    const std::int64_t blockStart = samplesIn_ - static_cast<std::int64_t>(B);
    pickUpPending(blockStart);
    // Overlap-save input frame.
    std::copy(frame_.begin() + Bd, frame_.end(), frame_.begin());
    std::copy(inFifo_.begin(), inFifo_.end(), frame_.begin() + Bd);
    head_ = (head_ + 1) % maxParts_;
    float* x = fdl_.data() + head_ * 2 * S;
    fft_->forwardSplit(frame_.data(), x, x + S);
    if (!current_) {
        std::fill(outFifo_.begin(), outFifo_.end(), 0.0f);
        return;
    }
    accumulate(*current_, accNew_.data());
    fft_->inverseSplit(accNew_.data(), accNew_.data() + S, yNew_.data());
    if (!old_) {
        std::copy(yNew_.begin() + Bd, yNew_.end(), outFifo_.begin());
        return;
    }
    accumulate(*old_, accOld_.data());
    fft_->inverseSplit(accOld_.data(), accOld_.data() + S, yOld_.data());
    for (std::size_t i = 0; i < B; ++i) {
        float gOld = 0.0f, gNew = 1.0f;
        if (xfPos_ < xfLen_) {
            gOld = xfOld_[xfPos_];
            gNew = xfNew_[xfPos_];
            ++xfPos_;
        }
        outFifo_[i] = gOld * yOld_[B + i] + gNew * yNew_[B + i];
    }
    if (xfPos_ >= xfLen_) {
        if (!tryRetire(old_)) retireHold_ = old_;
        old_ = nullptr;
    }
}

void PartitionedConvolver::process(const float* in, float* out, std::size_t n) noexcept {
    if (!fft_) {
        std::fill(out, out + n, 0.0f);
        return;
    }
    // Chunks up to the next block boundary. in == out is allowed: each chunk's input is
    // consumed before its output is written.
    std::size_t i = 0;
    while (i < n) {
        const std::size_t len = std::min(n - i, block_ - pos_);
        std::memmove(inFifo_.data() + pos_, in + i, len * sizeof(float));
        std::memmove(out + i, outFifo_.data() + pos_, len * sizeof(float));
        samplesIn_ += static_cast<std::int64_t>(len);
        pos_ += len;
        i += len;
        if (pos_ == block_) {
            pos_ = 0;
            runBlock();
        }
    }
}

}  // namespace bf
