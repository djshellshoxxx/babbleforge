#include "core/engine/StationaryMaskEngine.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace bf {

void StationaryMaskEngine::prepare(double fs, std::size_t numChannels, std::uint64_t masterSeed,
                                   std::size_t maxKernelLen, float levelDbfs, std::uint64_t epoch) {
    channels_.clear();
    for (std::size_t c = 0; c < numChannels; ++c) {
        const std::string name = "noise.ch." + std::to_string(c);
        auto ch = std::make_unique<Channel>(deriveStreamSeed(masterSeed, name, epoch));
        ch->conv.prepare(fs, maxKernelLen);
        channels_.push_back(std::move(ch));
    }
    noise_.assign(kChunk, 0.0f);
    gain_.assign(kChunk, 0.0f);
    targetDb_.store(levelDbfs);
    gainTargetDb_ = levelDbfs;
    gainTarget_ = std::pow(10.0, static_cast<double>(levelDbfs) / 20.0);
    gainCur_ = gainTarget_;
    gainCoef_ = 1.0 - std::exp(-1.0 / (0.2 * fs));
    sample_ = 0;
}

bool StationaryMaskEngine::setFilter(const std::vector<float>& h, std::int64_t effectiveSample) {
    if (channels_.empty()) return true;
    // One set of spectra shared by every channel (one kernel object per convolver).
    auto proto = PartitionedKernel::create(h.data(), h.size(), channels_.front()->conv.partitionSize());
    proto->effectiveSample = effectiveSample;
    bool ok = true;
    for (auto& ch : channels_) ok = ch->conv.postKernel(proto->clone()) && ok;
    return ok;
}

std::size_t StationaryMaskEngine::collectGarbage() {
    std::size_t n = 0;
    for (auto& ch : channels_) n += ch->conv.collectGarbage();
    return n;
}

void StationaryMaskEngine::process(float* const* out, std::size_t nFrames) noexcept {
    constexpr float kSqrt3 = 1.7320508075688772f;
    std::size_t off = 0;
    while (off < nFrames) {
        const std::size_t len = std::min(kChunk, nFrames - off);
        for (std::size_t i = 0; i < len; ++i) {
            if (sample_ % static_cast<std::int64_t>(kGainGrid) == 0) {
                const float db = targetDb_.load(std::memory_order_relaxed);
                if (db != gainTargetDb_) {
                    gainTargetDb_ = db;
                    gainTarget_ = std::pow(10.0, static_cast<double>(db) / 20.0);
                }
            }
            gainCur_ += gainCoef_ * (gainTarget_ - gainCur_);
            gain_[i] = static_cast<float>(gainCur_);
            ++sample_;
        }
        for (std::size_t c = 0; c < channels_.size(); ++c) {
            Channel& ch = *channels_[c];
            for (std::size_t i = 0; i < len; ++i) noise_[i] = kSqrt3 * ch.rng.uniformPM1f();
            float* o = out[c] + off;
            ch.conv.process(noise_.data(), o, len);
            for (std::size_t i = 0; i < len; ++i) o[i] *= gain_[i];
        }
        off += len;
    }
}

}  // namespace bf
