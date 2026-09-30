#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/analysis/SpectralCorrection.h"

using namespace bf;

namespace {
OperatingBands flat() { OperatingBands t{}; t.fill(0.0); return t; }

// Closed loop: measured = plant offset + applied correction (the babble EQ acts on the estimate
// instantly; the 60 s estimator lag is exercised by the time constants, not modelled here).
struct Loop {
    SpectralCorrection sc;
    OperatingBands plant{};
    OperatingBands target = flat();
    explicit Loop(const CorrectionConfig& c = {}) : sc(c) {}
    CorrectionStepResult run(const CorrectionFreeze& f = {}) {
        OperatingBands meas{};
        for (std::size_t i = 0; i < meas.size(); ++i) meas[i] = plant[i] + sc.correction()[i];
        return sc.step(meas, target, f);
    }
    double shapeErr() const {
        OperatingBands meas{}, d{};
        for (std::size_t i = 0; i < meas.size(); ++i) meas[i] = plant[i] + sc.correction()[i];
        shapeDeviation(meas, target, d);
        double m = 0;
        for (double v : d) m = std::max(m, std::fabs(v));
        return m;
    }
};

OperatingBands bump(double db) {
    OperatingBands p{};
    for (std::size_t i = 0; i < p.size(); ++i) p[i] = (i >= 3 && i <= 8) ? db : 0.0;  // 160 Hz .. 500 Hz
    return p;
}
}  // namespace

TEST_CASE("SpectralCorrection: parameters by speed", "[correction]") {
    CorrectionConfig c;
    c.speed = CorrectionSpeed::Slow; REQUIRE(c.tauSeconds() == 300.0); REQUIRE(c.slewDbPerMin() == 0.5);
    c.speed = CorrectionSpeed::Normal; REQUIRE(c.tauSeconds() == 120.0);
    c.speed = CorrectionSpeed::Fast; REQUIRE(c.tauSeconds() == 45.0); REQUIRE(c.slewDbPerMin() == 1.0);
    c.strict = true; REQUIRE(c.tauSeconds() == 20.0);
}

TEST_CASE("SpectralCorrection: converges from +-3 dB offsets within slew, clamp, deadband", "[correction]") {
    for (double sign : {1.0, -1.0}) {
        Loop l;
        l.plant = bump(3.0 * sign);
        const double slew = 0.5 * 5.0 / 60.0;
        const double e0 = l.shapeErr();
        REQUIRE(e0 > 2.0);
        double maxStep = 0.0, elapsed = 0.0, errAt6min = 0.0;
        bool anyRedesign = false;
        for (int k = 0; k < 240; ++k) {  // 20 min
            const auto before = l.sc.correction();
            const auto r = l.run();
            anyRedesign |= r.redesignNeeded;
            for (std::size_t i = 0; i < before.size(); ++i) {
                maxStep = std::max(maxStep, std::fabs(r.correction[i] - before[i]));
                REQUIRE(std::fabs(r.correction[i]) <= 6.0 + 1e-12);
            }
            elapsed += 5.0;
            if (k == 71) errAt6min = l.shapeErr();
        }
        // slew bound (plus the tiny re-centring shift)
        REQUIRE(maxStep <= slew * 1.5 + 1e-9);
        REQUIRE(anyRedesign);
        // moves the right way and ends up within the deadband + smoothing residual
        REQUIRE(errAt6min < e0);
        REQUIRE(l.shapeErr() < 1.0);
        // sign: measured too loud -> negative correction
        REQUIRE(l.sc.correction()[5] * sign < 0.0);
    }
}

TEST_CASE("SpectralCorrection: fast speed converges faster", "[correction]") {
    CorrectionConfig f; f.speed = CorrectionSpeed::Fast;
    Loop a(f), b;
    a.plant = b.plant = bump(3.0);
    for (int k = 0; k < 60; ++k) { a.run(); b.run(); }  // 5 min
    REQUIRE(a.shapeErr() < b.shapeErr());
    REQUIRE(a.shapeErr() < 1.0);
}

TEST_CASE("SpectralCorrection: deadband and clamp", "[correction]") {
    SECTION("deviation below the deadband is ignored") {
        Loop l;
        l.plant = bump(0.25);  // smoothed |d| < 0.3
        for (int k = 0; k < 100; ++k) l.run();
        for (double c : l.sc.correction()) REQUIRE(c == 0.0);
    }
    SECTION("a persistent 12 dB error saturates at +-6 dB and raises the large flag") {
        Loop l;
        CorrectionConfig f; f.speed = CorrectionSpeed::Fast; l.sc.setConfig(f);
        l.plant = bump(12.0);
        // plant is independent of C (broken estimate): measured stays 12 dB off
        CorrectionStepResult r;
        for (int k = 0; k < 400; ++k) {
            OperatingBands meas = l.plant;
            r = l.sc.step(meas, l.target);
        }
        double mx = 0;
        for (double c : r.correction) mx = std::max(mx, std::fabs(c));
        REQUIRE(mx <= 6.0 + 1e-12);
        REQUIRE(mx > 4.5);
        REQUIRE(r.large);
    }
    SECTION("correction has zero power-weighted mean") {
        Loop l;
        l.plant = bump(3.0);
        for (int k = 0; k < 100; ++k) l.run();
        double s = 0;
        for (double c : l.sc.correction()) s += c;  // flat target -> equal weights
        REQUIRE(std::fabs(s / 21.0) < 1e-9);
    }
}

TEST_CASE("SpectralCorrection: freeze conditions and plan-change hold-off", "[correction]") {
    Loop l;
    l.plant = bump(3.0);
    for (int k = 0; k < 20; ++k) l.run();
    const auto c0 = l.sc.correction();
    bool moved = false;
    for (std::size_t i = 0; i < c0.size(); ++i) moved |= c0[i] != 0.0;
    REQUIRE(moved);

    for (int which = 0; which < 3; ++which) {
        CorrectionFreeze f;
        f.lowLevel = which == 0;
        f.lowBabbleFraction = which == 1;
        f.crossfading = which == 2;
        for (int k = 0; k < 10; ++k) {
            const auto r = l.run(f);
            REQUIRE(r.frozen);
            REQUIRE(r.correction == c0);
        }
    }
    // hold-off: 120 s (24 blocks) after a plan change
    l.sc.notifyPlanChange();
    for (int k = 0; k < 24; ++k) {
        const auto r = l.run();
        REQUIRE(r.frozen);
        REQUIRE(r.correction == c0);
    }
    const auto r = l.run();
    REQUIRE_FALSE(r.frozen);
    REQUIRE(r.correction != c0);
}

TEST_CASE("SpectralCorrection: oscillation guard", "[correction]") {
    Loop l;
    std::uint32_t triggered = 0, active = 0;
    // alternating-sign deviation around 600 Hz .. 1.6 kHz every block
    for (int k = 0; k < 40; ++k) {
        OperatingBands meas{};
        const double s = (k % 2 == 0) ? 4.0 : -4.0;
        for (std::size_t i = 8; i <= 13; ++i) meas[i] = s;
        const auto r = l.sc.step(meas, l.target);
        triggered |= r.guardTriggered;
        active |= r.guardActive;
    }
    REQUIRE(triggered != 0);
    REQUIRE((active & triggered) != 0);
    // a well behaved loop never trips the guard
    Loop calm;
    calm.plant = bump(3.0);
    std::uint32_t t2 = 0;
    for (int k = 0; k < 240; ++k) t2 |= calm.run().guardTriggered;
    REQUIRE(t2 == 0);
}

TEST_CASE("SpectralCorrection: redesign flag spacing, reset, initial value", "[correction]") {
    Loop l;
    l.plant = bump(3.0);
    std::vector<double> times;
    for (int k = 0; k < 120; ++k) {
        if (l.run().redesignNeeded) times.push_back(l.sc.timeSeconds());
    }
    REQUIRE(times.size() >= 2);
    for (std::size_t i = 1; i < times.size(); ++i) REQUIRE(times[i] - times[i - 1] >= 15.0);
    // the flag means a change >= 0.1 dB since the previous redesign
    l.sc.reset();
    for (double c : l.sc.correction()) REQUIRE(c == 0.0);
    REQUIRE(l.sc.timeSeconds() == 0.0);

    OperatingBands init{};
    init[2] = 9.0; init[5] = -1.0;
    l.sc.setInitial(init);
    REQUIRE(l.sc.correction()[2] == 6.0);
    REQUIRE(l.sc.correction()[5] == -1.0);
}

TEST_CASE("SpectralCorrection: strict mode (tau 20 s) converges within a 120 s pre-roll", "[correction]") {
    CorrectionConfig c; c.strict = true;
    Loop l(c);
    l.plant = bump(3.0);
    for (int k = 0; k < 24; ++k) l.run();
    REQUIRE(l.shapeErr() < 1.0);
}
