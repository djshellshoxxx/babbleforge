#include "core/spectrum/SpectrumTarget.h"

#include <cmath>

namespace bf {

namespace {
constexpr std::array<double, kNumThirdOctBands> kNominal = {
    50,   63,   80,   100,  125,  160,  200,  250,  315,  400,  500,   630,   800,
    1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000};
constexpr std::array<double, kNumOctaveBands> kOctNominal = {125, 250, 500, 1000, 2000, 4000, 8000};

double pchipSlope(double h0, double h1, double d0, double d1) {
    if (d0 * d1 <= 0.0) return 0.0;
    const double w1 = 2.0 * h1 + h0, w2 = h1 + 2.0 * h0;
    return (w1 + w2) / (w1 / d0 + w2 / d1);
}
}  // namespace

const std::array<double, kNumThirdOctBands>& thirdOctNominalHz() noexcept { return kNominal; }
const std::array<double, kNumOctaveBands>& octaveNominalHz() noexcept { return kOctNominal; }

double thirdOctCentreHz(std::size_t band) noexcept {
    const int n = static_cast<int>(band) - 13;
    return 1000.0 * std::pow(10.0, n / 10.0);
}
double thirdOctLowerEdgeHz(std::size_t band) noexcept {
    return thirdOctCentreHz(band) * std::pow(10.0, -1.0 / 20.0);
}
double thirdOctUpperEdgeHz(std::size_t band) noexcept {
    return thirdOctCentreHz(band) * std::pow(10.0, 1.0 / 20.0);
}

int thirdOctBandIndexForHz(double f) noexcept {
    for (std::size_t i = 0; i < kNumThirdOctBands; ++i)
        if (std::fabs(f - kNominal[i]) <= 0.03 * kNominal[i]) return static_cast<int>(i);
    return -1;
}

OctaveArray octaveFromThirdOct(const ThirdOctArray& t) noexcept {
    OctaveArray o{};
    for (std::size_t k = 0; k < kNumOctaveBands; ++k) {
        const std::size_t c = 4 + 3 * k;  // 125 Hz is band 4
        double p = 0.0;
        for (std::size_t j = c - 1; j <= c + 1; ++j) p += std::pow(10.0, t[j] / 10.0);
        o[k] = 10.0 * std::log10(p);
    }
    return o;
}

ThirdOctArray SpectrumTarget::effectiveThirdOctDb() const noexcept {
    ThirdOctArray r{};
    for (std::size_t i = 0; i < kNumThirdOctBands; ++i) r[i] = thirdOctDb[i];
    for (std::size_t i = 3; i <= 5; ++i) r[i] += lfTrimDb125;
    return r;
}

double pchipEval(const std::vector<double>& x, const std::vector<double>& y, double xq) noexcept {
    const std::size_t n = x.size();
    if (n == 0) return 0.0;
    if (n == 1 || xq <= x.front()) return y.front();
    if (xq >= x.back()) return y.back();
    std::size_t i = 0;
    while (i + 2 < n && xq >= x[i + 1]) ++i;
    auto h = [&](std::size_t k) { return x[k + 1] - x[k]; };
    auto del = [&](std::size_t k) { return (y[k + 1] - y[k]) / h(k); };
    auto deriv = [&](std::size_t k) -> double {
        if (n == 2) return del(0);
        if (k == 0 || k == n - 1) {
            // scipy's one-sided three-point end condition with shape preservation
            const std::size_t a = (k == 0) ? 0 : n - 2, b = (k == 0) ? 1 : n - 3;
            const double h0 = h(a), h1 = h(b), d0 = del(a), d1 = del(b);
            double d = ((2.0 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
            if (d * d0 <= 0.0) d = 0.0;
            else if (d0 * d1 <= 0.0 && std::fabs(d) > 3.0 * std::fabs(d0)) d = 3.0 * d0;
            return d;
        }
        return pchipSlope(h(k - 1), h(k), del(k - 1), del(k));
    };
    const double hi = h(i), t = (xq - x[i]) / hi;
    const double m0 = deriv(i), m1 = deriv(i + 1);
    const double t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * y[i] + (t3 - 2 * t2 + t) * hi * m0 + (-2 * t3 + 3 * t2) * y[i + 1] +
           (t3 - t2) * hi * m1;
}

SpectrumTargetBuild buildSpectrumTarget(const SpectrumTargetDef& def, float lfTrimDb125) {
    SpectrumTargetBuild r;
    const auto& f = def.bandCentersHz;
    const auto& v = def.levelsDb;
    if (f.size() != v.size() || f.size() < 2) {
        r.error = "target '" + def.id + "': band/level count mismatch or too few bands";
        return r;
    }
    for (double x : v)
        if (!std::isfinite(x) || std::fabs(x) > 40.0) {
            r.error = "target '" + def.id + "': level NaN or outside +-40 dB";
            return r;
        }
    std::vector<double> xs, ys;
    const bool octave = f.size() == kNumOctaveBands &&
                        [&] {
                            for (std::size_t i = 0; i < f.size(); ++i)
                                if (std::fabs(f[i] - kOctNominal[i]) > 0.03 * kOctNominal[i]) return false;
                            return true;
                        }();
    if (octave) {
        for (std::size_t i = 0; i < f.size(); ++i) {
            xs.push_back(std::log2(kOctNominal[i]));
            ys.push_back(v[i]);
        }
    } else {
        int prev = -1;
        for (std::size_t i = 0; i < f.size(); ++i) {
            const int b = thirdOctBandIndexForHz(f[i]);
            if (b < 0 || b <= prev) {
                r.error = "target '" + def.id + "': band centres must be increasing IEC nominal values";
                return r;
            }
            prev = b;
            xs.push_back(std::log2(thirdOctCentreHz(static_cast<std::size_t>(b))));
            ys.push_back(v[i]);
        }
    }
    SpectrumTarget t;
    t.id = def.id;
    t.lfTrimDb125 = lfTrimDb125;
    ThirdOctArray lv{};
    for (std::size_t b = 0; b < kNumThirdOctBands; ++b)
        lv[b] = pchipEval(xs, ys, std::log2(octave ? kNominal[b] : thirdOctCentreHz(b)));
    const double ref = lv[kBand1k];
    for (std::size_t b = 0; b < kNumThirdOctBands; ++b) t.thirdOctDb[b] = static_cast<float>(lv[b] - ref);
    r.target = std::move(t);
    return r;
}

}  // namespace bf
