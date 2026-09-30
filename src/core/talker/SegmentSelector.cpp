#include "core/talker/SegmentSelector.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include <nlohmann/json.hpp>

#include "core/math/DetMath.h"
#include "core/random/Distributions.h"

namespace bf {

namespace {
constexpr double kFs = static_cast<double>(kCorpusRate);
constexpr float kFeatureWeights[7] = {1.0f, 0.5f, 0.5f, 0.5f, 0.25f, 0.25f, 0.25f};
constexpr std::size_t kRecentCap = 256;

std::int64_t secToSamples(double s) { return static_cast<std::int64_t>(std::llround(s * kFs)); }
}  // namespace

SegmentSelector::SegmentSelector(std::shared_ptr<const CorpusSnapshot> snap, const SelectorConfig& cfg)
    : snap_(std::move(snap)), cfg_(cfg), rng_(cfg.seed, "selector", 0) {
    const std::size_t ns = snap_->numSpeakers();
    spk_.resize(ns);
    cool_.resize(snap_->numRecordings());
    eligibleAnchors_.resize(ns);
    const std::int64_t minMat = secToSamples(cfg_.segMinS + 0.5);
    for (SpeakerId s = 0; s < ns; ++s) {
        const auto anchors = snap_->anchorsOf(s);
        for (std::uint32_t i = 0; i < anchors.size(); ++i)
            if (anchors[i].maxLen >= minMat) eligibleAnchors_[s].push_back(i);
    }
    buildFeatureSpace();
    buildEligible();
    buildPool();
    nextRotation_ = cfg_.rotationPeriodS > 0.0 ? secToSamples(cfg_.rotationPeriodS)
                                               : std::numeric_limits<std::int64_t>::max();
}

void SegmentSelector::reseed(std::uint64_t epoch, std::uint64_t planHash) {
    rng_ = RngStream(epoch == 0 ? cfg_.seed : (cfg_.seed ^ planHash), "selector", epoch);
}

void SegmentSelector::setPoolSize(std::uint32_t p) {
    if (p == cfg_.poolSize) return;
    cfg_.poolSize = p;
    buildPool();
}

void SegmentSelector::setVoiceSlots(std::uint32_t v, double meanActive) {
    cfg_.voiceSlots = v;
    cfg_.meanActive = meanActive;
    buildEligible();
}

// ------------------------------------------------------------------ feature space / pool

void SegmentSelector::buildFeatureSpace() {
    const std::size_t ns = snap_->numSpeakers();
    z_.assign(ns, std::vector<float>(kNumSpeakerFeatures, 0.0f));
    for (std::size_t f = 0; f < 7; ++f) {
        double sum = 0.0, sum2 = 0.0;
        std::size_t n = 0;
        for (SpeakerId s = 0; s < ns; ++s) {
            if (!snap_->speakerHealthy(s)) continue;
            const double v = snap_->speaker(s).features[f];
            sum += v;
            sum2 += v * v;
            ++n;
        }
        if (n == 0) continue;
        const double mean = sum / static_cast<double>(n);
        const double var = std::max(0.0, sum2 / static_cast<double>(n) - mean * mean);
        const double sd = detsqrt(var);
        for (SpeakerId s = 0; s < ns; ++s)
            z_[s][f] = sd > 1e-12 ? static_cast<float>((snap_->speaker(s).features[f] - mean) / sd) : 0.0f;
    }
    for (SpeakerId s = 0; s < ns; ++s) z_[s][kFeatLanguage] = snap_->speaker(s).features[kFeatLanguage];
}

double SegmentSelector::featureDistance(SpeakerId a, SpeakerId b) const {
    double d = 0.0;
    for (std::size_t f = 0; f < 7; ++f) {
        const double x = static_cast<double>(z_[a][f]) - static_cast<double>(z_[b][f]);
        d += static_cast<double>(kFeatureWeights[f]) * x * x;
    }
    double r = detsqrt(d);
    if (cfg_.languageAware && z_[a][kFeatLanguage] != z_[b][kFeatLanguage]) r += 1.0;
    return r;
}

void SegmentSelector::buildEligible() {
    eligible_.clear();
    double material = 0.0;
    for (SpeakerId s = 0; s < snap_->numSpeakers(); ++s) {
        if (!snap_->speakerHealthy(s) || eligibleAnchors_[s].empty()) continue;
        if (cfg_.diversity == DiversityMode::High && std::fabs(z_[s][kFeatF0MedianSt]) > 2.5f) continue;
        eligible_.push_back(s);
        material += snap_->usableSpeechSeconds(s);
    }
    // T_seg,eff = min(45 min, 0.8 * M_elig / r_consume), minimum 5 min (TALKER §6.4).
    const double r = std::max(cfg_.meanActive, 1e-3);
    segCooldownS_ = std::max(300.0, std::min(2700.0, 0.8 * material / r));
    if (cfg_.segCooldownOverrideS >= 0.0) segCooldownS_ = cfg_.segCooldownOverrideS;
}

void SegmentSelector::buildPool() {
    pool_.clear();
    const std::size_t n = eligible_.size();
    const std::size_t P = std::min<std::size_t>(cfg_.poolSize, n);
    if (P == 0) return;
    std::vector<SpeakerId> chosen;

    switch (cfg_.diversity) {
    case DiversityMode::Low: {
        std::size_t medoid = 0;
        double best = std::numeric_limits<double>::max();
        for (std::size_t i = 0; i < n; ++i) {
            double sum = 0.0;
            for (std::size_t j = 0; j < n; ++j) sum += featureDistance(eligible_[i], eligible_[j]);
            if (sum < best) { best = sum; medoid = i; }
        }
        std::vector<std::pair<double, SpeakerId>> d;
        for (SpeakerId s : eligible_) d.push_back({featureDistance(eligible_[medoid], s), s});
        std::sort(d.begin(), d.end());
        for (std::size_t i = 0; i < P; ++i) chosen.push_back(d[i].second);
        break;
    }
    case DiversityMode::Balanced: {
        std::vector<SpeakerId> byF0 = eligible_;
        std::stable_sort(byF0.begin(), byF0.end(), [&](SpeakerId a, SpeakerId b) {
            return snap_->speaker(a).features[kFeatF0MedianSt] < snap_->speaker(b).features[kFeatF0MedianSt];
        });
        std::vector<std::vector<SpeakerId>> q(4);
        for (std::size_t i = 0; i < n; ++i) q[(4 * i) / n].push_back(byF0[i]);
        std::size_t cnt[4] = {P / 4, P / 4, P / 4, P / 4};
        const std::size_t order[4] = {1, 2, 0, 3};  // remainder keeps the lower/upper halves balanced
        for (std::size_t k = 0; k < P % 4; ++k) ++cnt[order[k]];
        std::vector<SpeakerId> leftovers;
        for (std::size_t qi = 0; qi < 4; ++qi) {
            auto& v = q[qi];
            for (std::size_t k = 0; k < v.size(); ++k) {  // partial Fisher-Yates
                const std::size_t j = k + static_cast<std::size_t>(rng_.uniformInt(v.size() - k));
                std::swap(v[k], v[j]);
            }
            const std::size_t take = std::min(cnt[qi], v.size());
            chosen.insert(chosen.end(), v.begin(), v.begin() + static_cast<std::ptrdiff_t>(take));
            leftovers.insert(leftovers.end(), v.begin() + static_cast<std::ptrdiff_t>(take), v.end());
        }
        for (std::size_t k = 0; chosen.size() < P && k < leftovers.size(); ++k) chosen.push_back(leftovers[k]);
        break;
    }
    case DiversityMode::High: {
        std::vector<bool> used(n, false);
        std::size_t first = static_cast<std::size_t>(rng_.uniformInt(n));
        used[first] = true;
        chosen.push_back(eligible_[first]);
        while (chosen.size() < P) {
            std::vector<std::pair<double, std::size_t>> cand;
            for (std::size_t i = 0; i < n; ++i) {
                if (used[i]) continue;
                double md = std::numeric_limits<double>::max();
                for (SpeakerId c : chosen) md = std::min(md, featureDistance(eligible_[i], c));
                cand.push_back({-md, i});
            }
            std::sort(cand.begin(), cand.end());
            const std::size_t k = std::min<std::size_t>(3, cand.size());
            const std::size_t pickIdx = cand[static_cast<std::size_t>(rng_.uniformInt(k))].second;
            used[pickIdx] = true;
            chosen.push_back(eligible_[pickIdx]);
        }
        break;
    }
    case DiversityMode::Matched: {
        const auto& qs = cfg_.target.f0MedianQuantilesHz;
        std::vector<bool> used(n, false);
        for (std::size_t k = 0; k < P; ++k) {
            double targetSt = 0.0;
            if (!qs.empty()) {
                const double pos = (static_cast<double>(k) + 0.5) / static_cast<double>(P) *
                                   static_cast<double>(qs.size() - 1);
                const std::size_t i0 = std::min(static_cast<std::size_t>(pos), qs.size() - 1);
                const std::size_t i1 = std::min(i0 + 1, qs.size() - 1);
                const double fr = pos - static_cast<double>(i0);
                const double hz = qs[i0] + (qs[i1] - qs[i0]) * fr;
                targetSt = 12.0 * detlog(hz / 100.0) / detlog(2.0);
            }
            std::size_t best = n;
            double bd = std::numeric_limits<double>::max();
            for (std::size_t i = 0; i < n; ++i) {
                if (used[i]) continue;
                const double d = std::fabs(snap_->speaker(eligible_[i]).features[kFeatF0MedianSt] - targetSt);
                if (d < bd) { bd = d; best = i; }
            }
            if (best == n) break;
            used[best] = true;
            chosen.push_back(eligible_[best]);
        }
        break;
    }
    }
    std::sort(chosen.begin(), chosen.end());
    pool_ = std::move(chosen);
}

SpeakerId SegmentSelector::nearestNonPool(SpeakerId ref, const std::vector<bool>& inPool,
                                          const std::vector<bool>& taken) const {
    SpeakerId best = ref;
    double bd = std::numeric_limits<double>::max();
    for (SpeakerId s : eligible_) {
        if (inPool[s] || taken[s]) continue;
        const double d = featureDistance(ref, s);
        if (d < bd) { bd = d; best = s; }
    }
    return best;
}

void SegmentSelector::maybeRotatePool(std::int64_t t, std::span<const SpeakerId> active) {
    while (t >= nextRotation_) {
        nextRotation_ += secToSamples(cfg_.rotationPeriodS);
        const std::size_t P = pool_.size();
        if (P == 0 || eligible_.size() <= P) continue;
        const std::size_t nRep = (P + 3) / 4;
        std::vector<bool> inPool(snap_->numSpeakers(), false), taken(snap_->numSpeakers(), false);
        for (SpeakerId s : pool_) inPool[s] = true;
        // Least recently used first (never used = least recent), not active.
        std::vector<std::pair<std::int64_t, SpeakerId>> lru;
        for (SpeakerId s : pool_) {
            if (std::find(active.begin(), active.end(), s) != active.end()) continue;
            lru.push_back({spk_[s].hasUse ? spk_[s].lastUseStart : std::numeric_limits<std::int64_t>::min(), s});
        }
        std::sort(lru.begin(), lru.end());
        // F0 quartile per eligible speaker (Balanced).
        std::vector<int> quart(snap_->numSpeakers(), -1);
        if (cfg_.diversity == DiversityMode::Balanced) {
            std::vector<SpeakerId> byF0 = eligible_;
            std::stable_sort(byF0.begin(), byF0.end(), [&](SpeakerId a, SpeakerId b) {
                return snap_->speaker(a).features[kFeatF0MedianSt] < snap_->speaker(b).features[kFeatF0MedianSt];
            });
            for (std::size_t i = 0; i < byF0.size(); ++i) quart[byF0[i]] = static_cast<int>((4 * i) / byF0.size());
        }
        std::size_t done = 0;
        for (std::size_t k = 0; k < lru.size() && done < nRep; ++k) {
            const SpeakerId out = lru[k].second;
            SpeakerId in = out;
            if (cfg_.diversity == DiversityMode::Balanced) {
                std::vector<SpeakerId> same, any;
                for (SpeakerId s : eligible_) {
                    if (inPool[s] || taken[s]) continue;
                    any.push_back(s);
                    if (quart[s] == quart[out]) same.push_back(s);
                }
                const auto& from = same.empty() ? any : same;
                if (from.empty()) break;
                in = from[static_cast<std::size_t>(rng_.uniformInt(from.size()))];
            } else {
                in = nearestNonPool(out, inPool, taken);
                if (in == out) break;
            }
            taken[in] = true;
            inPool[out] = false;
            inPool[in] = true;
            *std::find(pool_.begin(), pool_.end(), out) = in;
            ++done;
        }
        std::sort(pool_.begin(), pool_.end());
        if (done > 0) ++stats_.rotations;
    }
}

std::vector<SpeakerId> SegmentSelector::chooseSpeakerSet(std::uint32_t n, RngStream& rng) const {
    std::vector<SpeakerId> v = pool_;
    const std::size_t k = std::min<std::size_t>(n, v.size());
    for (std::size_t i = 0; i < k; ++i) {
        const std::size_t j = i + static_cast<std::size_t>(rng.uniformInt(v.size() - i));
        std::swap(v[i], v[j]);
    }
    v.resize(k);
    return v;
}

// ------------------------------------------------------------------ shuffle / cooldown

void SegmentSelector::newCycle(SpeakerId s) {
    SpeakerState& st = spk_[s];
    const auto& base = eligibleAnchors_[s];
    const std::size_t n = base.size();
    std::vector<std::uint32_t> prevTail;
    const bool had = !st.perm.empty();
    if (had) {
        const std::size_t tail = (n * 2) / 10;
        prevTail.assign(st.perm.end() - static_cast<std::ptrdiff_t>(std::min(tail, st.perm.size())), st.perm.end());
        ++st.cycle;
    }
    st.perm = base;
    RngStream r(cfg_.seed, "selector.shuffle." + std::to_string(s) + "." + std::to_string(st.cycle), 0);
    for (std::size_t i = n; i > 1; --i) {  // Fisher-Yates
        const std::size_t j = static_cast<std::size_t>(r.uniformInt(i));
        std::swap(st.perm[i - 1], st.perm[j]);
    }
    // Cycle constraint: anchors of the last 20 % of the previous cycle may not appear in the
    // first 20 % of the new one (swap repair with the first admissible later position).
    const std::size_t head = (n * 2) / 10;
    auto inTail = [&](std::uint32_t a) {
        return std::find(prevTail.begin(), prevTail.end(), a) != prevTail.end();
    };
    std::size_t j = head;
    for (std::size_t i = 0; i < head; ++i) {
        if (!inTail(st.perm[i])) continue;
        while (j < n && inTail(st.perm[j])) ++j;
        if (j >= n) break;
        std::swap(st.perm[i], st.perm[j]);
        ++j;
    }
    st.pos = 0;
    const auto anchors = snap_->anchorsOf(s);
    st.soloRemaining = 0;
    for (std::uint32_t a : st.perm)
        if (anchors[a].flags & kSegSoloRisk) ++st.soloRemaining;
}

bool SegmentSelector::anchorInCooldown(SegmentId g, std::int64_t t) const {
    const SegmentRec& seg = snap_->segment(g);
    for (const auto& c : cool_[seg.recording])
        if (c.expiry > t && seg.anchor >= c.start && seg.anchor < c.end) return true;
    return false;
}

std::optional<std::uint32_t> SegmentSelector::nextAnchor(SpeakerId s, std::int64_t t) {
    SpeakerState& st = spk_[s];
    const std::size_t n = eligibleAnchors_[s].size();
    if (n == 0) return std::nullopt;
    if (st.perm.empty()) newCycle(s);
    const std::uint32_t first = snap_->speaker(s).firstAnchor;
    const auto anchors = snap_->anchorsOf(s);
    auto consume = [&]() {
        if (st.pos >= st.perm.size()) newCycle(s);
        const std::uint32_t a = st.perm[st.pos++];
        if ((anchors[a].flags & kSegSoloRisk) && st.soloRemaining > 0) --st.soloRemaining;
        return a;
    };
    for (std::size_t tries = 0; tries < 2 * n; ++tries) {
        const std::uint32_t a = consume();
        if (anchorInCooldown(first + a, t)) { ++stats_.cooldownSkips; continue; }
        return a;
    }
    ++stats_.cooldownRelaxed;
    return consume();
}

// ------------------------------------------------------------------ selection

std::uint32_t SegmentSelector::recentWindow() const noexcept {
    const auto P = static_cast<std::int64_t>(pool_.size());
    const auto V = static_cast<std::int64_t>(cfg_.voiceSlots);
    return static_cast<std::uint32_t>(std::max<std::int64_t>(1, (P - V) / 2));
}

bool SegmentSelector::eligibleAt(SpeakerId s, std::int64_t t, std::span<const SpeakerId> active,
                                 std::uint32_t recentN, bool timeRule) const {
    if (!snap_->speakerHealthy(s) || eligibleAnchors_[s].empty()) return false;
    if (std::find(active.begin(), active.end(), s) != active.end()) return false;
    const std::size_t nr = std::min<std::size_t>(recentN, recent_.size());
    for (std::size_t i = 0; i < nr; ++i)
        if (recent_[recent_.size() - 1 - i] == s) return false;
    if (timeRule && spk_[s].hasUse && t - spk_[s].lastUseEnd < secToSamples(cfg_.speakerReuseS)) return false;
    return true;
}

double SegmentSelector::weightOf(SpeakerId s, std::int64_t t) const {
    const SpeakerState& st = spk_[s];
    const double wDiv = static_cast<double>(snap_->speaker(s).weight);
    double wAge = 3.0;
    if (st.hasUse)
        wAge = std::min(1.0 + static_cast<double>(t - st.lastUseStart) / (cfg_.ageTS * kFs), 3.0);
    double wCvr = 1.0;
    if (cfg_.soloRiskWeight != 1.0) {
        std::size_t rem = st.perm.empty() ? 0 : st.perm.size() - st.pos;
        std::size_t solo = st.soloRemaining;
        if (rem == 0) {  // not yet shuffled / exhausted: all eligible anchors
            const auto anchors = snap_->anchorsOf(s);
            rem = eligibleAnchors_[s].size();
            solo = 0;
            for (std::uint32_t a : eligibleAnchors_[s])
                if (anchors[a].flags & kSegSoloRisk) ++solo;
        }
        if (rem > 0)
            wCvr = (static_cast<double>(solo) * cfg_.soloRiskWeight + static_cast<double>(rem - solo)) /
                   static_cast<double>(rem);
    }
    return wDiv * wAge * wCvr;
}

std::optional<SegmentPick> SegmentSelector::pick(std::int64_t t, std::span<const SpeakerId> active) {
    const std::uint32_t R = recentWindow();
    const std::uint32_t recentN[3] = {R, R / 2, 0};
    const bool timeRule[3] = {true, true, false};
    std::vector<SpeakerId> cand;
    std::vector<double> w;
    for (int stage = 0; stage < 3; ++stage) {
        cand.clear();
        w.clear();
        for (SpeakerId s : pool_) {
            if (!eligibleAt(s, t, active, recentN[stage], timeRule[stage])) continue;
            cand.push_back(s);
            w.push_back(weightOf(s, t));
        }
        if (cand.empty()) continue;
        ++stats_.relaxed[stage];
        const SpeakerId s = cand[weightedChoice(rng_, w)];
        const auto a = nextAnchor(s, t);
        if (!a) continue;
        ++stats_.picks;
        const SegmentId g = snap_->speaker(s).firstAnchor + *a;
        return SegmentPick{s, snap_->segment(g).recording, g, snap_->segment(g).anchor};
    }
    ++stats_.failures;
    return std::nullopt;
}

std::optional<SegmentPick> SegmentSelector::pickForSpeaker(SpeakerId s, std::int64_t t) {
    if (s >= spk_.size() || !snap_->speakerHealthy(s)) return std::nullopt;
    const auto a = nextAnchor(s, t);
    if (!a) return std::nullopt;
    ++stats_.picks;
    const SegmentId g = snap_->speaker(s).firstAnchor + *a;
    return SegmentPick{s, snap_->segment(g).recording, g, snap_->segment(g).anchor};
}

void SegmentSelector::commit(const SegmentPick& p, std::int64_t srcEnd, std::int64_t tStart, std::int64_t tEnd) {
    SpeakerState& st = spk_[p.speaker];
    st.hasUse = true;
    st.lastUseStart = tStart;
    st.lastUseEnd = tEnd;
    recent_.push_back(p.speaker);
    if (recent_.size() > kRecentCap) recent_.pop_front();
    auto& cl = cool_[p.recording];
    std::erase_if(cl, [&](const CoolRegion& c) { return c.expiry <= tStart; });
    cl.push_back({p.anchor, std::max(srcEnd, p.anchor + 1), tEnd + secToSamples(segCooldownS_)});
}

// ------------------------------------------------------------------ persistence

nlohmann::json SegmentSelector::exportState(std::int64_t now) const {
    nlohmann::json j;
    j["schema"] = "babbleforge.selectorstate/1";
    j["corpusVersion"] = snap_->corpusVersion();
    nlohmann::json sp = nlohmann::json::array();
    for (SpeakerId s = 0; s < spk_.size(); ++s) {
        const auto& st = spk_[s];
        if (st.perm.empty() && !st.hasUse) continue;
        nlohmann::json e;
        e["id"] = s;
        e["cycle"] = st.cycle;
        e["pos"] = st.pos;
        e["perm"] = st.perm;
        if (st.hasUse) {
            e["lastUseAgo"] = now - st.lastUseStart;
            e["lastEndAgo"] = now - st.lastUseEnd;
        }
        sp.push_back(std::move(e));
    }
    j["speakers"] = std::move(sp);
    nlohmann::json cd = nlohmann::json::array();
    for (RecordingId r = 0; r < cool_.size(); ++r)
        for (const auto& c : cool_[r])
            if (c.expiry > now) cd.push_back({{"rec", r}, {"start", c.start}, {"end", c.end}, {"remaining", c.expiry - now}});
    j["cooldowns"] = std::move(cd);
    j["pool"] = pool_;
    j["recent"] = std::vector<SpeakerId>(recent_.begin(), recent_.end());
    return j;
}

bool SegmentSelector::importState(const nlohmann::json& j, std::int64_t now) {
    try {
        if (j.value("schema", std::string()) != "babbleforge.selectorstate/1") return false;
        if (j.value("corpusVersion", std::string()) != snap_->corpusVersion()) return false;
        for (auto& st : spk_) st = SpeakerState{};
        for (const auto& e : j.at("speakers")) {
            const SpeakerId s = e.at("id").get<SpeakerId>();
            if (s >= spk_.size()) return false;
            auto& st = spk_[s];
            st.cycle = e.at("cycle").get<std::uint32_t>();
            st.pos = e.at("pos").get<std::uint32_t>();
            st.perm = e.at("perm").get<std::vector<std::uint32_t>>();
            if (st.perm.size() != eligibleAnchors_[s].size() || st.pos > st.perm.size()) return false;
            if (e.contains("lastUseAgo")) {
                st.hasUse = true;
                st.lastUseStart = now - e.at("lastUseAgo").get<std::int64_t>();
                st.lastUseEnd = now - e.at("lastEndAgo").get<std::int64_t>();
            }
            const auto anchors = snap_->anchorsOf(s);
            st.soloRemaining = 0;
            for (std::size_t i = st.pos; i < st.perm.size(); ++i)
                if (anchors[st.perm[i]].flags & kSegSoloRisk) ++st.soloRemaining;
        }
        for (auto& c : cool_) c.clear();
        for (const auto& c : j.at("cooldowns")) {
            const RecordingId r = c.at("rec").get<RecordingId>();
            if (r >= cool_.size()) return false;
            cool_[r].push_back({c.at("start").get<std::int64_t>(), c.at("end").get<std::int64_t>(),
                                now + c.at("remaining").get<std::int64_t>()});
        }
        auto pool = j.at("pool").get<std::vector<SpeakerId>>();
        if (!pool.empty()) pool_ = std::move(pool);
        recent_.clear();
        for (SpeakerId s : j.at("recent").get<std::vector<SpeakerId>>()) recent_.push_back(s);
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

}  // namespace bf
