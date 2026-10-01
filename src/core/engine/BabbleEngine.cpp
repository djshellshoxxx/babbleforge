#include "core/engine/BabbleEngine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bf {

namespace {
constexpr double kTrimCadenceS = 5.0;            // analysis cadence (REALTIME §8.4)
constexpr std::size_t kTrimWindows = 2;          // 10 s measurement window
constexpr double kTrimTau = 30.0, kTrimClampDb = 6.0, kTrimSlewDbPerS = 0.5;
constexpr double kTrimFreezeS = 10.0;

SelectorConfig selectorConfigFor(const BabbleEngineConfig& c) {
    SelectorConfig s;
    s.poolSize = c.plan.pool;
    s.voiceSlots = c.plan.slots();
    s.meanActive = c.plan.targetMean();
    s.diversity = c.diversity;
    s.languageAware = c.languageAware;
    s.target = c.target;
    s.seed = c.plan.seed;
    s.segMinS = c.plan.segMinS;
    s.rotationPeriodS = c.plan.mode == PlanMode::ContinuousN ? 0.0 : c.rotationPeriodS;
    s.soloRiskWeight = c.plan.cvrSoloRiskWeight;
    s.segCooldownOverrideS = c.segCooldownOverrideS;
    s.clockRate = c.fs;
    return s;
}
}  // namespace

BabbleEngine::BabbleEngine(std::shared_ptr<const CorpusSnapshot> snap, IAudioSource& source,
                           const BabbleEngineConfig& cfg)
    : snap_(std::move(snap)), cfg_(cfg), preparer_(source) {
    cfg_.plan.talkerRefDbfs = cfg_.busLevelDbfs;
    if (!isSupportedBabbleRate(cfg_.fs)) cfg_.fs = 48000.0;  // callers validate; keep a sane clock
    fs_ = static_cast<std::int64_t>(cfg_.fs);
    trimCadence_ = static_cast<std::int64_t>(std::llround(kTrimCadenceS * cfg_.fs));
    trimFreeze_ = static_cast<std::int64_t>(std::llround(kTrimFreezeS * cfg_.fs));
    selector_ = std::make_unique<SegmentSelector>(snap_, selectorConfigFor(cfg_));
    planner_ = std::make_unique<TalkerPlanner>(snap_, *selector_, cfg_.plan, cfg_.fs);
    if (cfg_.retainLayouts) planner_->setLayoutRetention(TalkerPlanner::LayoutRetention::All);

    const std::size_t V = std::max<std::size_t>(cfg_.plan.slots(), 1);
    const std::size_t maxVoices = 2 * V + 16;
    const std::size_t blocks = cfg_.blockPoolBlocks ? cfg_.blockPoolBlocks : 4 * maxVoices + 16;
    pool_ = std::make_unique<BlockPool>(blocks);
    for (std::size_t i = 0; i < maxVoices; ++i) {
        chainStore_.push_back(std::make_unique<BlockChain>());
        freeChains_.push_back(chainStore_.back().get());
    }
    renderer_.prepare(cfg_.numChannels, maxVoices, pool_.get(), cfg_.fs, cfg_.externalFeed);
    ptrs_.assign(cfg_.numChannels, nullptr);
    gbnorm_ = computeCountNorm();
    renderer_.setCountNorm(gbnorm_, 0, true);
    nextTrimUpdate_ = trimCadence_;
    trimFrozenUntil_ = trimFreeze_;
}

BabbleEngine::~BabbleEngine() = default;

double BabbleEngine::computeCountNorm() const {
    const double F = planner_->estimateBusPowerFactor(cfg_.probeSeconds);
    return F > 1e-9 ? 1.0 / std::sqrt(F) : 1.0;
}

void BabbleEngine::scheduleReplan(const TalkerPlanParams& params, std::int64_t atSample) {
    TalkerPlanParams p = params;
    p.talkerRefDbfs = cfg_.busLevelDbfs;
    Replan r{p, std::max(atSample, pos_)};
    auto it = std::upper_bound(replans_.begin(), replans_.end(), r.at,
                               [](std::int64_t v, const Replan& x) { return v < x.at; });
    replans_.insert(it, r);
}

void BabbleEngine::applyReplan() {
    const Replan r = replans_.front();
    replans_.pop_front();
    const auto discarded = planner_->replan(r.params, r.at);
    if (!discarded.empty()) {
        std::erase_if(pending_, [&](const PlannedEvent& pe) {
            return std::find(discarded.begin(), discarded.end(), pe.ev.eventId) != discarded.end();
        });
        renderer_.dropStale(planner_->epoch(), r.at + planner_->freezeSamples());
    }
    gbnorm_ = computeCountNorm();
    renderer_.setCountNorm(gbnorm_, r.at);
    trimFrozenUntil_ = r.at + trimFreeze_;
}

std::vector<std::uint64_t> BabbleEngine::applyReplanNow(const TalkerPlanParams& params, std::int64_t atSample) {
    TalkerPlanParams p = params;
    p.talkerRefDbfs = cfg_.busLevelDbfs;
    const auto discarded = planner_->replan(p, atSample);
    if (!discarded.empty()) renderer_.dropStale(planner_->epoch(), atSample + planner_->freezeSamples());
    gbnorm_ = computeCountNorm();
    renderer_.setCountNorm(gbnorm_, atSample);
    return discarded;
}

void BabbleEngine::consumeOccupancy() noexcept {
    OccupancyFrame fr;
    while (renderer_.popOccupancy(fr)) {
        if (fr.sample < 0) continue;
        ++stats_.frames;
        stats_.sumActive += fr.activeTalkers;
        stats_.sumSpeaking += fr.speakingTalkers;
        stats_.minActive = std::min<std::uint32_t>(stats_.minActive, fr.activeTalkers);
        stats_.maxActive = std::max<std::uint32_t>(stats_.maxActive, fr.activeTalkers);
    }
}

void BabbleEngine::render(float* const* out, int nCh, int nFrames) {
    if (nCh != static_cast<int>(cfg_.numChannels) || nFrames <= 0) return;
    if (cfg_.externalFeed) {  // RT: planning and preparation run on the host's threads
        renderer_.process(out, static_cast<std::size_t>(nFrames));
        pos_ += nFrames;
        return;
    }
    std::vector<float*>& ptrs = ptrs_;
    std::size_t off = 0;
    const auto total = static_cast<std::size_t>(nFrames);
    while (off < total) {
        while (!replans_.empty() && replans_.front().at <= pos_) applyReplan();
        std::size_t n = total - off;
        if (!replans_.empty()) n = std::min<std::size_t>(n, static_cast<std::size_t>(replans_.front().at - pos_));
        if (cfg_.trimEnabled) n = std::min<std::size_t>(n, static_cast<std::size_t>(nextTrimUpdate_ - pos_));
        for (std::size_t c = 0; c < ptrs.size(); ++c) ptrs[c] = out[c] + off;
        step(ptrs.data(), n);
        off += n;
    }
}

void BabbleEngine::step(float* const* out, std::size_t n) {
    const std::int64_t end = pos_ + static_cast<std::int64_t>(n);
    std::int64_t planTo = end + static_cast<std::int64_t>(cfg_.horizonS * cfg_.fs);
    if (!replans_.empty()) planTo = std::min(planTo, replans_.front().at + planner_->freezeSamples());
    planner_->planUntil(planTo);
    scratch_.clear();
    planner_->takeNew(scratch_);
    for (auto& pe : scratch_) pending_.push_back(std::move(pe));

    // Recycle chains of finished voices.
    BlockChain* done = nullptr;
    while (renderer_.popFinished(done)) {
        done->releaseAll(*pool_);
        std::erase_if(inflight_, [&](const Inflight& f) { return f.chain == done; });
        freeChains_.push_back(done);
    }
    // Arm events starting in this block.
    while (!pending_.empty() && pending_.front().ev.startSample < end) {
        PlannedEvent pe = std::move(pending_.front());
        pending_.pop_front();
        if (pe.ev.endSample <= pos_ || freeChains_.empty()) continue;
        Inflight f;
        f.pe = std::move(pe);
        f.chain = freeChains_.back();
        freeChains_.pop_back();
        f.chainOffset = std::max<std::int64_t>(0, pos_ - f.pe.ev.startSample);
        f.chain->reset(static_cast<std::uint64_t>(f.pe.ev.length() - f.chainOffset));
        VoiceEvent ve;
        ve.ev = f.pe.ev;
        ve.chain = f.chain;
        ve.chainOffset = f.chainOffset;
        ve.speech = f.pe.layout ? f.pe.layout->speech.data() : nullptr;
        ve.numSpeech = f.pe.layout ? static_cast<std::uint32_t>(f.pe.layout->speech.size()) : 0;
        inflight_.push_back(std::move(f));
        renderer_.pushEvent(ve);
    }
    // Synchronous preparation: exactly the audio this block needs.
    for (auto& f : inflight_) {
        if (!f.pe.layout) continue;
        const std::int64_t need = std::min(end, f.pe.ev.endSample) - f.pe.ev.startSample - f.chainOffset;
        if (need <= 0) continue;
        // The layout is event-relative; the chain starts at chainOffset.
        std::uint64_t w = f.chain->written();
        if (w < static_cast<std::uint64_t>(need)) {
            f.chain->reclaim(*pool_);
            std::vector<float>& buf = scratchAudio_;
            const std::size_t k = static_cast<std::size_t>(static_cast<std::uint64_t>(need) - w);
            buf.resize(k);
            if (!preparer_.render(*f.pe.layout, f.chainOffset + static_cast<std::int64_t>(w), buf.data(), k))
                sourceErrors_.store(true, std::memory_order_relaxed);
            f.chain->append(*pool_, buf.data(), k);
        }
    }
    renderer_.process(out, n);

    OccupancyFrame fr;
    while (renderer_.popOccupancy(fr)) {
        if (fr.sample < 0) continue;
        ++stats_.frames;
        stats_.sumActive += fr.activeTalkers;
        stats_.sumSpeaking += fr.speakingTalkers;
        stats_.minActive = std::min<std::uint32_t>(stats_.minActive, fr.activeTalkers);
        stats_.maxActive = std::max<std::uint32_t>(stats_.maxActive, fr.activeTalkers);
    }
    if (cfg_.trimEnabled) updateTrim(out, n);
    pos_ = end;
}

void BabbleEngine::updateTrim(float* const* out, std::size_t n) {
    for (std::size_t c = 0; c < cfg_.numChannels; ++c)
        for (std::size_t i = 0; i < n; ++i) trimAcc_ += static_cast<double>(out[c][i]) * out[c][i];
    trimAccN_ += static_cast<std::int64_t>(n);
    if (pos_ + static_cast<std::int64_t>(n) < nextTrimUpdate_) return;
    trimWindows_.push_back(trimAcc_ / static_cast<double>(std::max<std::int64_t>(trimAccN_, 1)));
    if (trimWindows_.size() > kTrimWindows) trimWindows_.pop_front();
    trimAcc_ = 0.0;
    trimAccN_ = 0;
    const std::int64_t now = nextTrimUpdate_;
    nextTrimUpdate_ += trimCadence_;
    if (now < trimFrozenUntil_ || trimWindows_.size() < kTrimWindows) return;
    double p = 0.0;
    for (double w : trimWindows_) p += w;
    p /= static_cast<double>(trimWindows_.size());
    if (!(p > 0.0)) return;
    const double measDb = 10.0 * std::log10(p);
    const double dt = static_cast<double>(trimCadence_) / cfg_.fs;
    double step = (cfg_.busLevelDbfs - measDb) * dt / kTrimTau;
    step = std::clamp(step, -kTrimSlewDbPerS * dt, kTrimSlewDbPerS * dt);
    trimDb_ = std::clamp(trimDb_ + step, -kTrimClampDb, kTrimClampDb);
    renderer_.setTrimDb(trimDb_, now);
}

}  // namespace bf
