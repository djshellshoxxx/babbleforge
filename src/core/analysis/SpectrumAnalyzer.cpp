#include "core/analysis/SpectrumAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace bf {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

OperatingBands operatingSlice(const ThirdOctArray& a) noexcept {
    OperatingBands r{};
    for (std::size_t i = 0; i < kNumOperatingBands; ++i) r[i] = a[kFirstOperatingBand + i];
    return r;
}

double powerToDb(double p) noexcept { return 10.0 * std::log10(std::max(p, 1e-30)); }

ThirdOctArray powerToDb(const ThirdOctArray& p) noexcept {
    ThirdOctArray r{};
    for (std::size_t i = 0; i < p.size(); ++i) r[i] = powerToDb(p[i]);
    return r;
}

OctaveArray octaveDbFromPower(const ThirdOctArray& p) noexcept { return octaveFromThirdOct(powerToDb(p)); }

void SpectrumAnalyzer::prepare(double fs, std::size_t numChannels) {
    fs_ = fs > 0.0 ? fs : 48000.0;
    nCh_ = std::max<std::size_t>(1, numChannels);
    std::size_t n = 8192;
    const double target = 8192.0 * fs_ / 48000.0;
    while (static_cast<double>(n) < target * 0.999 && n < (1u << 20)) n *= 2;
    n_ = n;
    hop_ = n_ / 2;
    fft_ = std::make_unique<FftD>(n_);
    window_.assign(n_, 0.0);
    double sw2 = 0.0;
    for (std::size_t i = 0; i < n_; ++i) {
        window_[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n_));
        sw2 += window_[i] * window_[i];
    }
    // one-sided mean-square per bin = 2 |X|^2 / (N sum w^2)
    norm_ = 2.0 / (static_cast<double>(n_) * sw2);
    work_.assign(n_, 0.0);
    spec_.assign(n_ / 2 + 1, {});
    binPower_.assign(n_ / 2 + 1, 0.0);
    const double df = fs_ / static_cast<double>(n_);
    for (std::size_t b = 0; b < kNumThirdOctBands; ++b) {
        auto& ws = weights_[b];
        ws.clear();
        const double lo = thirdOctLowerEdgeHz(b), hi = std::min(thirdOctUpperEdgeHz(b), fs_ / 2.0);
        if (hi <= lo) continue;
        for (auto k = static_cast<std::size_t>(std::max(0.0, std::floor(lo / df - 0.5)));
             k <= n_ / 2 && (static_cast<double>(k) - 0.5) * df < hi; ++k) {
            const double a = std::max(lo, (static_cast<double>(k) - 0.5) * df);
            const double e = std::min(hi, (static_cast<double>(k) + 0.5) * df);
            if (e > a) ws.push_back({static_cast<std::uint32_t>(k), (e - a) / df});
        }
    }
    hopsPerBlock_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(kBlockSeconds * fs_ / static_cast<double>(hop_))));
    const double hopSec = static_cast<double>(hop_) / fs_;
    shortAlpha_ = 1.0 - std::exp(-hopSec / kShortTauSeconds);
    longAlpha_ = 1.0 - std::exp(-(static_cast<double>(hopsPerBlock_) * hopSec) / kLongTauSeconds);
    reset();
}

void SpectrumAnalyzer::reset() {
    buf_.assign(nCh_, std::vector<double>(n_, 0.0));
    fill_ = 0;
    samples_ = 0;
    hopCount_ = blockHops_ = 0;
    shortP_.fill(0.0);
    longP_.fill(0.0);
    blockAcc_.fill(0.0);
    overallAcc_.fill(0.0);
    haveLong_ = false;
    blocks_.clear();
}

template <typename T>
void SpectrumAnalyzer::processImpl(const T* const* in, std::size_t nFrames) {
    std::size_t pos = 0;
    while (pos < nFrames) {
        const std::size_t take = std::min(nFrames - pos, n_ - fill_);
        for (std::size_t c = 0; c < nCh_; ++c) {
            double* dst = buf_[c].data() + fill_;
            const T* src = in[c] + pos;
            for (std::size_t i = 0; i < take; ++i) dst[i] = static_cast<double>(src[i]);
        }
        fill_ += take;
        pos += take;
        samples_ += take;
        if (fill_ == n_) {
            analyseHop();
            for (std::size_t c = 0; c < nCh_; ++c)
                std::copy(buf_[c].begin() + static_cast<std::ptrdiff_t>(hop_), buf_[c].end(), buf_[c].begin());
            fill_ = hop_;
        }
    }
}

void SpectrumAnalyzer::process(const float* const* in, std::size_t nFrames) { processImpl(in, nFrames); }
void SpectrumAnalyzer::process(const double* const* in, std::size_t nFrames) { processImpl(in, nFrames); }

void SpectrumAnalyzer::analyseHop() {
    std::fill(binPower_.begin(), binPower_.end(), 0.0);
    for (std::size_t c = 0; c < nCh_; ++c) {
        for (std::size_t i = 0; i < n_; ++i) work_[i] = window_[i] * buf_[c][i];
        fft_->forward(work_.data(), spec_.data());
        for (std::size_t k = 0; k <= n_ / 2; ++k) binPower_[k] += norm_ * std::norm(spec_[k]);
    }
    ThirdOctArray p{};
    for (std::size_t b = 0; b < kNumThirdOctBands; ++b) {
        double s = 0.0;
        for (const auto& w : weights_[b]) s += w.w * binPower_[w.bin];
        p[b] = s;
    }
    for (std::size_t b = 0; b < kNumThirdOctBands; ++b) {
        shortP_[b] = hopCount_ == 0 ? p[b] : shortP_[b] + shortAlpha_ * (p[b] - shortP_[b]);
        blockAcc_[b] += p[b];
        overallAcc_[b] += p[b];
    }
    ++hopCount_;
    if (++blockHops_ >= hopsPerBlock_) {
        SpectrumBlock blk;
        for (std::size_t b = 0; b < kNumThirdOctBands; ++b) blk.powerLin[b] = blockAcc_[b] / static_cast<double>(blockHops_);
        // window of the last hop ends at (hopCount + 1) * hop samples
        blk.endSeconds = static_cast<double>(samples_) / fs_;
        for (std::size_t b = 0; b < kNumThirdOctBands; ++b)
            longP_[b] = haveLong_ ? longP_[b] + longAlpha_ * (blk.powerLin[b] - longP_[b]) : blk.powerLin[b];
        haveLong_ = true;
        blocks_.push_back(blk);
        blockAcc_.fill(0.0);
        blockHops_ = 0;
    }
}

std::vector<SpectrumBlock> SpectrumAnalyzer::takeBlocks() {
    std::vector<SpectrumBlock> r;
    r.swap(blocks_);
    return r;
}

ThirdOctArray SpectrumAnalyzer::overallPower() const noexcept {
    ThirdOctArray r{};
    if (hopCount_ == 0) return r;
    for (std::size_t b = 0; b < r.size(); ++b) r[b] = overallAcc_[b] / static_cast<double>(hopCount_);
    return r;
}

// ---- metrics ---------------------------------------------------------------------------------

void shapeDeviation(const OperatingBands& measuredDb, const OperatingBands& targetDb, OperatingBands& d,
                    double* mu) noexcept {
    double sw = 0.0, sd = 0.0;
    OperatingBands off{};
    for (std::size_t i = 0; i < kNumOperatingBands; ++i) {
        off[i] = measuredDb[i] - targetDb[i];
        const double w = std::pow(10.0, targetDb[i] / 10.0);
        sw += w;
        sd += w * off[i];
    }
    const double m = sw > 0.0 ? sd / sw : 0.0;
    for (std::size_t i = 0; i < kNumOperatingBands; ++i) d[i] = off[i] - m;
    if (mu) *mu = m;
}

double leastSquaresSlopeDbPerOct(const ThirdOctArray& levelsDb, std::size_t first, std::size_t last) noexcept {
    double sx = 0, sy = 0, sxx = 0, sxy = 0, n = 0;
    for (std::size_t b = first; b <= last && b < levelsDb.size(); ++b) {
        const double x = std::log2(thirdOctCentreHz(b));
        sx += x; sy += levelsDb[b]; sxx += x * x; sxy += x * levelsDb[b]; n += 1;
    }
    const double den = n * sxx - sx * sx;
    return den > 0.0 ? (n * sxy - sx * sy) / den : 0.0;
}

ShapeMetrics computeShapeMetrics(const ThirdOctArray& p, const ThirdOctArray& targetDb) noexcept {
    ShapeMetrics m;
    const ThirdOctArray db = powerToDb(p);
    shapeDeviation(operatingSlice(db), operatingSlice(targetDb), m.d, &m.mu);
    double ss = 0.0, cnt = 0.0;
    for (std::size_t i = 0; i < kNumOperatingBands; ++i) {
        const std::size_t b = kFirstOperatingBand + i;
        if (b >= 4 && b <= 22) { ss += m.d[i] * m.d[i]; cnt += 1.0; }  // 125 Hz .. 8 kHz
        if (std::fabs(m.d[i]) > m.maxAbsDev) { m.maxAbsDev = std::fabs(m.d[i]); m.maxAbsBand = b; }
    }
    m.rmsDev125to8k = cnt > 0 ? std::sqrt(ss / cnt) : 0.0;
    m.slopeDbPerOct = leastSquaresSlopeDbPerOct(db, 7, 19);  // 250 Hz .. 4 kHz
    double total = 0.0, lf = 0.0, hf = 0.0, sp = 0.0;
    for (std::size_t b = 0; b < kNumThirdOctBands; ++b) {
        total += p[b];
        if (b >= 3 && b <= 6) lf += p[b];    // 100..200 Hz
        if (b >= 19 && b <= 23) hf += p[b];  // 4..10 kHz
        if (b >= 6 && b <= 20) sp += p[b];   // 200 Hz..5 kHz
    }
    if (total > 0.0) { m.lfFraction = lf / total; m.hfFraction = hf / total; m.speechFraction = sp / total; }
    return m;
}

SpectrumAnalysis analyzeSpectrum(const float* const* planar, std::size_t numChannels, std::size_t nFrames,
                                 double fs, const ThirdOctArray* targetDb) {
    SpectrumAnalyzer an;
    an.prepare(fs, numChannels);
    an.process(planar, nFrames);
    SpectrumAnalysis r;
    r.durationSeconds = fs > 0 ? static_cast<double>(nFrames) / fs : 0.0;
    r.overallPowerLin = an.overallPower();
    r.overallDb = powerToDb(r.overallPowerLin);
    r.octaveDb = octaveFromThirdOct(r.overallDb);
    r.blocks = an.takeBlocks();
    r.hasLongTerm = an.hasLongTerm();
    if (r.hasLongTerm) r.longTermDb = powerToDb(an.longTermPower());
    if (targetDb && an.numHops() > 0) {
        r.hasShape = true;
        r.shape = computeShapeMetrics(r.overallPowerLin, *targetDb);
    }
    return r;
}

}  // namespace bf
