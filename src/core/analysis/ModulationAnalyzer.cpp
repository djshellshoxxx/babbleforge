#include "core/analysis/ModulationAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace bf {

namespace {
constexpr double kPi = 3.14159265358979323846;
double levelDb(double p) noexcept { return 10.0 * std::log10(std::max(p, 1e-20)); }

// Linear-interpolated percentile of an ascending-sorted vector, p in [0,1].
double percentileSorted(const std::vector<double>& v, double p) noexcept {
    if (v.empty()) return 0.0;
    const double pos = p * static_cast<double>(v.size() - 1);
    const auto i = static_cast<std::size_t>(pos);
    const double f = pos - static_cast<double>(i);
    return i + 1 < v.size() ? v[i] * (1.0 - f) + v[i + 1] * f : v[i];
}
}  // namespace

const std::array<double, kNumModBands>& modulationBandCentresHz() noexcept {
    static const std::array<double, kNumModBands> c{0.5, 0.63, 0.8, 1.0, 1.25, 1.6, 2.0, 2.5,
                                                    3.15, 4.0, 5.0, 6.3, 8.0, 10.0, 12.5, 16.0};
    return c;
}

// ---- ModulationSpectrumTracker ----------------------------------------------------------------

ModulationSpectrumTracker::ModulationSpectrumTracker(double frameRateHz)
    : rate_(frameRateHz),
      blockLen_(static_cast<std::size_t>(std::llround(20.0 * frameRateHz))),
      hop_(blockLen_ / 2),
      fft_([&] { std::size_t n = 4; while (n < static_cast<std::size_t>(std::llround(20.0 * frameRateHz))) n *= 2; return n; }()) {
    enbw_ = 1.5 * static_cast<double>(fft_.size()) / static_cast<double>(blockLen_);  // Hann ENBW in padded bins
    window_.assign(blockLen_, 0.0);
    for (std::size_t i = 0; i < blockLen_; ++i)
        window_[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(blockLen_));
    work_.assign(fft_.size(), 0.0);
    spec_.assign(fft_.numBins(), {});
    ring_.assign(blockLen_, 0.0);
    const double df = rate_ / static_cast<double>(fft_.size());
    for (std::size_t b = 0; b < kNumModBands; ++b) {
        const double fc = std::pow(10.0, (static_cast<double>(b) - 3.0) / 10.0);  // exact base-10 centres
        const double lo = fc * std::pow(10.0, -0.05), hi = fc * std::pow(10.0, 0.05);
        // bins whose centre frequency lies in [lo, hi)
        binRange_[b] = {static_cast<std::size_t>(std::ceil(lo / df)), static_cast<std::size_t>(std::ceil(hi / df))};
    }
}

void ModulationSpectrumTracker::reset() {
    std::fill(ring_.begin(), ring_.end(), 0.0);
    sinceBlock_ = total_ = 0;
    recent_.clear();
}

void ModulationSpectrumTracker::push(double p) {
    ring_[total_ % blockLen_] = p;
    ++total_;
    ++sinceBlock_;
    if (total_ >= blockLen_ && sinceBlock_ >= hop_) {
        sinceBlock_ = 0;
        computeBlock();
    }
}

void ModulationSpectrumTracker::computeBlock() {
    std::fill(work_.begin(), work_.end(), 0.0);
    for (std::size_t i = 0; i < blockLen_; ++i) work_[i] = window_[i] * ring_[(total_ + i) % blockLen_];
    fft_.forward(work_.data(), spec_.data());
    const double e0 = spec_[0].real();
    Rec r{total_, {}};
    if (e0 > 0.0) {
        for (std::size_t b = 0; b < kNumModBands; ++b) {
            double s = 0.0;
            for (std::size_t k = binRange_[b].first; k < binRange_[b].second && k < spec_.size(); ++k) {
                const double m = 2.0 * std::abs(spec_[k]) / e0;
                s += m * m;
            }
            r.m[b] = std::sqrt(s / enbw_);
        }
    }
    recent_.push_back(r);
    const auto horizon = static_cast<std::size_t>(std::llround(60.0 * rate_));
    while (!recent_.empty() && total_ - recent_.front().endFrame >= horizon) recent_.erase(recent_.begin());
}

ModBandArray ModulationSpectrumTracker::average() const noexcept {
    ModBandArray a{};
    if (recent_.empty()) return a;
    for (const auto& r : recent_)
        for (std::size_t b = 0; b < kNumModBands; ++b) a[b] += r.m[b];
    for (auto& v : a) v /= static_cast<double>(recent_.size());
    return a;
}

// ---- ModulationAnalyzer -----------------------------------------------------------------------

ModulationAnalyzer::ModulationAnalyzer(bool octaveCarriers, double frameRateHz)
    : rate_(frameRateHz),
      cap_(static_cast<std::size_t>(std::llround(600.0 * frameRateHz))),
      w10_(static_cast<std::size_t>(std::llround(10.0 * frameRateHz))),
      w60_(static_cast<std::size_t>(std::llround(60.0 * frameRateHz))),
      broadband_(frameRateHz) {
    ring_.assign(cap_, {});
    lpRing_.assign(cap_, 0.0);
    if (octaveCarriers) octave_.assign(kNumModCarriers, ModulationSpectrumTracker(frameRateHz));
    // RBJ low-pass, fc = 16 Hz, Butterworth Q
    const double w0 = 2.0 * kPi * std::min(16.0, 0.45 * frameRateHz) / frameRateHz;
    const double alpha = std::sin(w0) / (2.0 * 0.70710678118654752);
    const double a0 = 1.0 + alpha;
    b0_ = (1.0 - std::cos(w0)) / 2.0 / a0;
    b1_ = (1.0 - std::cos(w0)) / a0;
    b2_ = b0_;
    a1_ = -2.0 * std::cos(w0) / a0;
    a2_ = (1.0 - alpha) / a0;
}

void ModulationAnalyzer::reset() {
    count_ = 0;
    sum10_ = 0.0;
    inGap_ = false;
    gapLen_ = 0;
    gaps_.clear();
    z1_ = z2_ = 0.0;
    broadband_.reset();
    for (auto& o : octave_) o.reset();
}

const ModulationAnalyzer::Frame& ModulationAnalyzer::at(std::size_t back) const noexcept {
    return ring_[(count_ - 1 - back) % cap_];
}

void ModulationAnalyzer::pushFrame(double power, int ka, int ks, std::span<const double> talkerPowers,
                                   std::span<const double> octavePowers) {
    power = std::max(power, 0.0);
    bool solo = false;
    if (ks == 1) solo = true;
    else if (!talkerPowers.empty()) {
        double mx = 0.0, sum = 0.0;
        for (double t : talkerPowers) { mx = std::max(mx, t); sum += t; }
        const double others = sum - mx;
        if (mx > 0.0 && mx >= others * std::pow(10.0, kSoloDominanceDb / 10.0)) solo = true;
    }
    // low-pass (16 Hz) power envelope; prime the state with the first sample
    if (count_ == 0) { z1_ = power * (1.0 - b0_); z2_ = power * (b2_ - a2_); }
    const double y = b0_ * power + z1_;
    z1_ = b1_ * power - a1_ * y + z2_;
    z2_ = b2_ * power - a2_ * y;
    const std::size_t slot = count_ % cap_;
    ring_[slot] = {power, static_cast<float>(ka), static_cast<float>(ks), solo};
    lpRing_[slot] = y;
    ++count_;
    sum10_ += power;
    if (count_ > w10_) sum10_ -= at(w10_).power;
    // gap detection against Leq of the last 10 s (incl. this frame)
    const double leq = levelDb(sum10_ / static_cast<double>(std::min(count_, w10_)));
    const bool below = levelDb(power) < leq - kGapBelowLeqDb;
    if (below) {
        inGap_ = true;
        ++gapLen_;
    } else if (inGap_) {
        gaps_.push_back({static_cast<double>(gapLen_) / rate_, count_ - 1});
        inGap_ = false;
        gapLen_ = 0;
    }
    broadband_.push(power);
    for (std::size_t c = 0; c < octave_.size(); ++c)
        octave_[c].push(c < octavePowers.size() ? octavePowers[c] : 0.0);
}

TemporalDensity ModulationAnalyzer::classify(double crestDb) noexcept {
    if (crestDb > 12.0) return TemporalDensity::Low;
    if (crestDb >= 7.0) return TemporalDensity::Medium;
    return TemporalDensity::High;
}

ModulationSnapshot ModulationAnalyzer::snapshot() const {
    ModulationSnapshot s;
    s.frames = count_;
    if (count_ == 0) return s;
    const std::size_t n10 = std::min(count_, w10_), n60 = std::min(count_, w60_), nAll = std::min(count_, cap_);
    s.leq10sDb = levelDb(sum10_ / static_cast<double>(n10));

    auto crest = [&](std::size_t n) {
        std::vector<double> v(n);
        for (std::size_t i = 0; i < n; ++i) v[i] = levelDb(at(i).power);
        std::sort(v.begin(), v.end());
        return percentileSorted(v, 0.9) - percentileSorted(v, 0.1);  // L10 - L90
    };
    s.crest10sDb = crest(n10);
    s.crest60sDb = crest(n60);
    s.density = classify(s.crest60sDb);

    // occupancy, k means, solo
    std::size_t occ60 = 0, occAll = 0, soloN = 0, known = 0;
    double ka = 0.0, ks = 0.0;
    for (std::size_t i = 0; i < nAll; ++i) {
        const Frame& f = at(i);
        const bool speaking = f.ks >= 1.f;
        if (f.ks >= 0.f) {
            occAll += speaking;
            if (i < n60) { occ60 += speaking; ka += static_cast<double>(f.ka); ks += static_cast<double>(f.ks); ++known; }
        }
        if (i < n60 && f.solo) ++soloN;
    }
    s.haveK = known > 0;
    if (s.haveK) {
        s.occupancy60s = static_cast<double>(occ60) / static_cast<double>(n60);
        s.occupancy10min = static_cast<double>(occAll) / static_cast<double>(nAll);
        s.meanKa60s = ka / static_cast<double>(known);
        s.meanKs60s = ks / static_cast<double>(known);
    }
    s.soloExposurePct = 100.0 * static_cast<double>(soloN) / static_cast<double>(n60);

    // gaps ending within the last 60 s
    std::vector<double> d;
    for (auto it = gaps_.rbegin(); it != gaps_.rend(); ++it) {
        if (count_ - 1 - it->endFrame >= w60_) break;
        d.push_back(it->seconds);
    }
    std::sort(d.begin(), d.end());
    s.gaps.count = d.size();
    if (!d.empty()) {
        s.gaps.medianSec = percentileSorted(d, 0.5);
        s.gaps.p95Sec = percentileSorted(d, 0.95);
        s.gaps.maxSec = d.back();
    }
    s.gaps.ratePerSec = static_cast<double>(d.size()) / (static_cast<double>(n60) / rate_);

    // broadband modulation depth (16 Hz low-passed envelope, last 10 s)
    double m = 0.0, v = 0.0;
    for (std::size_t i = 0; i < n10; ++i) m += lpRing_[(count_ - 1 - i) % cap_];
    m /= static_cast<double>(n10);
    for (std::size_t i = 0; i < n10; ++i) { const double x = lpRing_[(count_ - 1 - i) % cap_] - m; v += x * x; }
    s.modulationDepth10s = m > 0.0 ? std::sqrt(v / static_cast<double>(n10)) / m : 0.0;

    s.haveModSpectrum = broadband_.has();
    s.modBlocks = broadband_.numBlocks();
    if (s.haveModSpectrum) s.modBroadband = broadband_.average();
    for (std::size_t c = 0; c < octave_.size(); ++c)
        if (octave_[c].has()) s.modOctave[c] = octave_[c].average();
    return s;
}

ModulationSnapshot analyzeModulation(const float* const* planar, std::size_t numChannels, std::size_t nFrames,
                                     double fs) {
    ModulationAnalyzer an;
    const auto flen = static_cast<std::size_t>(std::llround(fs * 0.01));
    if (flen == 0) return {};
    for (std::size_t off = 0; off + flen <= nFrames; off += flen) {
        double p = 0.0;
        for (std::size_t c = 0; c < numChannels; ++c) {
            double s = 0.0;
            for (std::size_t i = 0; i < flen; ++i) { const double x = static_cast<double>(planar[c][off + i]); s += x * x; }
            p += s / static_cast<double>(flen);
        }
        an.pushFrame(p);
    }
    return an.snapshot();
}

}  // namespace bf
