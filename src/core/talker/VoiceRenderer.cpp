#include "core/talker/VoiceRenderer.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace bf {

namespace {
constexpr double kHalfPi = 1.57079632679489661923;
constexpr std::size_t kMaxDrainPerSub = 32;
}  // namespace

void VoiceRenderer::prepare(std::size_t numChannels, std::size_t maxVoices, const BlockPool* pool, double fs) {
    nCh_ = std::clamp<std::size_t>(numChannels, 1, kMaxChannels);
    fs_ = fs;
    pool_ = pool;
    voices_.assign(maxVoices, Voice{});
    order_.clear();
    order_.reserve(maxVoices);
    src_.assign(kSubBlock, 0.0f);
    gainBuf_.assign(kSubBlock, 0.0f);
    fifo_ = std::make_unique<Spsc<VoiceEvent, kFifoSize>>();
    finished_ = std::make_unique<Spsc<BlockChain*, kFifoSize>>();
    cmds_ = std::make_unique<Spsc<Cmd, 64>>();
    occ_ = std::make_unique<Spsc<OccupancyFrame, kOccSize>>();
    slotGains_ = std::make_unique<std::array<std::atomic<float>, kMaxSlots * kMaxChannels>>();
    slotGainVer_ = std::make_unique<std::array<std::atomic<std::uint32_t>, kMaxSlots>>();
    for (auto& g : *slotGains_) g.store(0.0f, std::memory_order_relaxed);
    for (auto& v : *slotGainVer_) v.store(0, std::memory_order_relaxed);
    numPending_ = 0;
    gbCur_ = gbStart_ = gbTarget_ = 1.0;
    gbRampPos_ = gbRampLen_ = static_cast<std::int64_t>(std::llround(2.0 * fs));
    trimCur_ = trimTarget_ = 1.0;
    coefGain_ = 1.0 - std::exp(-1.0 / (0.020 * fs));
    coefCh_ = 1.0 - std::exp(-1.0 / (0.050 * fs));
    minEpoch_ = 0;
    freeze_ = 0;
    pos_ = 0;
}

bool VoiceRenderer::pushEvent(const VoiceEvent& e) noexcept { return fifo_->push(e); }
bool VoiceRenderer::popFinished(BlockChain*& chain) noexcept { return finished_->pop(chain); }
bool VoiceRenderer::popOccupancy(OccupancyFrame& f) noexcept { return occ_->pop(f); }

void VoiceRenderer::setCountNorm(double gLin, std::int64_t effectiveSample, bool immediate) noexcept {
    cmds_->push(Cmd{immediate ? CmdType::CountNormNow : CmdType::CountNorm, gLin, effectiveSample, 0});
}
void VoiceRenderer::setTrimDb(double db, std::int64_t effectiveSample) noexcept {
    cmds_->push(Cmd{CmdType::Trim, db, effectiveSample, 0});
}
void VoiceRenderer::dropStale(std::uint32_t minEpoch, std::int64_t freezeSample) noexcept {
    cmds_->push(Cmd{CmdType::Drop, 0.0, freezeSample, minEpoch});
}

void VoiceRenderer::setGains(std::uint32_t slot, const float* gains, std::size_t n) noexcept {
    if (slot >= kMaxSlots) return;
    for (std::size_t c = 0; c < kMaxChannels; ++c)
        (*slotGains_)[slot * kMaxChannels + c].store(c < n ? gains[c] : 0.0f, std::memory_order_relaxed);
    auto& ver = (*slotGainVer_)[slot];
    std::uint32_t v = ver.load(std::memory_order_relaxed) + 1;
    if (v == 0) v = 1;
    ver.store(v, std::memory_order_release);
}

void VoiceRenderer::clearGains(std::uint32_t slot) noexcept {
    if (slot < kMaxSlots) (*slotGainVer_)[slot].store(0, std::memory_order_release);
}

void VoiceRenderer::computeTargets(Voice& v) noexcept {
    const std::uint32_t slot = v.e.ev.slot;
    const std::uint32_t ver = slot < kMaxSlots ? (*slotGainVer_)[slot].load(std::memory_order_acquire) : 0;
    v.gainVer = ver;
    v.chTarget.fill(0.0f);
    if (ver != 0) {
        for (std::size_t c = 0; c < nCh_; ++c)
            v.chTarget[c] = (*slotGains_)[slot * kMaxChannels + c].load(std::memory_order_relaxed);
    } else if (nCh_ == 1) {
        v.chTarget[0] = 1.0f;
    } else {
        const double th = (static_cast<double>(v.e.ev.pan) + 1.0) * 0.5 * kHalfPi;
        v.chTarget[0] = static_cast<float>(std::cos(th));
        v.chTarget[1] = static_cast<float>(std::sin(th));
    }
}

void VoiceRenderer::freeVoice(std::size_t i) noexcept {
    Voice& v = voices_[i];
    if (v.e.chain) {
        v.e.chain->markFinished();
        finished_->push(v.e.chain);
    }
    v.used = false;
    v.e = VoiceEvent{};
    order_.erase(std::find(order_.begin(), order_.end(), static_cast<std::uint32_t>(i)));
}

void VoiceRenderer::pollCommands() noexcept {
    Cmd c;
    while (cmds_->pop(c)) {
        if (c.type == CmdType::Drop) {
            minEpoch_ = std::max(minEpoch_, c.epoch);
            freeze_ = c.effective;
            for (std::size_t k = order_.size(); k-- > 0;) {
                const std::size_t i = order_[k];
                const auto& ev = voices_[i].e.ev;
                if (ev.epoch < minEpoch_ && ev.startSample >= freeze_ && ev.startSample > pos_) freeVoice(i);
            }
        } else if (numPending_ < pending_.size()) {
            pending_[numPending_++] = c;
        }
    }
}

void VoiceRenderer::applyControls(std::int64_t cellStart) noexcept {
    std::size_t w = 0;
    for (std::size_t k = 0; k < numPending_; ++k) {
        const Cmd& c = pending_[k];
        if (c.effective > cellStart) { pending_[w++] = c; continue; }
        switch (c.type) {
        case CmdType::CountNormNow:
            gbCur_ = gbStart_ = gbTarget_ = c.value;
            gbRampPos_ = gbRampLen_;
            break;
        case CmdType::CountNorm:
            gbStart_ = gbCur_;
            gbTarget_ = c.value;
            gbRampPos_ = 0;
            break;
        case CmdType::Trim:
            trimTarget_ = std::pow(10.0, c.value / 20.0);
            break;
        case CmdType::Drop:
            break;
        }
    }
    numPending_ = w;
}

void VoiceRenderer::drain() noexcept {
    VoiceEvent e;
    for (std::size_t k = 0; k < kMaxDrainPerSub && fifo_->pop(e); ++k) {
        const bool stale = e.ev.epoch < minEpoch_ && e.ev.startSample >= freeze_;
        std::size_t i = 0;
        while (i < voices_.size() && voices_[i].used) ++i;
        if (stale || i == voices_.size() || e.ev.endSample <= pos_) {
            if (!stale) dropped_.fetch_add(1, std::memory_order_relaxed);
            if (e.chain) { e.chain->markFinished(); finished_->push(e.chain); }
            continue;
        }
        Voice& v = voices_[i];
        v.used = true;
        v.e = e;
        v.speechIdx = 0;
        v.gainTarget = v.gain = static_cast<double>(e.ev.segGainLin);
        computeTargets(v);
        v.chGain = v.chTarget;
        auto it = std::lower_bound(order_.begin(), order_.end(), e.ev.eventId,
                                   [&](std::uint32_t vi, std::uint64_t id) { return voices_[vi].e.ev.eventId < id; });
        order_.insert(it, static_cast<std::uint32_t>(i));
    }
}

bool VoiceRenderer::speechAt(Voice& v, std::int64_t rel) noexcept {
    while (v.speechIdx < v.e.numSpeech && v.e.speech[v.speechIdx].end <= rel) ++v.speechIdx;
    return v.speechIdx < v.e.numSpeech && v.e.speech[v.speechIdx].start <= rel;
}

void VoiceRenderer::publishOccupancy(std::int64_t t) noexcept {
    std::uint64_t act = 0, spk = 0;
    for (std::uint32_t i : order_) {
        Voice& v = voices_[i];
        const auto& ev = v.e.ev;
        if (t < ev.startSample || t >= ev.endSample) continue;
        const std::uint64_t bit = std::uint64_t{1} << (ev.slot % kMaxSlots);
        act |= bit;
        if (speechAt(v, t - ev.startSample)) spk |= bit;
    }
    OccupancyFrame f;
    f.sample = t;
    f.activeTalkers = static_cast<std::uint16_t>(std::popcount(act));
    f.speakingTalkers = static_cast<std::uint16_t>(std::popcount(spk));
    lastKa_.store(f.activeTalkers, std::memory_order_relaxed);
    lastKs_.store(f.speakingTalkers, std::memory_order_relaxed);
    occ_->push(f);  // dropped when the consumer does not keep up
}

void VoiceRenderer::renderSub(float* const* out, std::size_t offset, std::size_t n) noexcept {
    for (std::size_t c = 0; c < nCh_; ++c) std::memset(out[c] + offset, 0, n * sizeof(float));
    const std::int64_t t0 = pos_, t1 = pos_ + static_cast<std::int64_t>(n);
    std::size_t k = 0;
    while (k < order_.size()) {
        const std::uint32_t vi = order_[k];
        Voice& v = voices_[vi];
        const TalkerEvent& ev = v.e.ev;
        const std::int64_t a = std::max(t0, ev.startSample), b = std::min(t1, ev.endSample);
        if (b > a) {
            const std::int64_t rel0 = a - ev.startSample;
            const std::size_t cnt = static_cast<std::size_t>(b - a);
            if (!v.e.chain || rel0 < v.e.chainOffset ||
                !v.e.chain->read(*pool_, static_cast<std::uint64_t>(rel0 - v.e.chainOffset), src_.data(), cnt)) {
                underflows_.fetch_add(1, std::memory_order_relaxed);
                std::memset(src_.data(), 0, cnt * sizeof(float));
            }
            if (ev.slot < kMaxSlots &&
                (*slotGainVer_)[ev.slot].load(std::memory_order_acquire) != v.gainVer)
                computeTargets(v);
            const std::int64_t fi = ev.fadeInLen;
            const std::int64_t fos = ev.fadeOutStart - ev.startSample;
            const std::int64_t foLen = ev.endSample - ev.fadeOutStart;
            for (std::size_t i = 0; i < cnt; ++i) {
                const std::int64_t rel = rel0 + static_cast<std::int64_t>(i);
                double fade = 1.0;
                if (rel < fi) fade = std::sin(kHalfPi * static_cast<double>(rel) / static_cast<double>(fi));
                if (rel >= fos && foLen > 0)
                    fade *= std::cos(kHalfPi * static_cast<double>(rel - fos) / static_cast<double>(foLen));
                v.gain += coefGain_ * (v.gainTarget - v.gain);
                gainBuf_[i] = static_cast<float>(v.gain * fade) * src_[i];
            }
            const std::size_t o = offset + static_cast<std::size_t>(a - t0);
            for (std::size_t c = 0; c < nCh_; ++c) {
                float g = v.chGain[c];
                const float tg = v.chTarget[c];
                if (g <= 1e-5f && tg <= 1e-5f) continue;
                const float coef = static_cast<float>(coefCh_);
                float* dst = out[c] + o;
                for (std::size_t i = 0; i < cnt; ++i) {
                    g += coef * (tg - g);
                    dst[i] += g * gainBuf_[i];
                }
                v.chGain[c] = g;
            }
            if (v.e.chain && rel0 >= v.e.chainOffset)
                v.e.chain->setReadIndex(static_cast<std::uint64_t>(rel0 - v.e.chainOffset) + cnt);
        }
        if (t1 >= ev.endSample) { freeVoice(vi); continue; }  // order_ shrinks; k stays
        ++k;
    }
    // Bus gain: g_bnorm (linear 2 s ramp) x g_btrim (one-pole 20 ms).
    for (std::size_t i = 0; i < n; ++i) {
        if (gbRampPos_ < gbRampLen_) {
            ++gbRampPos_;
            gbCur_ = gbStart_ + (gbTarget_ - gbStart_) * static_cast<double>(gbRampPos_) / static_cast<double>(gbRampLen_);
        }
        trimCur_ += coefGain_ * (trimTarget_ - trimCur_);
        gainBuf_[i] = static_cast<float>(gbCur_ * trimCur_);
    }
    for (std::size_t c = 0; c < nCh_; ++c) {
        float* dst = out[c] + offset;
        for (std::size_t i = 0; i < n; ++i) dst[i] *= gainBuf_[i];
    }
}

void VoiceRenderer::process(float* const* out, std::size_t nFrames) noexcept {
    std::size_t offset = 0;
    while (offset < nFrames) {
        const std::int64_t cellStart = (pos_ >= 0 ? pos_ / static_cast<std::int64_t>(kSubBlock)
                                                  : (pos_ - static_cast<std::int64_t>(kSubBlock) + 1) /
                                                        static_cast<std::int64_t>(kSubBlock)) *
                                       static_cast<std::int64_t>(kSubBlock);
        const std::size_t n = std::min(nFrames - offset,
                                       static_cast<std::size_t>(cellStart + static_cast<std::int64_t>(kSubBlock) - pos_));
        pollCommands();
        drain();
        if (pos_ == cellStart) {
            applyControls(cellStart);
            publishOccupancy(pos_);
        }
        renderSub(out, offset, n);
        offset += n;
        pos_ += static_cast<std::int64_t>(n);
    }
}

}  // namespace bf
