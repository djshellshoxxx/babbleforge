#include "core/engine/MaskEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

#include "core/spectrum/FirDesigner.h"

namespace bf {

namespace {

constexpr double kTrimTauS = 30.0, kTrimClampDb = 6.0, kTrimSlewDbPerS = 0.5;
constexpr double kTrimFreezeS = 10.0;       // after start / plan change (ENGINE.md §3.2)
constexpr double kCrossfadeS = 2.0;         // b ramp (MASK_STRATEGIES §7 step 4)
constexpr double kGainSmoothS = 1.0;        // per-channel babble gain smoothing
constexpr double kMasterTauS = 0.200;       // Strength smoothing (ENGINE.md §3.1)
constexpr std::size_t kStationaryMaxTaps = 16384;
constexpr std::size_t kBabbleMaxTaps = 8192;
constexpr double kHistMinDb = -200.0, kHistStepDb = 0.1;
constexpr std::size_t kHistBins = 2400;

double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
double powDb(double p) { return p > 1e-30 ? 10.0 * std::log10(p) : -200.0; }

DiversityMode diversityFrom(const std::string& s) {
    if (s == "low") return DiversityMode::Low;
    if (s == "balanced") return DiversityMode::Balanced;
    if (s == "matched") return DiversityMode::Matched;
    return DiversityMode::High;
}

bool sameTarget(const SpectrumTarget& a, const SpectrumTarget& b) {
    return a.id == b.id && a.thirdOctDb == b.thirdOctDb && a.lfLimitHz == b.lfLimitHz &&
           a.hfLimitHz == b.hfLimitHz && a.lfTrimDb125 == b.lfTrimDb125;
}

bool sameSpatial(const SpatialPlanParams& a, const SpatialPlanParams& b) {
    return a.algorithm == b.algorithm && a.params.spread == b.params.spread && a.params.motion == b.params.motion &&
           a.params.variation == b.params.variation && a.params.neighbourhoodSize == b.params.neighbourhoodSize;
}

FirDesignParams firParams(double fs, const SpectrumTarget& t, bool babble) {
    FirDesignParams p;
    p.fs = fs;
    p.taps = babble ? defaultBabbleTaps(fs) : defaultStationaryTaps(fs);
    p.maxTaps = babble ? kBabbleMaxTaps : kStationaryMaxTaps;
    p.lfLimitHz = t.lfLimitHz;
    p.hfLimitHz = t.hfLimitHz;
    p.unitEnergy = !babble;
    return p;
}

// Max |d| over 125 Hz..8 kHz of the mean-removed 1/3-octave deviation, and the octave equivalent.
void shapeDevs(const ThirdOctArray& measPowerLin, const ThirdOctArray& refDb, double& thirdMax, double& octMax) {
    const ThirdOctArray measDb = powerToDb(measPowerLin);
    OperatingBands d{};
    shapeDeviation(operatingSlice(measDb), operatingSlice(refDb), d, nullptr);
    thirdMax = 0.0;
    for (std::size_t i = 1; i + 1 < kNumOperatingBands; ++i) thirdMax = std::max(thirdMax, std::fabs(d[i]));
    const OctaveArray mo = octaveDbFromPower(measPowerLin);
    const OctaveArray ro = octaveFromThirdOct(refDb);
    double wsum = 0.0, mu = 0.0;
    for (std::size_t k = 0; k < kNumOctaveBands; ++k) {
        const double w = std::pow(10.0, ro[k] / 10.0);
        mu += w * (mo[k] - ro[k]);
        wsum += w;
    }
    mu /= std::max(wsum, 1e-30);
    octMax = 0.0;
    for (std::size_t k = 0; k < kNumOctaveBands; ++k) octMax = std::max(octMax, std::fabs(mo[k] - ro[k] - mu));
}

ThirdOctArray extendCorrection(const OperatingBands& c) {
    ThirdOctArray r{};
    for (std::size_t k = 0; k < kNumThirdOctBands; ++k) {
        const std::size_t j = std::clamp(k, kFirstOperatingBand, kLastOperatingBand) - kFirstOperatingBand;
        r[k] = c[j];
    }
    return r;
}

double percentileFromHist(const std::vector<std::uint64_t>& h, double q) {
    const std::uint64_t total = std::accumulate(h.begin(), h.end(), std::uint64_t{0});
    if (total == 0) return kHistMinDb;
    const double want = q * static_cast<double>(total);
    std::uint64_t acc = 0;
    for (std::size_t i = 0; i < h.size(); ++i) {
        acc += h[i];
        if (static_cast<double>(acc) >= want) return kHistMinDb + (static_cast<double>(i) + 0.5) * kHistStepDb;
    }
    return kHistMinDb + static_cast<double>(h.size()) * kHistStepDb;
}

double percentileOf(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double pos = q * static_cast<double>(v.size() - 1);
    const std::size_t i = static_cast<std::size_t>(pos);
    const double f = pos - static_cast<double>(i);
    return i + 1 < v.size() ? v[i] * (1.0 - f) + v[i + 1] * f : v[i];
}

}  // namespace

// ------------------------------------------------------------------------------ pool LTASS

ThirdOctArray estimatePoolLtassDb(const CorpusSnapshot& snap, IAudioSource& audio,
                                  const std::vector<SpeakerId>& speakers, bool* readFailure) {
    constexpr std::size_t kAnchors = 4;
    constexpr std::int64_t kLen = 3 * kCorpusRate;  // raw corpus audio (48 kHz domain)
    ThirdOctArray acc{};
    std::size_t used = 0;
    std::vector<float> buf;
    for (SpeakerId s : speakers) {
        if (s >= snap.numSpeakers()) continue;
        const auto anchors = snap.anchorsOf(s);
        if (anchors.empty()) continue;
        buf.clear();
        const std::size_t na = std::min(kAnchors, anchors.size());
        for (std::size_t k = 0; k < na; ++k) {
            const SegmentRec& seg = anchors[(k * anchors.size()) / na];
            const std::int64_t n = std::min<std::int64_t>(kLen, seg.maxLen);
            if (n <= 0) continue;
            const std::size_t off = buf.size();
            buf.resize(off + static_cast<std::size_t>(n));
            if (!audio.read(seg.recording, static_cast<std::uint64_t>(seg.anchor), buf.data() + off,
                            static_cast<std::size_t>(n))) {
                if (readFailure) *readFailure = true;
                buf.resize(off);
            }
        }
        if (buf.size() < 16384) continue;
        const float* ch = buf.data();
        const SpectrumAnalysis a = analyzeSpectrum(&ch, 1, buf.size(), static_cast<double>(kCorpusRate));
        double tot = 0.0;
        for (double p : a.overallPowerLin) tot += p;
        if (!(tot > 0.0)) continue;
        for (std::size_t k = 0; k < kNumThirdOctBands; ++k) acc[k] += a.overallPowerLin[k] / tot;
        ++used;
    }
    ThirdOctArray db{};
    if (used == 0) return db;  // flat: no information
    const double ref = std::max(acc[kBand1k], 1e-30);
    for (std::size_t k = 0; k < kNumThirdOctBands; ++k) db[k] = 10.0 * std::log10(std::max(acc[k], 1e-12 * ref) / ref);
    return db;
}

// ------------------------------------------------------------------------------ MaskEngine

MaskEngine::MaskEngine(MaskEngineConfig cfg) : cfg_(std::move(cfg)) {}
MaskEngine::~MaskEngine() = default;

bool MaskEngine::prepare(double fs, const OutputLayout& layout, int maxBlock, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    if (!(fs >= 8000.0 && fs <= 192000.0)) return fail("unsupported sample rate");
    if (layout.size() < 1 || layout.size() > 32) return fail("layout must have 1..32 outputs");
    if (cfg_.corpus && !cfg_.audio) return fail("corpus without audio source");
    (void)maxBlock;  // the engine re-blocks internally into <= 256-frame cells
    fs_ = fs;
    layout_ = layout;
    nCh_ = layout.size();
    const auto N = static_cast<std::size_t>(nCh_);

    zoneOf_.assign(N, 0);
    std::vector<int> device(N);
    for (std::size_t c = 0; c < N; ++c) {
        int z = layout.outputs[c].zone;
        if (c < cfg_.channels.size()) z = cfg_.channels[c].zone;
        zoneOf_[c] = std::clamp(z, 0, HybridMixer::kMaxZones - 1);
        device[c] = static_cast<int>(c);
    }

    stationary_.prepare(fs, N, cfg_.seed, kStationaryMaxTaps, static_cast<float>(cfg_.lRefDbfs));
    mixer_.prepare(fs, zoneOf_, 0.0);
    sourceStage_.prepare(fs, static_cast<int>(N), kCell);
    matrix_.prepare(fs, kCell, zoneOf_, device, nCh_);
    for (std::size_t c = 0; c < N && c < cfg_.channels.size(); ++c) {
        const OutputChannel& oc = cfg_.channels[c];
        const int o = static_cast<int>(c);
        matrix_.setOutputGainDb(o, oc.gainDb);
        matrix_.setMute(o, oc.mute);
        matrix_.setOutputEnabled(o, oc.enabled);
        matrix_.setPolarityInvert(o, oc.polarityInvert);
        matrix_.setDelayMs(o, oc.delayMs);
    }
    for (const OutputZone& z : cfg_.zones) {
        if (z.id < 0 || z.id >= OutputMatrix::kMaxZones) continue;
        matrix_.setZoneGainDb(z.id, z.levelDb);
        matrix_.setZoneEnabled(z.id, z.enabled);
    }
    matrix_.applyImmediately();
    limiter_.prepare(fs, nCh_, kCell, zoneOf_);
    decorrelator_.prepare(fs, nCh_, nCh_ > 1 ? variation_ : SpeakerVariation::Low, cfg_.seed);
    balance_.prepare(nCh_);

    babbleAnalyzer_.prepare(fs, N);
    stationaryAnalyzer_.prepare(fs, N);
    CorrectionConfig cc;
    cc.speed = cfg_.correctionSpeed;
    cc.strict = cfg_.correctionStrict;
    correction_.setConfig(cc);
    correction_.reset();
    if (cfg_.initialCorrection) correction_.setInitial(*cfg_.initialCorrection);
    meters_.prepare(fs, nCh_);
    modulation_.reset();
    levelHist_.assign(kHistBins, 0);

    frameLen_ = static_cast<int>(std::lround(fs / 100.0));
    framePos_ = 0;
    frameAcc_ = 0.0;
    blockLen_ = static_cast<std::int64_t>(std::llround(5.0 * fs));
    ffLen_ = static_cast<std::int64_t>(std::llround(fs));
    motionLen_ = 48 * kCell;
    nextBlock_ = blockLen_;
    nextFf_ = ffLen_;
    nextMotion_ = motionLen_;
    blockPowT1_.assign(N, 0.0);
    recentT1_.clear();
    blockN_ = 0;

    sumT1_.assign(N, 0.0);
    sumT2_.assign(N, 0.0);
    sumT3_.assign(N, 0.0);
    sumT4_.assign(N, 0.0);
    babbleInMix_ = 0.0;
    grActiveSamples_ = 0;

    masterCoef_ = 1.0 - std::exp(-1.0 / (kMasterTauS * fs));
    gainCoef_ = 1.0 - std::exp(-1.0 / (kGainSmoothS * fs));
    staticGain_ = dbToLin(cfg_.staticGainDb);
    gainCur_.assign(N, 1.0);
    gainTarget_.assign(N, 1.0);
    gainStamped_.assign(N, 1.0);
    gainStampPending_ = false;
    trimDb_ = 0.0;
    trimFrozenUntil_ = static_cast<std::int64_t>(kTrimFreezeS * fs);

    auto alloc = [&](std::vector<std::vector<float>>& b, std::vector<float*>& p) {
        b.assign(N, std::vector<float>(kCell, 0.0f));
        p.resize(N);
        for (std::size_t c = 0; c < N; ++c) p[c] = b[c].data();
    };
    alloc(bab_, pBab_);
    alloc(sta_, pSta_);
    alloc(mix_, pMix_);
    alloc(dev_, pDev_);
    pOut_.resize(N);

    babble_.reset();
    spatial_.reset();
    current_.reset();
    pending_.clear();
    history_ = nlohmann::json::array();
    pos_ = 0;
    rtPos_.store(0, std::memory_order_release);
    inbox_ = std::make_unique<rt::SpscFifo<RtGainStamp, 64>>();
    havePendingStamp_ = false;
    anaPtr_.assign(N, nullptr);
    anaSamples_ = 0;
    rtOut_.store(RtOutputStats{});
    prepared_ = true;
    return true;
}

bool MaskEngine::setPlan(const MaskRenderPlan& plan, std::int64_t effectiveSample, std::string* error) {
    if (!prepared_) {
        if (error) *error = "engine not prepared";
        return false;
    }
    if (cfg_.realtime && current_) {
        if (error) *error = "real-time mode: plan changes other than Strength / mix / ceiling need a rebuild";
        return false;
    }
    Pending p{plan, std::max(effectiveSample, pos_)};
    auto it = std::upper_bound(pending_.begin(), pending_.end(), p.at,
                               [](std::int64_t v, const Pending& x) { return v < x.at; });
    pending_.insert(it, std::move(p));
    return true;
}

int MaskEngine::latencySamples() const noexcept {
    return static_cast<int>(PartitionedConvolver::kLatency) + (cfg_.limiterStage ? limiter_.latencySamples() : 0);
}

bool MaskEngine::createBabble(const MaskRenderPlan& plan, std::int64_t at) {
    if (!cfg_.corpus || !cfg_.audio || !isSupportedBabbleRate(fs_)) {
        babbleUnavailable_ = true;
        return false;
    }
    BabbleEngineConfig bc;
    bc.plan = plan.talkers;
    bc.plan.seed = cfg_.seed;
    const bool continuous = plan.talkers.mode == PlanMode::ContinuousN;
    if (continuous) {
        // Nested speaker sets (MASK_STRATEGIES §8.2): the selector pool is the whole healthy
        // corpus, so the "lab.speakerset" draw for N is a prefix of the draw for any larger N.
        std::uint32_t healthy = 0;
        for (SpeakerId s = 0; s < cfg_.corpus->numSpeakers(); ++s) healthy += cfg_.corpus->speakerHealthy(s) ? 1u : 0u;
        bc.plan.pool = std::max(bc.plan.pool, healthy);
    }
    bc.diversity = diversityFrom(plan.selector.diversity);
    bc.languageAware = plan.selector.languageAware;
    bc.rotationPeriodS = plan.selector.poolRotation ? 1200.0 : 0.0;
    bc.numChannels = static_cast<std::size_t>(nCh_);
    bc.busLevelDbfs = cfg_.lRefDbfs;
    bc.trimEnabled = false;  // the trim is applied per channel by this engine
    bc.probeSeconds = cfg_.babbleProbeSeconds;
    bc.externalFeed = cfg_.realtime;
    bc.blockPoolBlocks = cfg_.blockPoolBlocks;
    bc.fs = fs_;
    babble_ = std::make_unique<BabbleEngine>(cfg_.corpus, *cfg_.audio, bc);
    babbleStart_ = at;
    currentTalkerHash_ = bc.plan.hash();

    SpatialParams sp = plan.spatial.params;
    sp.algorithm = plan.spatial.algorithm;
    spatial_ = std::make_unique<SpatialPolicy>(layout_, sp, cfg_.seed, fs_, 0);
    slotPlaced_.assign(VoiceRenderer::kMaxSlots, 0);
    slotEnd_.assign(VoiceRenderer::kMaxSlots, 0);
    slotUsed_.assign(VoiceRenderer::kMaxSlots, 0);
    slotStart_.assign(VoiceRenderer::kMaxSlots, 0);
    eventCursor_ = 0;

    babbleConv_.clear();
    for (int c = 0; c < nCh_; ++c) {
        babbleConv_.push_back(std::make_unique<PartitionedConvolver>());
        babbleConv_.back()->prepare(fs_, kBabbleMaxTaps);
    }

    // Pool LTASS: the speakers the plan actually uses.
    if (cfg_.poolLtassDb) {
        poolLtassDb_ = *cfg_.poolLtassDb;
    } else {
        std::vector<SpeakerId> spk;
        if (continuous) {
            for (const auto& pe : babble_->planner().events())
                if (std::find(spk.begin(), spk.end(), pe.ev.speaker) == spk.end()) spk.push_back(pe.ev.speaker);
        } else {
            spk = babble_->selector().pool();
        }
        poolLtassDb_ = estimatePoolLtassDb(*cfg_.corpus, *cfg_.audio, spk, &poolReadFailure_);
    }

    if (!cfg_.realtime) placeNewTalkers(at);  // real-time: the planner thread places events
    feedForwardBalance();
    for (std::size_t c = 0; c < gainCur_.size(); ++c) gainCur_[c] = gainTarget_[c] = gainStamped_[c];
    gainStampPending_ = false;
    return true;
}

void MaskEngine::designStationary(const MaskRenderPlan& plan, std::int64_t at) {
    const FirDesignParams p = firParams(fs_, plan.target, false);
    const FirDesignResult r = designMinPhaseFir(plan.target.effectiveThirdOctDb(), p);
    stationary_.setFilter(r.tapsFloat(), at);
    referenceDb_ = idealBandLevelsDb(plan.target.effectiveThirdOctDb(), p);
    haveReference_ = true;
}

void MaskEngine::designBabbleKernel(std::int64_t at) {
    if (!babble_ || !current_) return;
    const ThirdOctArray T = current_->target.effectiveThirdOctDb();
    // Centre the pool LTASS on the target over the operating range so that the +-12 dB EQ
    // clamp acts on the shape difference only.
    double mu = 0.0;
    for (std::size_t k = kFirstOperatingBand; k <= kLastOperatingBand; ++k) mu += T[k] - poolLtassDb_[k];
    mu /= static_cast<double>(kNumOperatingBands);
    ThirdOctArray pool{};
    for (std::size_t k = 0; k < kNumThirdOctBands; ++k) pool[k] = poolLtassDb_[k] + mu;
    bool clamped = false;
    const ThirdOctArray eq = babbleEqBandsDb(T, pool, extendCorrection(correction_.correction()), &clamped);
    eqClamped_ = eqClamped_ || clamped;
    // eq is a per-band GAIN (mean |H|^2 in the band). designMinPhaseFir takes the band levels of the
    // filter's response to white noise, i.e. gain + 10 log10(bandwidth): add the bandwidth term.
    ThirdOctArray design{};
    const double bw1k = thirdOctUpperEdgeHz(kBand1k) - thirdOctLowerEdgeHz(kBand1k);
    for (std::size_t k = 0; k < kNumThirdOctBands; ++k)
        design[k] = eq[k] + 10.0 * std::log10((thirdOctUpperEdgeHz(k) - thirdOctLowerEdgeHz(k)) / bw1k);
    FirDesignResult r = designMinPhaseFir(design, firParams(fs_, current_->target, true));
    std::vector<double> h = std::move(r.h);
    normalizeForPoolLtass(h, fs_, std::vector<double>(pool.begin(), pool.end()));
    std::vector<float> hf(h.size());
    for (std::size_t i = 0; i < h.size(); ++i) hf[i] = static_cast<float>(h[i]);
    for (auto& conv : babbleConv_) {
        auto k = PartitionedKernel::create(hf.data(), hf.size());
        k->effectiveSample = at;
        conv->postKernel(std::move(k));
    }
    ++babbleDesigns_;
}

void MaskEngine::applyPlan(const MaskRenderPlan& in, std::int64_t at) {
    const bool first = !current_;
    std::optional<MaskRenderPlan> prev = current_;
    MaskRenderPlan p = in;
    p.planId = ++planCounter_;
    current_ = p;

    const bool targetChanged = first || !sameTarget(prev->target, p.target);
    if (targetChanged) designStationary(p, at);

    TalkerPlanParams tp = p.talkers;
    tp.seed = cfg_.seed;
    const std::uint64_t talkerHash = p.babbleEnabled ? tp.hash() : 0;
    bool created = false, talkersChanged = false;
    if (p.babbleEnabled) {
        if (!babble_) {
            created = createBabble(p, at);
        } else if (talkerHash != currentTalkerHash_) {
            if (p.talkers.mode == PlanMode::ContinuousN) tp.pool = std::max(tp.pool, babble_->planner().params().pool);
            babble_->scheduleReplan(tp, at - babbleStart_);
            talkersChanged = true;
        }
        if (babble_ && !created && prev && !sameSpatial(prev->spatial, p.spatial)) {
            SpatialParams sp = p.spatial.params;
            sp.algorithm = p.spatial.algorithm;
            spatial_ = std::make_unique<SpatialPolicy>(layout_, sp, cfg_.seed, fs_, planCounter_);
            std::fill(slotPlaced_.begin(), slotPlaced_.end(), std::uint8_t{0});
            std::fill(slotUsed_.begin(), slotUsed_.end(), std::uint8_t{0});
        }
    }
    const SpeakerVariation var = p.spatial.params.variation;
    if (nCh_ > 1 && var != variation_) {
        variation_ = var;
        decorrelator_.prepare(fs_, nCh_, variation_, cfg_.seed, planCounter_);
    }
    if (!first && babble_ && (targetChanged || talkersChanged)) {
        if (targetChanged) {
            correction_.reset();
            if (cfg_.initialCorrection) correction_.setInitial(*cfg_.initialCorrection);
        }
        correction_.notifyPlanChange();
    }
    if (babble_ && (created || targetChanged)) designBabbleKernel(at);

    // Mix (per zone b = strategy b + zone offset).
    const double b = p.mix.babbleFraction;
    mixer_.setBabbleFraction(b);
    for (int z = 0; z < HybridMixer::kMaxZones; ++z) {
        zoneOffset_[static_cast<std::size_t>(z)] = p.mix.zoneBabbleFraction[static_cast<std::size_t>(z)] - b;
        mixer_.setZoneOffset(z, zoneOffset_[static_cast<std::size_t>(z)]);
    }
    if (first) mixer_.jumpToTarget();
    configuredB_ = b;

    masterTarget_ = dbToLin(p.level.strengthDb);
    if (first) masterCur_ = masterTarget_;
    limiterEnabled_ = cfg_.limiterOverride.value_or(p.level.limiterEnabled);
    limiter_.setBypass(!limiterEnabled_);
    limiter_.setCeilingDb(p.level.limiterCeilingDbtp);

    if (talkersChanged) {
        trimMemory_[currentTalkerHash_] = trimDb_;
        const auto it = trimMemory_.find(talkerHash);
        trimDb_ = it != trimMemory_.end() ? it->second : 0.0;
        currentTalkerHash_ = talkerHash;
        for (int c = 0; c < nCh_; ++c) gainStamped_[static_cast<std::size_t>(c)] = dbToLin(trimDb_) * balance_.gain(c);
        gainStampPending_ = true;
    }
    if (!first) {
        trimFrozenUntil_ = std::max(trimFrozenUntil_, at + static_cast<std::int64_t>(kTrimFreezeS * fs_));
        crossfadeUntil_ = at + static_cast<std::int64_t>(kCrossfadeS * fs_);
    }

    nlohmann::json h;
    h["planId"] = p.planId;
    h["sample"] = at;
    h["seconds"] = static_cast<double>(at) / fs_;
    h["strategy"] = std::string(toString(p.strategyId));
    h["strategyClass"] = std::string(toString(p.strategyClass));
    h["area"] = p.areaId;
    h["planHash"] = p.planHash;
    h["babbleEnabled"] = p.babbleEnabled;
    h["babbleFraction"] = b;
    h["strengthDb"] = p.level.strengthDb;
    h["meanActive"] = p.babbleEnabled ? p.talkers.targetMean() : 0.0;
    h["target"] = p.target.id;
    if (p.babbleEnabled && !babble_) h["babbleUnavailable"] = true;
    history_.push_back(std::move(h));
}

void MaskEngine::placeNewTalkers(std::int64_t cellStart) {
    if (!babble_ || !spatial_) return;
    const auto& ev = babble_->planner().events();
    const std::int64_t bStart = cellStart - babbleStart_;
    // Release slots whose event has ended (their load no longer counts for balancing).
    for (std::size_t s = 0; s < slotPlaced_.size(); ++s) {
        if (slotPlaced_[s] && slotEnd_[s] <= bStart) {
            spatial_->releaseTalker(static_cast<int>(s));
            slotPlaced_[s] = 0;
        }
    }
    if (eventCursor_ > ev.size()) eventCursor_ = ev.size();
    const std::int64_t bEnd = bStart + kCell;
    while (eventCursor_ < ev.size() && ev[eventCursor_].ev.startSample < bEnd) {
        const TalkerEvent& e = ev[eventCursor_].ev;
        ++eventCursor_;
        if (e.endSample <= bStart || e.slot >= slotPlaced_.size()) continue;
        const GainVector g = spatial_->placeTalker(static_cast<int>(e.slot),
                                                   static_cast<double>(e.length()) / fs_, -1);
        babble_->setSlotGains(e.slot, g.data(), g.size());
        slotPlaced_[e.slot] = 1;
        slotUsed_[e.slot] = 1;
        slotEnd_[e.slot] = e.endSample;
    }
}

void MaskEngine::updateMotion(std::int64_t dt) {
    if (!babble_ || !spatial_) return;
    for (std::size_t s = 0; s < slotPlaced_.size(); ++s) {
        if (!slotPlaced_[s]) continue;
        const GainVector g = spatial_->updateMotion(static_cast<int>(s), dt);
        babble_->setSlotGains(static_cast<std::uint32_t>(s), g.data(), g.size());
    }
}

void MaskEngine::feedForwardBalance() {
    if (!babble_ || !spatial_) return;
    const auto N = static_cast<std::size_t>(nCh_);
    if (cfg_.channelBalance && N > 1) {
        // E_c relative: share of the babble bus power landing in channel c (sum over c = 1),
        // from the gain vectors of the planned talkers: slots carrying a talker now (a_v = 1)
        // and slots between talkers at their last position (a_v = 0.5). E_c is floored at
        // 0.25/N [I] (feed-forward boost <= +6 dB over an even spread): with few talkers a channel
        // can momentarily receive none, and 1/sqrt(E_c) must not chase that.
        std::vector<double> e(N, 0.0);
        double wsum = 0.0;
        for (std::size_t s = 0; s < slotPlaced_.size(); ++s) {
            if (!slotUsed_[s]) continue;
            const double a = slotPlaced_[s] ? 1.0 : 0.5;
            const GainVector& g = spatial_->gains(static_cast<int>(s));
            for (std::size_t c = 0; c < N && c < g.size(); ++c) e[c] += a * static_cast<double>(g[c]) * g[c];
            wsum += a;
        }
        const double floorE = 0.25 / static_cast<double>(N);
        for (double& v : e) v = wsum > 0.0 ? std::max(v / wsum, floorE) : 1.0 / static_cast<double>(N);
        balance_.updateFeedForward(e, 1.0);
    }
    for (std::size_t c = 0; c < N; ++c) gainStamped_[c] = dbToLin(trimDb_) * balance_.gain(static_cast<int>(c));
    gainStampPending_ = true;
}

void MaskEngine::endOfBlock(std::int64_t now) {
    const auto N = static_cast<std::size_t>(nCh_);
    const double n = static_cast<double>(std::max<std::int64_t>(blockN_, 1));
    std::vector<double> p(N);
    for (std::size_t c = 0; c < N; ++c) p[c] = blockPowT1_[c] / n;
    recentT1_.push_back(p);
    if (recentT1_.size() > 2) recentT1_.erase(recentT1_.begin());
    std::fill(blockPowT1_.begin(), blockPowT1_.end(), 0.0);
    blockN_ = 0;

    if (babble_) {
        std::vector<double> recent(N, 0.0);
        double pm = 0.0;
        for (const auto& r : recentT1_)
            for (std::size_t c = 0; c < N; ++c) recent[c] += r[c] / static_cast<double>(recentT1_.size());
        for (double v : recent) pm += v / static_cast<double>(N);
        if (spatial_) spatial_->setRecentEnergy(recent);
        const bool frozen = now < trimFrozenUntil_ || crossfading(now);
        const bool low = pm < std::pow(10.0, (cfg_.lRefDbfs - 40.0) / 10.0);
        if (cfg_.babbleTrim && !frozen && !low && recentT1_.size() == 2) {
            const double dt = static_cast<double>(blockLen_) / fs_;
            double step = (cfg_.lRefDbfs - powDb(pm)) * dt / kTrimTauS;
            step = std::clamp(step, -kTrimSlewDbPerS * dt, kTrimSlewDbPerS * dt);
            trimDb_ = std::clamp(trimDb_ + step, -kTrimClampDb, kTrimClampDb);
        }
        if (cfg_.channelBalance && N > 1) {
            balance_.setFrozen(frozen || low);
            balance_.updateFeedback(p, static_cast<double>(blockLen_) / fs_);
        }
        for (std::size_t c = 0; c < N; ++c) gainStamped_[c] = dbToLin(trimDb_) * balance_.gain(static_cast<int>(c));
        gainStampPending_ = true;
    }
    if (!cfg_.realtime) {  // real-time: rtCollectGarbage()
        stationary_.collectGarbage();
        for (auto& conv : babbleConv_) conv->collectGarbage();
    }
}

void MaskEngine::correctionStep(const SpectrumBlock& blk, std::int64_t now) {
    if (!babble_ || !current_ || !haveReference_ || !cfg_.correctionEnabled || cfg_.freezeCorrection) return;
    if (!babbleAnalyzer_.hasLongTerm()) return;
    double pm = 0.0;
    for (double v : blk.powerLin) pm += v;
    pm /= static_cast<double>(nCh_);
    CorrectionFreeze fr;
    fr.lowLevel = pm < std::pow(10.0, (cfg_.lRefDbfs - 20.0) / 10.0);
    fr.lowBabbleFraction = current_->mix.babbleFraction < 0.05;
    fr.crossfading = crossfading(now);
    const CorrectionStepResult r = correction_.step(operatingSlice(powerToDb(babbleAnalyzer_.longTermPower())),
                                                    operatingSlice(referenceDb_), fr);
    if (r.redesignNeeded) {
        // Real-time: stamped to the first grid boundary after RT position + kStampDelay.
        const std::int64_t at = cfg_.realtime ? ((rtPosition() + kStampDelay + kCell - 1) / kCell) * kCell : now;
        designBabbleKernel(at);
    }
}

void MaskEngine::applyStamped() {
    if (!gainStampPending_) return;
    gainTarget_ = gainStamped_;
    gainStampPending_ = false;
}

std::int64_t MaskEngine::nextSplit(std::int64_t t) const {
    std::int64_t s = (t / kCell + 1) * kCell;
    if (!pending_.empty() && pending_.front().at > t) s = std::min(s, pending_.front().at);
    s = std::min({s, nextFf_, nextBlock_, nextMotion_});
    return s;
}

void MaskEngine::process(float* const* out, int nFrames) {
    if (cfg_.realtime) {
        // RT: DSP only. Plans are applied on the control thread (rtApplyInitialPlan); planning,
        // preload and analysis run on the host's service threads.
        if (!current_) {
            for (int c = 0; c < nCh_; ++c) std::memset(out[c], 0, static_cast<std::size_t>(std::max(nFrames, 0)) * sizeof(float));
            return;
        }
        int off = 0;
        while (off < nFrames) {
            if (pos_ % kCell == 0) rtPollInbox();
            const int n = static_cast<int>(std::min<std::int64_t>(nFrames - off, (pos_ / kCell + 1) * kCell - pos_));
            processCell(out, off, n);
            off += n;
            pos_ += n;
        }
        RtOutputStats os;
        os.position = pos_;
        if (cfg_.limiterStage) os.limiter = limiter_.stats();
        os.grActiveSamples = grActiveSamples_;
        rtOut_.store(os);
        rtPos_.store(pos_, std::memory_order_release);
        return;
    }
    int off = 0;
    while (off < nFrames) {
        if (!pending_.empty() && (pending_.front().at <= pos_ || !current_)) {
            Pending p = std::move(pending_.front());
            pending_.erase(pending_.begin());
            applyPlan(p.plan, pos_);
            continue;
        }
        if (!current_) {  // not running: silence, the clock does not advance
            for (int c = 0; c < nCh_; ++c)
                std::memset(out[c] + off, 0, static_cast<std::size_t>(nFrames - off) * sizeof(float));
            return;
        }
        if (pos_ % kCell == 0) {
            applyStamped();
            placeNewTalkers(pos_);
        }
        const int n = static_cast<int>(std::min<std::int64_t>(nFrames - off, nextSplit(pos_) - pos_));
        processCell(out, off, n);
        off += n;
        pos_ += n;
        if (pos_ == nextFf_) {
            feedForwardBalance();
            nextFf_ += ffLen_;
        }
        if (pos_ == nextMotion_) {
            updateMotion(motionLen_);
            nextMotion_ += motionLen_;
        }
        if (pos_ == nextBlock_) {
            endOfBlock(pos_);
            nextBlock_ += blockLen_;
        }
        for (const SpectrumBlock& b : babbleAnalyzer_.takeBlocks()) correctionStep(b, pos_);
        (void)stationaryAnalyzer_.takeBlocks();
    }
}

void MaskEngine::processCell(float* const* out, int offset, int n) {
    const auto N = static_cast<std::size_t>(nCh_);
    const auto un = static_cast<std::size_t>(n);
    const std::int64_t t0 = pos_;

    // 1. Babble bus -> per-channel gain -> shaper -> decorrelator (T1).
    if (babble_) {
        babble_->render(pBab_.data(), nCh_, n);
        for (std::size_t c = 0; c < N; ++c) {
            float* x = pBab_[c];
            double g = gainCur_[c];
            const double tg = gainTarget_[c];
            for (std::size_t i = 0; i < un; ++i) {
                g += gainCoef_ * (tg - g);
                x[i] *= static_cast<float>(g);
            }
            gainCur_[c] = g;
            babbleConv_[c]->process(x, x, un);
        }
        decorrelator_.process(pBab_.data(), pBab_.data(), n);
    } else {
        for (std::size_t c = 0; c < N; ++c) std::memset(pBab_[c], 0, un * sizeof(float));
    }
    // 2. Stationary (T2).
    stationary_.process(pSta_.data(), un);

    const bool rtMode = cfg_.realtime;  // real-time: analysis runs on the analysis thread (taps)
    if (!rtMode) {
        for (std::size_t c = 0; c < N; ++c) {
            double e1 = 0.0, e2 = 0.0;
            for (std::size_t i = 0; i < un; ++i) {
                e1 += static_cast<double>(pBab_[c][i]) * pBab_[c][i];
                e2 += static_cast<double>(pSta_[c][i]) * pSta_[c][i];
            }
            blockPowT1_[c] += e1;
            sumT1_[c] += e1;
            sumT2_[c] += e2;
            babbleInMix_ += mixer_.zoneFraction(zoneOf_[c]) * e1;
        }
        blockN_ += n;
        if (babble_) babbleAnalyzer_.process(pBab_.data(), un);
        stationaryAnalyzer_.process(pSta_.data(), un);
    }
    if (tapSink_) {
        tapSink_->onTap(0, t0, pBab_.data(), nCh_, n);
        tapSink_->onTap(1, t0, pSta_.data(), nCh_, n);
    }

    // 3. Hybrid mixer (T3).
    mixer_.process(pBab_.data(), pSta_.data(), pMix_.data(), n);
    for (std::size_t c = 0; c < N && !rtMode; ++c) {
        double e = 0.0;
        for (std::size_t i = 0; i < un; ++i) e += static_cast<double>(pMix_[c][i]) * pMix_[c][i];
        sumT3_[c] += e;
    }
    for (std::size_t i = 0; i < un && !rtMode; ++i) {
        double e = 0.0;
        for (std::size_t c = 0; c < N; ++c) e += static_cast<double>(pMix_[c][i]) * pMix_[c][i];
        frameAcc_ += e / static_cast<double>(N);
        if (++framePos_ == frameLen_) {
            const double pw = frameAcc_ / static_cast<double>(frameLen_);
            modulation_.pushFrame(pw);
            const double db = powDb(pw);
            const auto bin = static_cast<std::size_t>(
                std::clamp((db - kHistMinDb) / kHistStepDb, 0.0, static_cast<double>(kHistBins - 1)));
            ++levelHist_[bin];
            frameAcc_ = 0.0;
            framePos_ = 0;
        }
    }
    if (tapSink_) tapSink_->onTap(2, t0, pMix_.data(), nCh_, n);

    // 4. Strategy crossfader: the plan switch is realised by the epoch hand-over, the 2 s
    //    constant-power b ramp and the 100 ms equal-power kernel crossfades (identity here).
    // 5. Master gain (Strength, tau 200 ms) x static gain; source select = MASKER.
    for (std::size_t i = 0; i < un; ++i) {
        masterCur_ += masterCoef_ * (masterTarget_ - masterCur_);
        const float g = static_cast<float>(masterCur_ * staticGain_);
        for (std::size_t c = 0; c < N; ++c) pMix_[c][i] *= g;
    }
    // 5b. Source select: MASKER | CALIBRATION | MUTE (identity while MASKER, bit-identical).
    sourceStage_.process(pMix_.data(), n);
    // 6. Output matrix.
    matrix_.process(pMix_.data(), pDev_.data(), n);
    // 7. Limiter + safety clip.
    if (cfg_.limiterStage) {
        limiter_.process(pDev_.data(), pDev_.data(), n);
        if (limiter_.stats().grDbBlockMax > 1e-4f) grActiveSamples_ += static_cast<std::uint64_t>(n);
    }
    // 8. Out (T4).
    for (std::size_t c = 0; c < N; ++c) {
        if (!rtMode) {
            double e = 0.0;
            for (std::size_t i = 0; i < un; ++i) e += static_cast<double>(pDev_[c][i]) * pDev_[c][i];
            sumT4_[c] += e;
        }
        std::memcpy(out[c] + offset, pDev_[c], un * sizeof(float));
    }
    if (!rtMode) meters_.process(pDev_.data(), n);
    if (tapSink_) tapSink_->onTap(3, t0, pDev_.data(), nCh_, n);
}

MaskStatistics MaskEngine::statistics() const {
    std::unique_lock<std::mutex> lk(svcMutex_, std::defer_lock);
    if (cfg_.realtime) lk.lock();
    const std::int64_t pos = cfg_.realtime ? anaSamples_ : pos_;
    const RtOutputStats ros = cfg_.realtime ? rtOut_.load() : RtOutputStats{};
    MaskStatistics s;
    s.fs = fs_;
    s.numChannels = nCh_;
    s.samples = pos;
    s.seconds = static_cast<double>(pos) / fs_;
    s.latencySamples = latencySamples();
    s.planChanges = planCounter_ > 0 ? planCounter_ - 1 : 0;
    s.currentPlanId = planCounter_;
    const auto N = static_cast<std::size_t>(nCh_);
    const double n = static_cast<double>(std::max<std::int64_t>(pos, 1));
    auto allDb = [&](const std::vector<double>& v) {
        double e = 0.0;
        for (double x : v) e += x;
        return powDb(e / (n * static_cast<double>(std::max<std::size_t>(N, 1))));
    };
    for (std::size_t c = 0; c < N; ++c) {
        s.outputRmsChDb.push_back(powDb(sumT4_[c] / n));
        s.babbleRmsChDb.push_back(powDb(sumT1_[c] / n));
    }
    s.outputRmsDb = allDb(sumT4_);
    s.babbleRmsDb = allDb(sumT1_);
    s.stationaryRmsDb = allDb(sumT2_);
    s.mixRmsDb = allDb(sumT3_);

    const MeterSnapshot m = meters_.snapshot();
    s.lufsS = m.lufsS;
    s.lufsI = m.lufsI;
    s.leq60Db = m.leq60AllDb;
    for (double v : m.truePeakMaxDb) s.truePeakMaxDb = std::max(s.truePeakMaxDb, v);
    for (double v : m.samplePeakMaxDb) s.samplePeakMaxDb = std::max(s.samplePeakMaxDb, v);
    s.crestDb = s.outputRmsDb > -200.0 ? s.truePeakMaxDb - s.outputRmsDb : 0.0;

    s.configuredBabbleFraction = cfg_.realtime ? rtBabbleFraction() : configuredB_;
    const double t3 = std::accumulate(sumT3_.begin(), sumT3_.end(), 0.0);
    s.measuredBabbleFraction = t3 > 0.0 ? babbleInMix_ / t3 : 0.0;

    s.babbleActive = babble_ != nullptr;
    if (babble_) {
        const BabbleEngineStats& st = babble_->stats();
        s.meanActive = st.meanActive();
        s.meanSpeaking = st.meanSpeaking();
        s.minActive = st.frames ? static_cast<int>(st.minActive) : 0;
        s.maxActive = static_cast<int>(st.maxActive);
        s.occupancyFrames = st.frames;
        s.countNorm = babble_->countNorm();
        s.underflows = babble_->underflows();
        s.droppedEvents = babble_->droppedEvents();
    }
    s.sourceErrors = sourceFailure();
    s.babbleUnavailable = babbleUnavailable_;
    s.babbleTrimDb = trimDb_;
    for (int c = 0; c < nCh_; ++c) s.channelBalanceDb.push_back(20.0 * std::log10(std::max(balance_.gain(c), 1e-12)));

    std::vector<double> gaps;
    for (const auto& g : modulation_.allGaps()) gaps.push_back(g.seconds);
    s.gapCount = gaps.size();
    s.gapMedianS = percentileOf(gaps, 0.5);
    s.gapP95S = percentileOf(gaps, 0.95);
    s.gapMaxS = gaps.empty() ? 0.0 : *std::max_element(gaps.begin(), gaps.end());
    s.gapRatePerS = s.seconds > 0.0 ? static_cast<double>(gaps.size()) / s.seconds : 0.0;
    s.envelopeL10L90Db = percentileFromHist(levelHist_, 0.9) - percentileFromHist(levelHist_, 0.1);
    const ModulationSnapshot ms = modulation_.snapshot();
    s.crest60sDb = ms.crest60sDb;
    s.modulationDepth10s = ms.modulationDepth10s;
    s.haveModulation = ms.haveModSpectrum;
    s.modulation = ms.modBroadband;

    if (babble_ && babbleAnalyzer_.hasLongTerm() && haveReference_) {
        s.haveBabbleSpectrum = true;
        shapeDevs(babbleAnalyzer_.longTermPower(), referenceDb_, s.babbleThirdOctMaxDevDb, s.babbleOctaveMaxDevDb);
    }
    if (stationaryAnalyzer_.numHops() > 0 && haveReference_) {
        s.haveStationarySpectrum = true;
        shapeDevs(stationaryAnalyzer_.overallPower(), referenceDb_, s.stationaryThirdOctMaxDevDb,
                  s.stationaryOctaveMaxDevDb);
    }
    s.correctionDb = correction_.correction();
    s.babbleKernelDesigns = babbleDesigns_;
    s.eqClamped = eqClamped_;

    s.limiterEnabled = cfg_.limiterStage && limiterEnabled_;
    if (cfg_.limiterStage) {
        const LimiterStats& ls = cfg_.realtime ? ros.limiter : limiter_.stats();
        s.limiterGrMaxDb = ls.grDbMax;
        s.clipEvents = ls.clipEvents;
        const double ns = static_cast<double>(std::max<std::uint64_t>(ls.samples, 1));
        s.limiterAbove05Fraction = static_cast<double>(ls.samplesAbove05Db) / ns;
        s.limiterActiveFraction = static_cast<double>(cfg_.realtime ? ros.grActiveSamples : grActiveSamples_) / ns;
    }
    return s;
}

nlohmann::json MaskEngine::planHistoryJson() const {
    std::unique_lock<std::mutex> lk(svcMutex_, std::defer_lock);
    if (cfg_.realtime) lk.lock();
    return history_;
}

nlohmann::json MaskEngine::eventsJson(std::int64_t endSample) const {
    std::unique_lock<std::mutex> lk(svcMutex_, std::defer_lock);
    if (cfg_.realtime) lk.lock();
    nlohmann::json j;
    j["schema"] = "babbleforge.events/1";
    j["seed"] = cfg_.seed;
    j["sampleRate"] = fs_;
    j["corpusVersion"] = cfg_.corpus ? cfg_.corpus->corpusVersion() : std::string();
    j["planHistory"] = history_;
    nlohmann::json arr = nlohmann::json::array();
    if (babble_) {
        const nlohmann::json pj = babble_->planner().eventsJson();
        j["epoch"] = pj.value("epoch", 0);
        for (nlohmann::json e : pj.at("events")) {
            const std::int64_t start = e.at("start").get<std::int64_t>() + babbleStart_;
            if (start >= endSample) continue;
            e["start"] = start;
            e["fadeOutStart"] = e.at("fadeOutStart").get<std::int64_t>() + babbleStart_;
            e["end"] = e.at("end").get<std::int64_t>() + babbleStart_;
            arr.push_back(std::move(e));
        }
    }
    j["babbleStartSample"] = babbleStart_;
    j["events"] = std::move(arr);
    return j;
}

// ------------------------------------------------------------------------------ real-time mode

bool MaskEngine::rtApplyInitialPlan(std::string* error) {
    if (!cfg_.realtime || !prepared_ || current_ || pending_.empty()) {
        if (error) *error = "rtApplyInitialPlan: needs a prepared real-time engine with one pending plan";
        return false;
    }
    Pending p = std::move(pending_.front());
    pending_.erase(pending_.begin());
    applyPlan(p.plan, pos_);
    appliedStrengthDb_ = static_cast<float>(p.plan.level.strengthDb);
    appliedB_ = static_cast<float>(p.plan.mix.babbleFraction);
    appliedCeilingDb_ = static_cast<float>(p.plan.level.limiterCeilingDbtp);
    rtStrengthDb_.store(appliedStrengthDb_, std::memory_order_relaxed);
    rtBabbleB_.store(appliedB_, std::memory_order_relaxed);
    rtCeilingDb_.store(appliedCeilingDb_, std::memory_order_relaxed);
    gainTarget_ = gainCur_;
    gainStampPending_ = false;
    return true;
}

void MaskEngine::rtPollInbox() noexcept {
    const float sDb = rtStrengthDb_.load(std::memory_order_relaxed);
    if (sDb != appliedStrengthDb_) {
        appliedStrengthDb_ = sDb;
        masterTarget_ = std::pow(10.0, static_cast<double>(sDb) / 20.0);
    }
    const float b = rtBabbleB_.load(std::memory_order_relaxed);
    if (b != appliedB_) {
        appliedB_ = b;
        mixer_.setBabbleFraction(static_cast<double>(b));
    }
    const float ceil = rtCeilingDb_.load(std::memory_order_relaxed);
    if (ceil != appliedCeilingDb_) {
        appliedCeilingDb_ = ceil;
        limiter_.setCeilingDb(static_cast<double>(ceil));
    }
    for (;;) {
        if (!havePendingStamp_) {
            if (!inbox_->pop(pendingStamp_)) break;
            havePendingStamp_ = true;
        }
        if (pendingStamp_.effective > pos_) break;
        const std::size_t n = std::min<std::size_t>(pendingStamp_.numChannels, gainTarget_.size());
        for (std::size_t c = 0; c < n; ++c) gainTarget_[c] = static_cast<double>(pendingStamp_.gains[c]);
        havePendingStamp_ = false;
    }
}

double MaskEngine::zoneTargetFraction(int c) const noexcept {
    const double b = rtBabbleFraction();
    const auto z = static_cast<std::size_t>(zoneOf_[static_cast<std::size_t>(c)]);
    return std::clamp(b + zoneOffset_[z], 0.0, 1.0);
}

bool MaskEngine::rtPlaceEvent(const TalkerEvent& e, float* gains, std::size_t n) {
    if (!babble_ || !spatial_ || e.slot >= slotPlaced_.size()) return false;
    for (std::size_t s = 0; s < slotPlaced_.size(); ++s) {
        if (slotPlaced_[s] && slotEnd_[s] <= e.startSample) {
            spatial_->releaseTalker(static_cast<int>(s));
            slotPlaced_[s] = 0;
        }
    }
    const GainVector g = spatial_->placeTalker(static_cast<int>(e.slot), static_cast<double>(e.length()) / fs_, -1);
    slotPlaced_[e.slot] = 1;
    slotUsed_[e.slot] = 1;
    slotEnd_[e.slot] = e.endSample;
    slotStart_[e.slot] = e.startSample;
    for (std::size_t c = 0; c < n; ++c) gains[c] = c < g.size() ? g[c] : 0.0f;
    return true;
}

void MaskEngine::rtUpdateMotion(std::int64_t babbleNow) {
    if (!babble_ || !spatial_) return;
    for (std::size_t s = 0; s < slotPlaced_.size(); ++s) {
        if (!slotPlaced_[s]) continue;
        if (slotEnd_[s] <= babbleNow) {
            spatial_->releaseTalker(static_cast<int>(s));
            slotPlaced_[s] = 0;
            continue;
        }
        if (slotStart_[s] > babbleNow) continue;  // placed for an event that has not started yet
        const GainVector g = spatial_->updateMotion(static_cast<int>(s), motionLen_);
        babble_->setSlotGains(static_cast<std::uint32_t>(s), g.data(), g.size());
    }
}

void MaskEngine::rtFeedForwardBalance() { feedForwardBalance(); }

void MaskEngine::rtFlushGainStamp() {
    if (!gainStampPending_ || !inbox_) return;
    RtGainStamp st;
    st.effective = ((rtPosition() + kStampDelay + kCell - 1) / kCell) * kCell;
    st.numChannels = static_cast<std::uint32_t>(std::min<std::size_t>(gainStamped_.size(), st.gains.size()));
    for (std::size_t c = 0; c < st.numChannels; ++c) st.gains[c] = static_cast<float>(gainStamped_[c]);
    if (inbox_->push(st))
        gainStampPending_ = false;
    else
        ++stampsDropped_;
}

void MaskEngine::rtCollectGarbage() {
    stationary_.collectGarbage();
    for (auto& conv : babbleConv_) conv->collectGarbage();
}

void MaskEngine::rtAnalyzeTap(int tap, std::int64_t start, const float* const* ch, int nCh, int n) {
    if (!cfg_.realtime || n <= 0) return;
    const auto N = static_cast<std::size_t>(std::min(nCh, nCh_));
    auto sq = [](const float* x, std::size_t m) {
        double e = 0.0;
        for (std::size_t i = 0; i < m; ++i) e += static_cast<double>(x[i]) * x[i];
        return e;
    };
    switch (tap) {
    case 0: {  // T1: babble -> 5 s trim / balance blocks, babble spectrum, spectral correction
        int off = 0;
        while (off < n) {
            const std::int64_t t = start + off;
            int m = n - off;
            if (nextBlock_ > t) m = static_cast<int>(std::min<std::int64_t>(m, nextBlock_ - t));
            const auto um = static_cast<std::size_t>(m);
            for (std::size_t c = 0; c < N; ++c) {
                const double e1 = sq(ch[c] + off, um);
                blockPowT1_[c] += e1;
                sumT1_[c] += e1;
                babbleInMix_ += zoneTargetFraction(static_cast<int>(c)) * e1;
                anaPtr_[c] = ch[c] + off;
            }
            blockN_ += m;
            if (babble_) babbleAnalyzer_.process(anaPtr_.data(), um);
            off += m;
            const std::int64_t now = t + m;
            while (now >= nextBlock_) {
                endOfBlock(now);
                nextBlock_ += blockLen_;
            }
            for (const SpectrumBlock& b : babbleAnalyzer_.takeBlocks()) correctionStep(b, now);
        }
        if (babble_) babble_->consumeOccupancy();
        rtFlushGainStamp();
        break;
    }
    case 1: {  // T2: stationary spectrum
        for (std::size_t c = 0; c < N; ++c) sumT2_[c] += sq(ch[c], static_cast<std::size_t>(n));
        stationaryAnalyzer_.process(ch, static_cast<std::size_t>(n));
        (void)stationaryAnalyzer_.takeBlocks();
        break;
    }
    case 2: {  // T3: mix level, 10 ms envelope histogram, modulation / gaps
        for (std::size_t c = 0; c < N; ++c) sumT3_[c] += sq(ch[c], static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            double e = 0.0;
            for (std::size_t c = 0; c < N; ++c) e += static_cast<double>(ch[c][i]) * ch[c][i];
            frameAcc_ += e / static_cast<double>(std::max<std::size_t>(N, 1));
            if (++framePos_ == frameLen_) {
                const double pw = frameAcc_ / static_cast<double>(frameLen_);
                modulation_.pushFrame(pw);
                const double db = powDb(pw);
                const auto bin = static_cast<std::size_t>(
                    std::clamp((db - kHistMinDb) / kHistStepDb, 0.0, static_cast<double>(kHistBins - 1)));
                ++levelHist_[bin];
                frameAcc_ = 0.0;
                framePos_ = 0;
            }
        }
        break;
    }
    case 3: {  // T4: output level and meters
        for (std::size_t c = 0; c < N; ++c) sumT4_[c] += sq(ch[c], static_cast<std::size_t>(n));
        meters_.process(ch, n);
        anaSamples_ += n;
        break;
    }
    default: break;
    }
}

nlohmann::json toJson(const MaskStatistics& s) {
    nlohmann::json j;
    j["sampleRate"] = s.fs;
    j["channels"] = s.numChannels;
    j["samples"] = s.samples;
    j["seconds"] = s.seconds;
    j["latencySamples"] = s.latencySamples;
    j["planChanges"] = s.planChanges;
    j["level"] = {{"outputRmsDb", s.outputRmsDb},       {"outputRmsChDb", s.outputRmsChDb},
                  {"babbleRmsDb", s.babbleRmsDb},       {"babbleRmsChDb", s.babbleRmsChDb},
                  {"stationaryRmsDb", s.stationaryRmsDb}, {"mixRmsDb", s.mixRmsDb},
                  {"lufsS", s.lufsS},                   {"lufsI", s.lufsI},
                  {"leq60Db", s.leq60Db},               {"truePeakMaxDbtp", s.truePeakMaxDb},
                  {"samplePeakMaxDb", s.samplePeakMaxDb}, {"crestDb", s.crestDb}};
    j["mix"] = {{"configuredBabbleFraction", s.configuredBabbleFraction},
                {"measuredBabbleFraction", s.measuredBabbleFraction}};
    j["talkers"] = {{"babbleActive", s.babbleActive}, {"meanActive", s.meanActive},
                    {"meanSpeaking", s.meanSpeaking}, {"minActive", s.minActive},
                    {"maxActive", s.maxActive},       {"countNorm", s.countNorm},
                    {"babbleTrimDb", s.babbleTrimDb}, {"channelBalanceDb", s.channelBalanceDb}};
    j["temporal"] = {{"gapCount", s.gapCount},
                     {"gapMedianS", s.gapMedianS},
                     {"gapP95S", s.gapP95S},
                     {"gapMaxS", s.gapMaxS},
                     {"gapRatePerS", s.gapRatePerS},
                     {"envelopeL10L90Db", s.envelopeL10L90Db},
                     {"crest60sDb", s.crest60sDb},
                     {"modulationDepth10s", s.modulationDepth10s}};
    if (s.haveModulation) {
        nlohmann::json m = nlohmann::json::object();
        const auto& fc = modulationBandCentresHz();
        for (std::size_t k = 0; k < kNumModBands; ++k) m[std::to_string(fc[k])] = s.modulation[k];
        j["temporal"]["modulationSpectrum"] = m;
    }
    nlohmann::json sp;
    if (s.haveBabbleSpectrum)
        sp["babble"] = {{"thirdOctMaxDevDb", s.babbleThirdOctMaxDevDb}, {"octaveMaxDevDb", s.babbleOctaveMaxDevDb}};
    if (s.haveStationarySpectrum)
        sp["stationary"] = {{"thirdOctMaxDevDb", s.stationaryThirdOctMaxDevDb},
                            {"octaveMaxDevDb", s.stationaryOctaveMaxDevDb}};
    sp["correctionDb"] = std::vector<double>(s.correctionDb.begin(), s.correctionDb.end());
    sp["babbleKernelDesigns"] = s.babbleKernelDesigns;
    sp["eqClamped"] = s.eqClamped;
    j["spectrum"] = sp;
    j["limiter"] = {{"enabled", s.limiterEnabled},
                    {"grMaxDb", s.limiterGrMaxDb},
                    {"activeFraction", s.limiterActiveFraction},
                    {"above05DbFraction", s.limiterAbove05Fraction},
                    {"clipEvents", s.clipEvents}};
    j["reliability"] = {{"underflows", s.underflows},
                        {"droppedEvents", s.droppedEvents},
                        {"sourceErrors", s.sourceErrors},
                        {"babbleUnavailable", s.babbleUnavailable}};
    return j;
}

}  // namespace bf
