#include "core/spectrum/FirDesigner.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "core/dsp/Fft.h"

namespace bf {

namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;

double lpCornerHz(const FirDesignParams& p) { return std::min(p.hfLimitHz, 0.45 * p.fs); }

// 10log10 |H_hp H_lp|^2 (2nd-order Butterworth HP, 4th-order Butterworth LP).
double rolloffDb(double f, const FirDesignParams& p) {
    const double r = f / p.lfLimitHz, r4 = r * r * r * r;
    const double q = f / lpCornerHz(p), q2 = q * q, q4 = q2 * q2, q8 = q4 * q4;
    return 10.0 * std::log10(r4 / (1.0 + r4)) - 10.0 * std::log10(1.0 + q8);
}

// Per-Hz density model (dB) with PCHIP nodes at the exact band centres in log2(f).
struct DensityModel {
    std::vector<double> x, v;
    double operator()(double f) const { return pchipEval(x, v, std::log2(f)); }
};

// Simpson integral of 10^(g(f)/10) over [a, b], integrated in log-frequency.
template <typename G>
double integratePower(double a, double b, const G& g) {
    if (b <= a) return 0.0;
    constexpr int kN = 64;  // even
    const double la = std::log(a), lb = std::log(b), dl = (lb - la) / kN;
    double s = 0.0;
    for (int i = 0; i <= kN; ++i) {
        const double f = std::exp(la + dl * i);
        const double w = (i == 0 || i == kN) ? 1.0 : ((i & 1) ? 4.0 : 2.0);
        s += w * std::pow(10.0, g(f) / 10.0) * f;  // df = f dlnf
    }
    return s * dl / 3.0;
}

DensityModel fitDensity(const ThirdOctArray& bandsDb) {
    DensityModel m;
    m.x.resize(kNumThirdOctBands);
    m.v.resize(kNumThirdOctBands);
    for (std::size_t k = 0; k < kNumThirdOctBands; ++k) {
        m.x[k] = std::log2(thirdOctCentreHz(k));
        const double bw = thirdOctUpperEdgeHz(k) - thirdOctLowerEdgeHz(k);
        m.v[k] = bandsDb[k] - 10.0 * std::log10(bw);
    }
    // Fixed-point refinement: band integral of density == band level.
    for (int it = 0; it < 12; ++it) {
        std::array<double, kNumThirdOctBands> err{};
        double maxErr = 0.0;
        for (std::size_t k = 0; k < kNumThirdOctBands; ++k) {
            const double pk = integratePower(thirdOctLowerEdgeHz(k), thirdOctUpperEdgeHz(k), m);
            err[k] = bandsDb[k] - 10.0 * std::log10(pk);
            maxErr = std::max(maxErr, std::fabs(err[k]));
        }
        for (std::size_t k = 0; k < kNumThirdOctBands; ++k) m.v[k] += err[k];
        if (maxErr < 1e-4) break;
    }
    return m;
}

std::size_t nextPow2(std::size_t n) {
    std::size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// Band power of h against unit white noise (one-sided PSD 2/fs), fractional-bin edges.
// Returns linear powers; also optionally band widths used.
std::array<double, kNumThirdOctBands> bandPowers(const std::vector<double>& h, double fs) {
    const std::size_t n = std::max<std::size_t>(65536, nextPow2(4 * h.size()));
    FftD fft(n);
    std::vector<double> x(n, 0.0);
    std::copy(h.begin(), h.end(), x.begin());
    std::vector<std::complex<double>> X(n / 2 + 1);
    fft.forward(x.data(), X.data());
    const double df = fs / static_cast<double>(n);
    std::array<double, kNumThirdOctBands> out{};
    for (std::size_t k = 0; k < kNumThirdOctBands; ++k) {
        const double lo = thirdOctLowerEdgeHz(k), hi = std::min(thirdOctUpperEdgeHz(k), fs / 2.0);
        if (hi <= lo) {
            out[k] = 1e-30;
            continue;
        }
        const auto i0 = static_cast<std::size_t>(std::floor(lo / df + 0.5));
        const auto i1 = std::min(static_cast<std::size_t>(std::floor(hi / df + 0.5)), n / 2);
        double s = 0.0;
        for (std::size_t i = i0; i <= i1; ++i) {
            const double a = std::max(lo, (static_cast<double>(i) - 0.5) * df);
            const double b = std::min(hi, (static_cast<double>(i) + 0.5) * df);
            if (b > a) s += std::norm(X[i]) * (b - a);
        }
        out[k] = std::max(s * 2.0 / fs, 1e-30);
    }
    return out;
}

std::vector<double> designAtLength(const DensityModel& dens, const FirDesignParams& p, std::size_t L) {
    const std::size_t n = 4 * L;  // step 1
    const std::size_t nb = n / 2 + 1;
    std::vector<double> dDb(nb);
    double peak = -1e300;
    for (std::size_t i = 1; i < nb; ++i) {  // step 2
        const double f = static_cast<double>(i) * p.fs / static_cast<double>(n);
        dDb[i] = dens(f) + rolloffDb(f, p);
        peak = std::max(peak, dDb[i]);
    }
    dDb[0] = dDb[1] - 60.0;
    FftD fft(n);
    std::vector<std::complex<double>> spec(nb);
    for (std::size_t i = 0; i < nb; ++i) {  // steps 3-4: log|D| (peak-normalised; shape only)
        const double mag = std::pow(10.0, (dDb[i] - peak) / 20.0);
        spec[i] = {std::log(std::max(mag, 1e-6)), 0.0};
    }
    std::vector<double> c(n);
    fft.inverse(spec.data(), c.data());
    for (std::size_t k = 1; k < n / 2; ++k) c[k] *= 2.0;  // fold
    for (std::size_t k = n / 2 + 1; k < n; ++k) c[k] = 0.0;
    fft.forward(c.data(), spec.data());
    for (auto& z : spec) {
        const double e = std::exp(z.real());
        z = {e * std::cos(z.imag()), e * std::sin(z.imag())};
    }
    std::vector<double> hFull(n);
    fft.inverse(spec.data(), hFull.data());
    std::vector<double> h(hFull.begin(), hFull.begin() + static_cast<std::ptrdiff_t>(L));  // step 5
    const std::size_t m = std::max<std::size_t>(1, L / 10);
    for (std::size_t j = 0; j < m; ++j)
        h[L - m + j] *= 0.5 * (1.0 + std::cos(kPi * (static_cast<double>(j) + 0.5) / static_cast<double>(m)));
    if (p.unitEnergy) {  // step 6
        double e = 0.0;
        for (double v : h) e += v * v;
        const double g = 1.0 / std::sqrt(e);
        for (double& v : h) v *= g;
    }
    return h;
}

}  // namespace

std::vector<float> FirDesignResult::tapsFloat() const {
    std::vector<float> r(h.size());
    for (std::size_t i = 0; i < h.size(); ++i) r[i] = static_cast<float>(h[i]);
    return r;
}

std::size_t defaultStationaryTaps(double fs) noexcept { return fs > 50000.0 ? 8192 : 4096; }
std::size_t defaultBabbleTaps(double fs) noexcept { return fs > 50000.0 ? 4096 : 2048; }

ThirdOctArray idealBandLevelsDb(const ThirdOctArray& bandsDb, const FirDesignParams& p) {
    const DensityModel dens = fitDensity(bandsDb);
    ThirdOctArray r{};
    for (std::size_t k = 0; k < kNumThirdOctBands; ++k) {
        const double hi = std::min(thirdOctUpperEdgeHz(k), p.fs / 2.0);
        const double pw = integratePower(thirdOctLowerEdgeHz(k), hi,
                                         [&](double f) { return dens(f) + rolloffDb(f, p); });
        r[k] = 10.0 * std::log10(std::max(pw, 1e-30));
    }
    return r;
}

ThirdOctArray firThirdOctResponseDb(const std::vector<double>& h, double fs) {
    const auto pw = bandPowers(h, fs);
    ThirdOctArray r{};
    for (std::size_t k = 0; k < kNumThirdOctBands; ++k) r[k] = 10.0 * std::log10(pw[k]);
    return r;
}

double maxShapeDeviationDb(const ThirdOctArray& a, const ThirdOctArray& b) noexcept {
    double mean = 0.0;
    for (std::size_t k = kFirstOperatingBand; k <= kLastOperatingBand; ++k) mean += a[k] - b[k];
    mean /= static_cast<double>(kNumOperatingBands);
    double mx = 0.0;
    for (std::size_t k = kFirstOperatingBand; k <= kLastOperatingBand; ++k)
        mx = std::max(mx, std::fabs(a[k] - b[k] - mean));
    return mx;
}

FirDesignResult designMinPhaseFir(const ThirdOctArray& bandsDb, const FirDesignParams& p) {
    FirDesignResult res;
    const DensityModel dens = fitDensity(bandsDb);
    res.referenceDb = idealBandLevelsDb(bandsDb, p);
    std::size_t L = p.taps ? p.taps : defaultStationaryTaps(p.fs);
    for (;;) {
        res.h = designAtLength(dens, p, L);
        res.taps = L;
        res.achievedDb = firThirdOctResponseDb(res.h, p.fs);  // step 7
        res.maxDeviationDb = maxShapeDeviationDb(res.achievedDb, res.referenceDb);
        if (res.maxDeviationDb <= p.toleranceDb) break;
        if (2 * L > p.maxTaps) {
            res.degraded = true;
            break;
        }
        L *= 2;
        ++res.doublings;
    }
    return res;
}

double normalizeForPoolLtass(std::vector<double>& h, double fs, const std::vector<double>& poolLtassDb) {
    const std::size_t first = poolLtassDb.size() == kNumOperatingBands ? kFirstOperatingBand : 0;
    const std::size_t count = std::min(poolLtassDb.size(), kNumThirdOctBands - first);
    const auto pw = bandPowers(h, fs);
    double num = 0.0, den = 0.0;
    for (std::size_t j = 0; j < count; ++j) {
        const std::size_t k = first + j;
        const double hi = std::min(thirdOctUpperEdgeHz(k), fs / 2.0);
        const double bw = hi - thirdOctLowerEdgeHz(k);
        if (bw <= 0.0) continue;
        const double gain = pw[k] / (bw * 2.0 / fs);  // mean |H|^2 in band
        const double pp = std::pow(10.0, poolLtassDb[j] / 10.0);
        num += pp;
        den += gain * pp;
    }
    if (den <= 0.0) return 1.0;
    const double g = std::sqrt(num / den);
    for (double& v : h) v *= g;
    return g;
}

ThirdOctArray babbleEqBandsDb(const ThirdOctArray& target, const ThirdOctArray& poolLtass,
                              const ThirdOctArray& correction, bool* clamped) noexcept {
    ThirdOctArray r{};
    bool hit = false;
    for (std::size_t k = 0; k < kNumThirdOctBands; ++k) {
        const double v = target[k] - poolLtass[k] + correction[k];
        r[k] = std::clamp(v, -12.0, 12.0);
        if (r[k] != v) hit = true;
    }
    if (clamped) *clamped = hit;
    return r;
}

}  // namespace bf
