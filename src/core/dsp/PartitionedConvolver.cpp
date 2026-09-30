#include "core/dsp/PartitionedConvolver.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "core/math/DetMath.h"

namespace bf {

std::unique_ptr<PartitionedKernel> PartitionedKernel::create(const float* h, std::size_t len) {
    std::unique_ptr<PartitionedKernel> k(new PartitionedKernel());
    k->length_ = len;
    k->numPartitions_ = std::max<std::size_t>(1, (len + kPartition - 1) / kPartition);
    k->data_.assign(k->numPartitions_ * 2 * kBins, 0.0f);
    FftF fft(kFftSize);
    std::vector<float> buf(kFftSize);
    for (std::size_t p = 0; p < k->numPartitions_; ++p) {
        std::fill(buf.begin(), buf.end(), 0.0f);
        for (std::size_t i = 0; i < kPartition && p * kPartition + i < len; ++i) buf[i] = h[p * kPartition + i];
        fft.forward(buf.data(), reinterpret_cast<std::complex<float>*>(k->data_.data() + p * 2 * kBins));
    }
    return k;
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
    constexpr std::size_t B = PartitionedKernel::kBins;
    fft_ = std::make_unique<FftF>(PartitionedKernel::kFftSize);
    maxParts_ = std::max<std::size_t>(1, (maxKernelLen + kBlock - 1) / kBlock);
    fdl_.assign(maxParts_ * 2 * B, 0.0f);
    head_ = 0;
    frame_.assign(PartitionedKernel::kFftSize, 0.0f);
    spec_.assign(2 * B, 0.0f);
    accNew_.assign(2 * B, 0.0f);
    accOld_.assign(2 * B, 0.0f);
    yNew_.assign(PartitionedKernel::kFftSize, 0.0f);
    yOld_.assign(PartitionedKernel::kFftSize, 0.0f);
    inFifo_.assign(kBlock, 0.0f);
    outFifo_.assign(kBlock, 0.0f);
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
    if (!kernel || !fft_ || kernel->numPartitions() > maxParts_) return false;
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
    constexpr std::size_t B = PartitionedKernel::kBins;
    std::fill(acc, acc + 2 * B, 0.0f);
    const std::size_t np = std::min(k.numPartitions(), maxParts_);
    for (std::size_t p = 0; p < np; ++p) {
        const float* kp = k.partition(p);
        const float* xp = fdl_.data() + ((head_ + maxParts_ - p) % maxParts_) * 2 * B;
        for (std::size_t i = 0; i < 2 * B; i += 2) {
            const float xr = xp[i], xi = xp[i + 1], kr = kp[i], ki = kp[i + 1];
            acc[i] += xr * kr - xi * ki;
            acc[i + 1] += xr * ki + xi * kr;
        }
    }
}

void PartitionedConvolver::runBlock() noexcept {
    constexpr std::size_t B = PartitionedKernel::kBins;
    const std::int64_t blockStart = samplesIn_ - static_cast<std::int64_t>(kBlock);
    pickUpPending(blockStart);
    // Overlap-save input frame.
    std::copy(frame_.begin() + kBlock, frame_.end(), frame_.begin());
    std::copy(inFifo_.begin(), inFifo_.end(), frame_.begin() + kBlock);
    head_ = (head_ + 1) % maxParts_;
    fft_->forward(frame_.data(), reinterpret_cast<std::complex<float>*>(fdl_.data() + head_ * 2 * B));
    if (!current_) {
        std::fill(outFifo_.begin(), outFifo_.end(), 0.0f);
        return;
    }
    accumulate(*current_, accNew_.data());
    fft_->inverse(reinterpret_cast<const std::complex<float>*>(accNew_.data()), yNew_.data());
    if (!old_) {
        std::copy(yNew_.begin() + kBlock, yNew_.end(), outFifo_.begin());
        return;
    }
    accumulate(*old_, accOld_.data());
    fft_->inverse(reinterpret_cast<const std::complex<float>*>(accOld_.data()), yOld_.data());
    for (std::size_t i = 0; i < kBlock; ++i) {
        float gOld = 0.0f, gNew = 1.0f;
        if (xfPos_ < xfLen_) {
            gOld = xfOld_[xfPos_];
            gNew = xfNew_[xfPos_];
            ++xfPos_;
        }
        outFifo_[i] = gOld * yOld_[kBlock + i] + gNew * yNew_[kBlock + i];
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
    for (std::size_t i = 0; i < n; ++i) {
        const float x = in[i];
        out[i] = outFifo_[pos_];
        inFifo_[pos_] = x;
        ++samplesIn_;
        if (++pos_ == kBlock) {
            pos_ = 0;
            runBlock();
        }
    }
}

}  // namespace bf
