#include "core/talker/TalkerPlanner.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <nlohmann/json.hpp>

#include "core/math/DetMath.h"
#include "core/random/Distributions.h"

namespace bf {

namespace {
constexpr double kLn10 = 2.302585092994045684;
constexpr double kPi = 3.14159265358979323846;

// 48 kHz source-domain conversions (layout construction parameters).
std::int64_t msToSrc(double ms) { return static_cast<std::int64_t>(std::llround(ms * 48.0)); }
std::int64_t sToSrc(double s) { return static_cast<std::int64_t>(std::llround(s * 48000.0)); }
double dbToLin(double db) { return detexp(db * kLn10 / 20.0); }

// Mean of a log-normal (median, sigmaLn) conditioned on [lo, hi] (Simpson, detmath only).
double truncLogNormalMean(double median, double sigma, double lo, double hi) {
    if (!(sigma > 0.0)) return std::clamp(median, lo, hi);
    const double a = detlog(lo / median) / sigma, b = detlog(hi / median) / sigma;
    if (!(b > a)) return std::clamp(median, lo, hi);
    constexpr int N = 2000;  // even
    const double h = (b - a) / N;
    double num = 0.0, den = 0.0;
    for (int i = 0; i <= N; ++i) {
        const double x = a + h * i;
        const double w = (i == 0 || i == N) ? 1.0 : ((i % 2) ? 4.0 : 2.0);
        const double phi = detexp(-0.5 * x * x);
        num += w * median * detexp(sigma * x) * phi;
        den += w * phi;
    }
    return num / den;
}

// Integral of the squared fade gain over [a, b) (event-relative samples).
double fadeSqIntegral(double a, double b, double fi, double fos, double len) {
    double s = 0.0;
    auto seg = [&](double lo, double hi) {
        lo = std::max(lo, a);
        hi = std::min(hi, b);
        return std::pair<double, double>{lo, hi};
    };
    if (fi > 0.0) {
        auto [lo, hi] = seg(0.0, fi);
        if (hi > lo) s += 0.5 * (hi - lo) - fi / (2.0 * kPi) * (std::sin(kPi * hi / fi) - std::sin(kPi * lo / fi));
    }
    {
        auto [lo, hi] = seg(fi, fos);
        if (hi > lo) s += hi - lo;
    }
    const double fo = len - fos;
    if (fo > 0.0) {
        auto [lo, hi] = seg(fos, len);
        if (hi > lo) {
            const double y0 = lo - fos, y1 = hi - fos;
            s += 0.5 * (y1 - y0) + fo / (2.0 * kPi) * (std::sin(kPi * y1 / fo) - std::sin(kPi * y0 / fo));
        }
    }
    return s;
}
}  // namespace

std::int64_t TalkerPlanner::msToSmp(double ms) const { return msToEngine(ms, fs_); }
std::int64_t TalkerPlanner::sToSmp(double s) const { return secondsToEngine(s, fs_); }

// floor(f * len) on the base-rate grid (len is a multiple of mult_): scale-exact across a family.
std::int64_t TalkerPlanner::fracOf(double f, std::int64_t len) const {
    return static_cast<std::int64_t>(f * static_cast<double>(len / mult_)) * mult_;
}

TalkerPlanner::TalkerPlanner(std::shared_ptr<const CorpusSnapshot> snap, SegmentSelector& selector,
                             const TalkerPlanParams& params, double fs)
    : fs_(static_cast<std::int64_t>(std::llround(fs > 0.0 ? fs : 48000.0))),
      snap_(std::move(snap)), sel_(selector), p_(params) {
    mult_ = rateFamily(fs_).mult;
    guard_ = msToSmp(40.0);
    freeze_ = sToSmp(0.5);
    keepEnded_ = 3 * fs_;
    jitter80_ = msToSmp(80.0);
    jitter300_ = msToSmp(300.0);
    lastStart_ = -guard_;
    nextTick_ = fs_;
    slots_.resize(p_.slots());
    initStreams();
    sel_.setVoiceSlots(p_.slots(), p_.targetMean());
    sel_.setPoolSize(p_.pool);
    sel_.setSoloRiskWeight(p_.cvrSoloRiskWeight);
    recomputeRates();
    initialPhase();
}

void TalkerPlanner::initStreams() {
    const std::uint64_t master = epoch_ == 0 ? p_.seed : (p_.seed ^ planHash_);
    for (std::size_t j = 0; j < slots_.size(); ++j) {
        slots_[j].rng = RngStream(master, "planner.slot." + std::to_string(j), epoch_);
        slots_[j].spatial = RngStream(master, "spatial.slot." + std::to_string(j), epoch_);
    }
    global_ = RngStream(master, "planner.global", epoch_);
    gainvar_ = RngStream(master, "gainvar", epoch_);
    lab_ = RngStream(master, "lab.speakerset", epoch_);
}

void TalkerPlanner::recomputeRates() {
    dOnS_ = truncLogNormalMean(p_.medianS, p_.sigmaLn, p_.segMinS, p_.segMaxS);
    const double V = static_cast<double>(p_.slots());
    const double m = std::max(p_.targetMean(), 1e-3);
    dOffS_ = m >= V ? 0.0 : dOnS_ * (V - m) / m;
}

std::int64_t TalkerPlanner::cooldownEnd(const Slot& s) const {
    return s.lastEnd == kNever ? kNever : s.lastEnd + msToSmp(p_.reEntryCooldownMs);
}

std::int64_t TalkerPlanner::overlapSamples() const {
    return p_.mode == PlanMode::FixedK ? msToSmp(150.0) : msToSmp(20.0);
}

std::int64_t TalkerPlanner::offDuration(Slot& s) {
    const double cd = p_.reEntryCooldownMs / 1000.0;
    const double meanEff = std::max(dOffS_ / lambda_ - cd, 0.05);
    return msToSmp(p_.reEntryCooldownMs) + sToSmp(exponential(s.rng, meanEff));
}

void TalkerPlanner::initialPhase() {
    const std::size_t V = slots_.size();
    std::vector<bool> on(V, true);
    std::vector<double> frac(V, 0.0);
    if (p_.mode == PlanMode::Stochastic) {
        const double pOn = std::min(1.0, p_.mean / static_cast<double>(V));
        std::uint32_t cnt = 0;
        for (std::size_t j = 0; j < V; ++j) {
            on[j] = slots_[j].rng.uniform01() < pOn;
            cnt += on[j] ? 1u : 0u;
        }
        const std::uint32_t minA = p_.effectiveMin();
        for (std::size_t j = 0; j < V && cnt < minA; ++j)
            if (!on[j]) { on[j] = true; ++cnt; }
    } else if (p_.mode == PlanMode::ContinuousN) {
        const auto set = sel_.chooseSpeakerSet(static_cast<std::uint32_t>(V), lab_);
        for (std::size_t j = 0; j < V && j < set.size(); ++j) {
            slots_[j].hasSpeaker = true;
            slots_[j].speaker = set[j];
        }
    }
    for (std::size_t j = 0; j < V; ++j) {
        const double u = slots_[j].rng.uniform01();
        if (on[j]) {
            if (!construct(static_cast<std::uint32_t>(j), 0, kEvResidual, u)) {
                slots_[j].nextStart = 0;
                slots_[j].chainFresh = true;
            }
        } else {
            slots_[j].nextStart = fracOf(u, offDuration(slots_[j]));
        }
    }
    std::stable_sort(events_.begin(), events_.end(), [](const PlannedEvent& a, const PlannedEvent& b) {
        return a.ev.startSample < b.ev.startSample;
    });
    nextId_ = 0;
    for (auto& e : events_) e.ev.eventId = nextId_++;
    lastStart_ = events_.empty() ? -guard_ : std::max(events_.back().ev.startSample, -guard_);
    live_.clear();
    for (std::size_t i = 0; i < events_.size(); ++i) live_.push_back(i);
}

void TalkerPlanner::addOccupancy(const TalkerEvent& ev, int sign) {
    std::int64_t a = std::max<std::int64_t>(0, ev.startSample);
    const std::int64_t b = ev.endSample;
    while (a < b) {
        const std::int64_t bin = a / fs_;
        const std::int64_t e = std::min(b, (bin + 1) * fs_);
        occ_[static_cast<std::size_t>(bin) % kRing] += sign * (e - a);
        a = e;
    }
}

void TalkerPlanner::tick() {
    const std::int64_t k = nextTick_ / fs_;
    const std::int64_t lo = std::max<std::int64_t>(0, k - 60);
    std::int64_t sum = 0;
    for (std::int64_t b = lo; b < k; ++b) sum += occ_[static_cast<std::size_t>(b) % kRing];
    const double meanActive = static_cast<double>(sum) / static_cast<double>((k - lo) * fs_);
    const double m = std::max(p_.mean, 1e-3);
    lambda_ = std::clamp(lambda_ + 0.02 * (m - meanActive) / m, 0.7, 1.3);
    if (k >= 61) occ_[static_cast<std::size_t>(k - 61) % kRing] = 0;
    nextTick_ += fs_;
}

void TalkerPlanner::pruneLive() {
    std::erase_if(live_, [&](std::size_t i) {
        PlannedEvent& pe = events_[i];
        // Keep recently ended events a little longer: the end-cluster guard looks back.
        if (pe.ev.endSample > lastStart_ - keepEnded_) return false;
        if (retention_ == LayoutRetention::None || (retention_ == LayoutRetention::UntilTaken && i < taken_))
            pe.layout.reset();
        return true;
    });
}

int TalkerPlanner::speakingOthersAt(std::int64_t t) const {
    int n = 0;
    for (std::size_t i : live_) {
        const auto& pe = events_[i];
        if (pe.ev.startSample <= t && t < pe.ev.endSample && pe.layout &&
            pe.layout->speechAt(t - pe.ev.startSample))
            ++n;
    }
    return n;
}

void TalkerPlanner::commit(PlannedEvent&& pe) {
    pe.ev.eventId = nextId_++;
    pe.ev.epoch = epoch_;
    Slot& s = slots_[pe.ev.slot];
    s.lastEnd = pe.ev.endSample;
    s.chainFresh = false;
    if (p_.mode == PlanMode::Stochastic)
        s.nextStart = pe.ev.endSample + offDuration(s);
    else
        s.nextStart = pe.ev.fadeOutStart;
    addOccupancy(pe.ev, +1);
    ++stats_.events;
    if (pe.ev.flags & kEvSnapped) ++stats_.snapped;
    if (pe.ev.flags & kEvPhraseCut) ++stats_.phraseCuts;
    if (pe.ev.flags & kEvCapped) ++stats_.capped;
    if (pe.ev.flags & kEvEndShifted) ++stats_.endShifts;
    if (!(pe.ev.flags & kEvResidual)) lastStart_ = std::max(lastStart_, pe.ev.startSample);
    live_.push_back(events_.size());
    events_.push_back(std::move(pe));
}

bool TalkerPlanner::construct(std::uint32_t j, std::int64_t t, std::uint16_t flags, double residualFrac) {
    Slot& s = slots_[j];
    const bool stochastic = p_.mode == PlanMode::Stochastic;
    const bool residual = residualFrac >= 0.0;

    std::vector<SpeakerId> act;
    for (std::size_t i : live_)
        if (events_[i].ev.endSample > t) act.push_back(events_[i].ev.speaker);
    if (p_.mode != PlanMode::ContinuousN) sel_.maybeRotatePool(t, act);

    std::optional<SegmentPick> pk;
    if (p_.mode == PlanMode::ContinuousN && s.hasSpeaker)
        pk = sel_.pickForSpeaker(s.speaker, t);
    else
        pk = sel_.pick(t, act);
    if (!pk) { ++stats_.pickFailures; return false; }

    const std::int64_t fixedOvl = overlapSamples();
    // Layout limits in the 48 kHz source domain (the layout reports engine samples).
    const std::int64_t maxLen = sToSrc(p_.segMaxS + 1.5) + msToSrc(p_.fadeOutMs * 1.3) + 8 * msToSrc(40.0);
    auto layout = std::make_shared<ProcessedLayout>(
        buildProcessedLayout(*snap_, pk->recording, pk->anchor, msToSrc(p_.maxGapMs), maxLen, fs_));
    const std::int64_t avail = layout->length;

    std::int64_t fi, fo;
    if (stochastic) {
        fi = msToSmp(p_.fadeInMs * (1.0 + 0.3 * (2.0 * s.rng.uniform01() - 1.0)));
        fo = msToSmp(p_.fadeOutMs * (1.0 + 0.3 * (2.0 * s.rng.uniform01() - 1.0)));
    } else {
        fi = fo = fixedOvl;
    }
    std::int64_t L = sToSmp(truncatedLogNormalMedian(s.rng, p_.medianS, p_.sigmaLn, p_.segMinS, p_.segMaxS));
    const std::int64_t minBody = msToSmp(250.0);
    if (avail < fi + fo + minBody) {
        sel_.commit(*pk, layout->sourcePosAt(avail), t, t + avail);
        ++stats_.pickFailures;
        return false;
    }
    L = std::min(L, avail);

    // End snapping (§4.4): pause boundary b (phrase-boundary pause start) nearest to L_target
    // within +-1 s; the fade-out starts at b.
    std::int64_t fos = L - fo;
    {
        std::int64_t bestD = sToSmp(1.0) + 1;
        std::int64_t best = -1;
        for (const auto& pz : layout->pauses) {
            if (!pz.phraseBoundary) continue;
            const std::int64_t b = pz.start;
            if (b < fi + minBody || b + fo > avail) continue;
            const std::int64_t d = b > L ? b - L : L - b;
            if (d < bestD) { bestD = d; best = b; }
        }
        if (best >= 0) { fos = best; flags |= kEvSnapped; }
    }
    fos = std::clamp(fos, fi, avail - fo);

    // CVR max phrase continuity (stochastic only): forced fade-out point when a continuous
    // phrase exceeds X s while fewer than 2 other (already planned) talkers speak.
    if (stochastic && !residual && p_.cvrMaxPhraseS > 0.0) {
        const std::int64_t X = sToSmp(p_.cvrMaxPhraseS);
        const auto& sp = layout->speech;
        std::int64_t runStart = -1;
        for (std::size_t k = 0; k < sp.size() && sp[k].start < fos; ++k) {
            bool boundary = runStart < 0;
            if (k > 0 && k - 1 < layout->pauses.size()) boundary = boundary || layout->pauses[k - 1].phraseBoundary;
            if (boundary) runStart = sp[k].start;
            const std::int64_t runEnd = std::min(sp[k].end, fos);
            if (runEnd - runStart > X) {
                const std::int64_t cut = runStart + X;
                if (cut >= fi + minBody && speakingOthersAt(t + cut) < 2) {
                    fos = cut;
                    flags |= kEvPhraseCut;
                    break;
                }
            }
        }
    }

    std::int64_t start = t;
    if (residual) {
        const std::int64_t len = fos + fo;
        std::int64_t elapsed = fracOf(1.0 - residualFrac, len);
        elapsed = std::clamp<std::int64_t>(elapsed, 0, std::max<std::int64_t>(0, len - sToSmp(0.5)));
        start = t - elapsed;
        for (int tries = 0; tries < 8; ++tries) {  // start anti-synchrony among residual events
            bool conflict = false;
            for (std::size_t i : live_)
                if (std::llabs(events_[i].ev.startSample - start) < guard_) conflict = true;
            if (!conflict) break;
            const std::int64_t sh = guard_ + fracOf(global_.uniform01(), jitter80_);
            if (t - (start - sh) <= len - sToSmp(0.5)) start -= sh; else start += sh;
        }
    }

    // End anti-synchrony guard: no two ends within 40 ms (shift the fade-out by +40..120 ms).
    auto endConflict = [&](std::int64_t endAbs) {
        for (std::size_t i : live_)
            if (events_[i].ev.endSample > t && std::llabs(events_[i].ev.endSample - endAbs) < guard_) return true;
        return false;
    };
    for (int tries = 0; tries < 8; ++tries) {
        if (!endConflict(start + fos + fo)) break;
        const std::int64_t sh = guard_ + fracOf(global_.uniform01(), jitter80_);
        if (fos + sh + fo <= avail) fos += sh;
        else if (fos - sh >= fi) fos -= sh;
        else break;
        flags |= kEvEndShifted;
    }
    // End-cluster guard (stochastic, min > 0): at most V - min ends within any window of
    // re-entry cooldown + 40 ms. Then, whenever k_a is about to fall below min, at least one
    // idle slot is out of its cooldown, so the forced start (§4.5) can always cover the drop.
    const std::int64_t spare = static_cast<std::int64_t>(slots_.size()) - static_cast<std::int64_t>(p_.effectiveMin());
    if (stochastic && p_.effectiveMin() > 0 && spare > 0) {
        const std::int64_t W = msToSmp(p_.reEntryCooldownMs) + guard_;
        std::vector<std::int64_t> nearEnds;
        auto clustered = [&](std::int64_t e) {
            nearEnds.clear();
            for (std::size_t i : live_) {
                const std::int64_t x = events_[i].ev.endSample;
                if (x > e - W && x < e + W) nearEnds.push_back(x);
            }
            nearEnds.push_back(e);
            std::sort(nearEnds.begin(), nearEnds.end());
            for (std::size_t a = 0; a < nearEnds.size() && nearEnds[a] <= e; ++a) {
                std::int64_t cnt = 0;
                for (std::size_t b = a; b < nearEnds.size() && nearEnds[b] < nearEnds[a] + W; ++b) ++cnt;
                if (cnt > spare) return true;
            }
            return false;
        };
        if (clustered(start + fos + fo)) {
            const std::int64_t stepS = std::max<std::int64_t>(guard_, (W / (4 * mult_)) * mult_);
            const std::int64_t base = fos;
            for (int k = 1; k <= 24; ++k) {
                const std::int64_t cand = k <= 12 ? base + k * stepS : base - (k - 12) * stepS;
                if (cand + fo > avail || cand < fi + minBody) continue;
                if (!clustered(start + cand + fo) && !endConflict(start + cand + fo)) {
                    fos = cand;
                    flags |= kEvEndShifted;
                    break;
                }
            }
        }
    }

    // Level: ASL normalisation x truncated-Gaussian variation, then the dominance cap.
    const double sigma = p_.gainSigmaDb * p_.cvrLevelSigmaMult;
    double varDb = sigma > 0.0 ? truncatedNormal(gainvar_, sigma, 2.0) : 0.0;
    if (p_.cvrDominanceCapDb > 0.0) {
        double sum = 0.0;
        int n = 0;
        for (std::size_t i : live_) {
            const auto& e = events_[i].ev;
            if (e.startSample <= t && e.endSample > t) {
                sum += detexp(static_cast<double>(e.levelVarDb) * kLn10 / 10.0);
                ++n;
            }
        }
        if (n > 0) {
            const double meanDb = 10.0 * detlog(sum / n) / kLn10;
            if (varDb > meanDb + p_.cvrDominanceCapDb) {
                varDb = meanDb + p_.cvrDominanceCapDb;
                flags |= kEvCapped;
            }
        }
    }
    const double asl = snap_->segment(pk->segment).aslDb;

    TalkerEvent ev;
    ev.slot = j;
    ev.startSample = start;
    ev.fadeInLen = fi;
    ev.fadeOutStart = start + fos;
    ev.endSample = start + fos + fo;
    ev.speaker = pk->speaker;
    ev.recording = pk->recording;
    ev.segment = pk->segment;
    ev.anchor = pk->anchor;
    ev.levelVarDb = static_cast<float>(varDb);
    ev.segGainLin = static_cast<float>(dbToLin(p_.talkerRefDbfs - asl + varDb));
    ev.pan = static_cast<float>(2.0 * s.spatial.uniform01() - 1.0);
    ev.flags = flags;
    sel_.commit(*pk, layout->sourcePosAt(fos + fo), start, ev.endSample);
    commit(PlannedEvent{ev, std::move(layout)});
    return true;
}

void TalkerPlanner::planUntil(std::int64_t T) {
    if (p_.mode == PlanMode::Stochastic) planStochastic(T);
    else planContinuous(T);
}

void TalkerPlanner::planStochastic(std::int64_t T) {
    constexpr std::int64_t kInf = std::numeric_limits<std::int64_t>::max();
    std::vector<std::int64_t> ends;
    while (true) {
        pruneLive();
        std::size_t js = 0;
        std::int64_t ts = kInf;
        for (std::size_t j = 0; j < slots_.size(); ++j)
            if (slots_[j].nextStart < ts) { ts = slots_[j].nextStart; js = j; }

        // Forced start (§4.5): the earliest time at which k_a would fall below min.
        std::int64_t tf = kInf, drop = kInf;
        std::size_t jf = 0;
        const std::uint32_t minA = p_.effectiveMin();
        if (minA > 0) {
            ends.clear();
            for (std::size_t i : live_)
                if (events_[i].ev.endSample > lastStart_) ends.push_back(events_[i].ev.endSample);
            std::sort(ends.begin(), ends.end());
            drop = ends.size() < minA ? lastStart_ : ends[ends.size() - minA];
            if (!pendingOverlapValid_) {
                pendingOverlap_ = msToSmp(p_.overlapMinMs) + fracOf(global_.uniform01(), jitter300_);
                pendingOverlapValid_ = true;
            }
            // Overlap floor of 120 ms: the forced start can then always keep the 40 ms start
            // guard without opening a gap below min.
            const std::int64_t target = drop - std::max(pendingOverlap_, msToSmp(120.0));
            const std::int64_t lo = std::max({lastStart_ + guard_, floorTime_, forcedBlockedUntil_});
            bool found = false;
            for (std::size_t j = 0; j < slots_.size(); ++j) {
                const std::int64_t ce = cooldownEnd(slots_[j]);
                if (ce > drop) continue;
                const std::int64_t tj = std::max({target, ce, lo});
                if (!found || tj < tf || (tj == tf && slots_[j].nextStart < slots_[jf].nextStart)) {
                    tf = tj; jf = j; found = true;
                }
            }
            if (!found) {
                std::int64_t bestCe = kInf;
                for (std::size_t j = 0; j < slots_.size(); ++j) {
                    const std::int64_t ce = cooldownEnd(slots_[j]);
                    if (ce < bestCe) { bestCe = ce; jf = j; }
                }
                tf = std::max(bestCe, lo);
            }
        }
        const bool forced = tf < ts;
        const std::int64_t tc = forced ? tf : ts;
        const std::size_t jc = forced ? jf : js;

        if (nextTick_ <= tc) {
            if (nextTick_ >= T) break;
            tick();
            continue;
        }
        if (tc >= T) break;
        if (!forced && tc < lastStart_ + guard_) {  // anti-synchrony: shift by +40..120 ms
            slots_[js].nextStart = tc + guard_ + fracOf(global_.uniform01(), jitter80_);
            ++stats_.startShifts;
            continue;
        }
        const int speakingBefore = speakingOthersAt(tc);
        std::uint16_t flags = forced ? kEvForced : 0;
        if (slots_[jc].pairedNext) flags |= kEvPaired;
        if (!construct(static_cast<std::uint32_t>(jc), tc, flags, -1.0)) {
            if (forced) forcedBlockedUntil_ = tc + msToSmp(200.0);
            else slots_[jc].nextStart = tc + msToSmp(200.0);
            continue;
        }
        slots_[jc].pairedNext = false;
        if (forced) {
            pendingOverlapValid_ = false;
            ++stats_.forcedStarts;
            if (tc > drop) ++stats_.forcedLate;
        }
        // CVR onset pairing: a start into silence needs another start within the window.
        if (p_.cvrOnsetWindowMs > 0.0 && speakingBefore == 0) {
            const std::int64_t X = msToSmp(p_.cvrOnsetWindowMs);
            bool exists = false;
            for (std::size_t k = 0; k < slots_.size(); ++k)
                if (k != jc && slots_[k].nextStart > tc && slots_[k].nextStart <= tc + X) exists = true;
            if (!exists) {
                std::size_t kb = slots_.size();
                for (std::size_t k = 0; k < slots_.size(); ++k) {
                    if (k == jc || cooldownEnd(slots_[k]) > tc + X) continue;
                    if (kb == slots_.size() || slots_[k].nextStart < slots_[kb].nextStart) kb = k;
                }
                if (kb < slots_.size()) {
                    const std::int64_t w = std::max<std::int64_t>(0, X - guard_);
                    const std::int64_t nt = std::max(cooldownEnd(slots_[kb]),
                                                     tc + guard_ + fracOf(global_.uniform01(), w));
                    if (nt < slots_[kb].nextStart) {
                        slots_[kb].nextStart = nt;
                        slots_[kb].pairedNext = true;
                        ++stats_.pairedStarts;
                    }
                }
            }
        }
    }
}

void TalkerPlanner::planContinuous(std::int64_t T) {
    while (true) {
        pruneLive();
        std::size_t j = 0;
        std::int64_t t = std::numeric_limits<std::int64_t>::max();
        for (std::size_t k = 0; k < slots_.size(); ++k)
            if (slots_[k].nextStart < t) { t = slots_[k].nextStart; j = k; }
        if (slots_.empty() || t >= T) break;
        if (slots_[j].chainFresh && t < lastStart_ + guard_) {
            slots_[j].nextStart = t + guard_ + fracOf(global_.uniform01(), jitter80_);
            ++stats_.startShifts;
            continue;
        }
        if (!construct(static_cast<std::uint32_t>(j), t, 0, -1.0)) {
            slots_[j].nextStart = t + msToSmp(200.0);
            slots_[j].chainFresh = true;
        }
    }
}

std::vector<std::uint64_t> TalkerPlanner::replan(const TalkerPlanParams& params, std::int64_t now) {
    const std::int64_t freeze = now + freeze_;
    std::vector<std::uint64_t> discarded;
    while (!events_.empty() && events_.back().ev.startSample >= freeze) {
        addOccupancy(events_.back().ev, -1);
        discarded.push_back(events_.back().ev.eventId);
        events_.pop_back();
    }
    taken_ = std::min(taken_, events_.size());
    ++epoch_;
    p_ = params;
    planHash_ = p_.hash();
    const std::size_t V = p_.slots();
    std::vector<Slot> old = std::move(slots_);
    slots_.assign(V, Slot{});
    for (std::size_t j = 0; j < V && j < old.size(); ++j) {
        slots_[j].hasSpeaker = old[j].hasSpeaker;
        slots_[j].speaker = old[j].speaker;
    }
    initStreams();
    sel_.reseed(epoch_, planHash_);
    sel_.setVoiceSlots(p_.slots(), p_.targetMean());
    sel_.setPoolSize(p_.pool);
    sel_.setSoloRiskWeight(p_.cvrSoloRiskWeight);
    recomputeRates();

    std::vector<const TalkerEvent*> lastOf(V, nullptr);
    for (const auto& pe : events_)
        if (pe.ev.slot < V) lastOf[pe.ev.slot] = &pe.ev;
    lastStart_ = -guard_;
    for (const auto& pe : events_) lastStart_ = std::max(lastStart_, pe.ev.startSample);
    live_.clear();
    for (std::size_t i = 0; i < events_.size(); ++i)
        if (events_[i].ev.endSample > lastStart_ - keepEnded_) live_.push_back(i);
    floorTime_ = freeze;
    pendingOverlapValid_ = false;
    forcedBlockedUntil_ = kNever;

    if (p_.mode == PlanMode::ContinuousN) {
        std::vector<bool> used(snap_->numSpeakers(), false);
        for (const auto& s : slots_) if (s.hasSpeaker) used[s.speaker] = true;
        auto set = sel_.chooseSpeakerSet(static_cast<std::uint32_t>(sel_.pool().size()), lab_);
        std::size_t k = 0;
        for (auto& s : slots_) {
            if (s.hasSpeaker) continue;
            while (k < set.size() && used[set[k]]) ++k;
            if (k >= set.size()) break;
            s.hasSpeaker = true;
            s.speaker = set[k];
            used[set[k]] = true;
        }
    }
    for (std::size_t j = 0; j < V; ++j) {
        Slot& s = slots_[j];
        s.lastEnd = lastOf[j] ? lastOf[j]->endSample : kNever;
        if (p_.mode == PlanMode::Stochastic) {
            if (s.lastEnd > freeze) {
                s.nextStart = s.lastEnd + offDuration(s);
            } else {
                const double cd = p_.reEntryCooldownMs / 1000.0;
                const double meanEff = std::max(dOffS_ / lambda_ - cd, 0.05);
                s.nextStart = std::max(cooldownEnd(s), freeze) + sToSmp(exponential(s.rng, meanEff));
            }
            s.chainFresh = false;
        } else if (lastOf[j] && lastOf[j]->fadeOutStart >= freeze) {
            s.nextStart = lastOf[j]->fadeOutStart;
            s.chainFresh = false;
        } else {
            s.nextStart = freeze;
            s.chainFresh = true;
        }
    }
    return discarded;
}

std::vector<std::uint64_t> TalkerPlanner::adoptSnapshot(std::shared_ptr<const CorpusSnapshot> snap, const CorpusMigration& map,
                                                        std::int64_t now) {
    snap_ = std::move(snap);
    for (auto& s : slots_) {
        if (!s.hasSpeaker) continue;
        const SpeakerId n = map.mapSpeaker(s.speaker);
        if (n == CorpusMigration::kNoSpeaker) s.hasSpeaker = false;
        else s.speaker = n;
    }
    // Kept events keep their recording / segment ids of the old snapshot (their layouts are built);
    // only the speaker id matters to the planner (the "active speakers" exclusion of the selector).
    for (auto& pe : events_) pe.ev.speaker = map.mapSpeaker(pe.ev.speaker);
    return replan(p_, now);
}

std::size_t TalkerPlanner::takeNew(std::vector<PlannedEvent>& out) {
    const std::size_t n = events_.size() - taken_;
    for (std::size_t i = taken_; i < events_.size(); ++i) out.push_back(events_[i]);
    taken_ = events_.size();
    return n;
}

double TalkerPlanner::plannedBusPower(const std::vector<PlannedEvent>& events, std::int64_t t0, std::int64_t t1) {
    double total = 0.0;
    for (const auto& pe : events) {
        const auto& e = pe.ev;
        if (!pe.layout || e.endSample <= t0 || e.startSample >= t1) continue;
        const double len = static_cast<double>(e.length());
        const double fi = static_cast<double>(e.fadeInLen);
        const double fos = static_cast<double>(e.fadeOutStart - e.startSample);
        const double w0 = static_cast<double>(std::max<std::int64_t>(0, t0 - e.startSample));
        const double w1 = static_cast<double>(std::min(e.length(), t1 - e.startSample));
        double acc = 0.0;
        for (const auto& r : pe.layout->speech) {
            const double a = std::max(static_cast<double>(r.start), w0);
            const double b = std::min(static_cast<double>(r.end), w1);
            if (b > a) acc += fadeSqIntegral(a, b, fi, fos, len);
        }
        total += acc * detexp(static_cast<double>(e.levelVarDb) * kLn10 / 10.0);
    }
    return total / static_cast<double>(t1 - t0);
}

double TalkerPlanner::estimateBusPowerFactor(double seconds) const {
    SegmentSelector selCopy = sel_;
    TalkerPlanParams pp = p_;
    pp.seed = p_.seed ^ 0xB5AD4ECEDA1CE2A9ULL;
    TalkerPlanner probe(snap_, selCopy, pp, static_cast<double>(fs_));
    probe.setLayoutRetention(LayoutRetention::All);
    const std::int64_t T = sToSmp(seconds);
    probe.planUntil(T);
    return plannedBusPower(probe.events_, 0, T);
}

nlohmann::json TalkerPlanner::eventsJson() const {
    nlohmann::ordered_json j;
    j["schema"] = "babbleforge.events/1";
    j["seed"] = p_.seed;
    j["corpusVersion"] = snap_->corpusVersion();
    j["epoch"] = epoch_;
    nlohmann::ordered_json arr = nlohmann::ordered_json::array();
    for (const auto& pe : events_) {
        const auto& e = pe.ev;
        nlohmann::ordered_json o;
        o["id"] = e.eventId;
        o["epoch"] = e.epoch;
        o["slot"] = e.slot;
        o["start"] = e.startSample;
        o["fadeIn"] = e.fadeInLen;
        o["fadeOutStart"] = e.fadeOutStart;
        o["end"] = e.endSample;
        o["speaker"] = e.speaker;
        o["recording"] = e.recording;
        o["segment"] = e.segment;
        o["anchor"] = e.anchor;
        o["gain"] = e.segGainLin;
        o["levelVarDb"] = e.levelVarDb;
        o["pan"] = e.pan;
        o["flags"] = e.flags;
        arr.push_back(std::move(o));
    }
    j["events"] = std::move(arr);
    return nlohmann::json::parse(j.dump());  // plain json (sorted keys) for comparisons
}

std::string TalkerPlanner::eventsJsonString() const {
    return eventsJson().dump();
}

}  // namespace bf
