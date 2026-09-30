#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <complex>
#include <vector>

#include "core/config/DataSet.h"
#include "core/dsp/Fft.h"
#include "core/spectrum/FirDesigner.h"
#include "core/spectrum/SpectrumTarget.h"

using namespace bf;
using Catch::Matchers::WithinAbs;

TEST_CASE("IEC 61260 third-octave centres and edges", "[spectrum]") {
    CHECK_THAT(thirdOctCentreHz(kBand1k), WithinAbs(1000.0, 1e-9));
    CHECK_THAT(thirdOctCentreHz(0), WithinAbs(50.1187, 1e-3));
    CHECK_THAT(thirdOctCentreHz(25), WithinAbs(15848.93, 1e-2));
    for (std::size_t b = 0; b + 1 < kNumThirdOctBands; ++b)
        CHECK_THAT(thirdOctUpperEdgeHz(b), WithinAbs(thirdOctLowerEdgeHz(b + 1), 1e-9));
    CHECK_THAT(thirdOctLowerEdgeHz(kBand1k), WithinAbs(891.2509, 1e-3));
    CHECK(thirdOctNominalHz()[kFirstOperatingBand] == 100.0);
    CHECK(thirdOctNominalHz()[kLastOperatingBand] == 10000.0);
    CHECK(thirdOctBandIndexForHz(6300) == 21);
    CHECK(thirdOctBandIndexForHz(700) == -1);
}

TEST_CASE("Octave derivation by power sum", "[spectrum]") {
    ThirdOctArray t{};
    t.fill(0.0);
    auto o = octaveFromThirdOct(t);
    for (double v : o) CHECK_THAT(v, WithinAbs(10.0 * std::log10(3.0), 1e-12));
    t[4] = 10.0;  // 125 Hz
    o = octaveFromThirdOct(t);
    CHECK_THAT(o[0], WithinAbs(10.0 * std::log10(12.0), 1e-12));
}

TEST_CASE("Build 26-band target from 21-band def with flat extrapolation and LF trim", "[spectrum]") {
    SpectrumTargetDef d;
    d.id = "t21";
    for (std::size_t b = kFirstOperatingBand; b <= kLastOperatingBand; ++b) {
        d.bandCentersHz.push_back(thirdOctNominalHz()[b]);
        d.levelsDb.push_back(-static_cast<double>(b));
    }
    auto r = buildSpectrumTarget(d, 2.0f);
    REQUIRE(r.target);
    const auto& t = *r.target;
    CHECK(t.thirdOctDb[kBand1k] == 0.0f);
    CHECK(t.thirdOctDb[0] == t.thirdOctDb[3]);
    CHECK(t.thirdOctDb[1] == t.thirdOctDb[3]);
    CHECK(t.thirdOctDb[25] == t.thirdOctDb[23]);
    CHECK(t.thirdOctDb[24] == t.thirdOctDb[23]);
    CHECK_THAT(t.thirdOctDb[10], WithinAbs(3.0, 1e-6));  // -10 - (-13)
    const auto e = t.effectiveThirdOctDb();
    CHECK_THAT(e[3] - t.thirdOctDb[3], WithinAbs(2.0, 1e-6));
    CHECK_THAT(e[5] - t.thirdOctDb[5], WithinAbs(2.0, 1e-6));
    CHECK_THAT(e[6] - t.thirdOctDb[6], WithinAbs(0.0, 1e-6));

    SpectrumTargetDef bad = d;
    bad.levelsDb[3] = std::nan("");
    CHECK_FALSE(buildSpectrumTarget(bad).target);
    SpectrumTargetDef oct;
    oct.bandCentersHz = {125, 250, 500, 1000, 2000, 4000, 8000};
    oct.levelsDb = {0, 0, 0, 0, -6, -12, -18};
    auto ro = buildSpectrumTarget(oct);
    REQUIRE(ro.target);
    CHECK_THAT(ro.target->thirdOctDb[16], WithinAbs(-6.0, 1e-6));  // 2 kHz node
    CHECK(ro.target->thirdOctDb[17] < -6.0f);
    CHECK(ro.target->thirdOctDb[17] > -12.0f);
}

namespace {
// Real cepstrum based minimum-phase reconstruction of h from |FFT(h)|; a minimum-phase h
// has a causal complex cepstrum, which makes h equal to this reconstruction.
double minPhaseReconstructionError(const std::vector<double>& h) {
    std::size_t n = 1;
    while (n < 8 * h.size()) n <<= 1;
    FftD fft(n);
    std::vector<double> x(n, 0.0), c(n);
    std::copy(h.begin(), h.end(), x.begin());
    std::vector<std::complex<double>> X(n / 2 + 1);
    fft.forward(x.data(), X.data());
    double peak = 0.0;
    for (auto& z : X) peak = std::max(peak, std::abs(z));
    for (auto& z : X) z = {std::log(std::max(std::abs(z), 1e-7 * peak)), 0.0};
    fft.inverse(X.data(), c.data());
    for (std::size_t k = 1; k < n / 2; ++k) c[k] *= 2.0;
    for (std::size_t k = n / 2 + 1; k < n; ++k) c[k] = 0.0;
    fft.forward(c.data(), X.data());
    for (auto& z : X) z = std::exp(z);
    fft.inverse(X.data(), x.data());
    double e = 0.0, d = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double ref = i < h.size() ? h[i] : 0.0;
        e += ref * ref;
        d += (x[i] - ref) * (x[i] - ref);
    }
    return d / e;
}
}  // namespace

TEST_CASE("Stationary FIR design for every data-set target and sample rate", "[spectrum][fir]") {
    auto ds = loadDataSet(BF_DATA_DIR);
    REQUIRE(ds.ok);
    REQUIRE(ds.data.targets.size() >= 6);
    double worst = 0.0;
    for (const auto& [id, def] : ds.data.targets) {
        auto bt = buildSpectrumTarget(def);
        REQUIRE(bt.target);
        const ThirdOctArray bands = bt.target->effectiveThirdOctDb();
        for (double fs : {44100.0, 48000.0, 88200.0, 96000.0}) {
            FirDesignParams p;
            p.fs = fs;
            const auto r = designMinPhaseFir(bands, p);
            INFO("target " << id << " fs " << fs << " taps " << r.taps << " dev " << r.maxDeviationDb);
            CHECK_FALSE(r.degraded);
            CHECK(r.h.size() >= defaultStationaryTaps(fs));
            // Independent check of the 1/3-oct response (analytic |H|^2 band integration)
            // against the target with the specified LF/HF limit roll-offs (§4.3 step 2).
            const auto achieved = firThirdOctResponseDb(r.h, fs);
            const double dev = maxShapeDeviationDb(achieved, r.referenceDb);
            worst = std::max(worst, dev);
            CHECK(dev <= 0.5);
            // Away from the roll-offs (315 Hz..5 kHz) the reference reproduces the target band levels.
            double mean = 0.0;
            for (std::size_t b = 8; b <= 20; ++b) mean += r.referenceDb[b] - bands[b];
            mean /= 13.0;
            for (std::size_t b = 8; b <= 20; ++b) CHECK(std::fabs(r.referenceDb[b] - bands[b] - mean) < 0.05);
            // Unit energy.
            double e = 0.0, eHead = 0.0;
            for (std::size_t i = 0; i < r.h.size(); ++i) {
                e += r.h[i] * r.h[i];
                if (i < r.h.size() / 4) eHead += r.h[i] * r.h[i];
            }
            CHECK_THAT(e, WithinAbs(1.0, 1e-6));
            // Minimum phase: energy concentrated at the start and causal cepstrum.
            CHECK(eHead / e >= 0.90);
            const double mpErr = minPhaseReconstructionError(r.h);
            INFO("min-phase reconstruction rel. error " << mpErr);
            // Residual comes only from truncation + taper (step 5); a non-minimum-phase
            // filter with the same magnitude (e.g. time-reversed) gives an O(1) error.
            CHECK(mpErr < 5e-3);
            if (fs == 48000.0) {
                std::vector<double> rev(r.h.rbegin(), r.h.rend());
                CHECK(minPhaseReconstructionError(rev) > 0.5);
            }
        }
    }
    INFO("worst deviation " << worst);
    SUCCEED();
}

TEST_CASE("80 Hz LF limit attenuates the 125 Hz band by less than 1 dB", "[spectrum][fir]") {
    ThirdOctArray flat{};
    flat.fill(0.0);
    FirDesignParams p;
    p.fs = 48000.0;
    p.lfLimitHz = 80.0;
    const auto a = designMinPhaseFir(flat, p);
    p.lfLimitHz = 10.0;
    const auto b = designMinPhaseFir(flat, p);
    const double att125 = (b.achievedDb[4] - b.achievedDb[kBand1k]) - (a.achievedDb[4] - a.achievedDb[kBand1k]);
    INFO("125 Hz band attenuation " << att125 << " dB");
    CHECK(att125 > 0.0);
    CHECK(att125 < 1.0);
    // And the HP really acts: 50 Hz band clearly attenuated.
    const double att50 = (b.achievedDb[0] - b.achievedDb[kBand1k]) - (a.achievedDb[0] - a.achievedDb[kBand1k]);
    CHECK(att50 > 3.0);
}

TEST_CASE("Babble kernel normalisation to pool LTASS", "[spectrum][fir]") {
    ThirdOctArray target{}, pool{}, corr{};
    for (std::size_t b = 0; b < kNumThirdOctBands; ++b) {
        target[b] = -5.0 * std::log2(thirdOctCentreHz(b) / 1000.0);
        pool[b] = -3.0 * std::log2(thirdOctCentreHz(b) / 1000.0) + (b % 3 == 0 ? 1.0 : 0.0);
    }
    bool clamped = true;
    const auto eq = babbleEqBandsDb(target, pool, corr, &clamped);
    CHECK_FALSE(clamped);
    FirDesignParams p;
    p.fs = 48000.0;
    p.taps = defaultBabbleTaps(p.fs);
    p.unitEnergy = false;
    auto r = designMinPhaseFir(eq, p);
    CHECK(r.taps == 2048);
    std::vector<double> pool21(pool.begin() + kFirstOperatingBand, pool.begin() + kLastOperatingBand + 1);
    normalizeForPoolLtass(r.h, p.fs, pool21);
    const auto resp = firThirdOctResponseDb(r.h, p.fs);
    double num = 0.0, den = 0.0;
    for (std::size_t b = kFirstOperatingBand; b <= kLastOperatingBand; ++b) {
        const double bw = thirdOctUpperEdgeHz(b) - thirdOctLowerEdgeHz(b);
        const double g = std::pow(10.0, resp[b] / 10.0) / (bw * 2.0 / p.fs);
        const double pp = std::pow(10.0, pool[b] / 10.0);
        num += pp;
        den += g * pp;
    }
    CHECK_THAT(den / num, WithinAbs(1.0, 1e-9));

    ThirdOctArray big = corr;
    big[10] = 30.0;
    babbleEqBandsDb(target, pool, big, &clamped);
    CHECK(clamped);
    CHECK(defaultBabbleTaps(96000.0) == 4096);
    CHECK(defaultStationaryTaps(88200.0) == 8192);
}
