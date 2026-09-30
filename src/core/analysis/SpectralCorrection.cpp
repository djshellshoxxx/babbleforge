#include "core/analysis/SpectralCorrection.h"

#include <algorithm>
#include <cmath>

namespace bf {

double CorrectionConfig::tauSeconds() const noexcept {
    if (strict) return 20.0;
    switch (speed) {
        case CorrectionSpeed::Slow: return 300.0;
        case CorrectionSpeed::Normal: return 120.0;
        case CorrectionSpeed::Fast: return 45.0;
    }
    return 120.0;
}

double CorrectionConfig::slewDbPerMin() const noexcept {
    if (strict) return 3.0;
    return speed == CorrectionSpeed::Fast ? 1.0 : 0.5;
}

void SpectralCorrection::reset() noexcept {
    c_.fill(0.0);
    cDesigned_.fill(0.0);
    t_ = 0.0;
    lastDesignT_ = -1e30;
    holdoffRemaining_ = 0.0;
    band_ = {};
}

void SpectralCorrection::setInitial(const OperatingBands& c) noexcept {
    reset();
    for (std::size_t i = 0; i < c.size(); ++i) c_[i] = std::clamp(c[i], -cfg_.clampDb, cfg_.clampDb);
    cDesigned_ = c_;
}

CorrectionStepResult SpectralCorrection::step(const OperatingBands& measuredDb, const OperatingBands& targetDb,
                                              const CorrectionFreeze& freeze) noexcept {
    constexpr std::size_t N = kNumOperatingBands;
    CorrectionStepResult res;
    t_ += cfg_.blockSeconds;
    const bool held = holdoffRemaining_ > 0.0;
    if (held) holdoffRemaining_ = std::max(0.0, holdoffRemaining_ - cfg_.blockSeconds);
    res.frozen = freeze.any() || held;

    std::array<double, N> w{};
    double wsum = 0.0;
    for (std::size_t i = 0; i < N; ++i) { w[i] = std::pow(10.0, targetDb[i] / 10.0); wsum += w[i]; }

    if (!res.frozen) {
        // 1. shape deviation (mean removed), 2. smoothing, 3. deadband
        OperatingBands d{};
        shapeDeviation(measuredDb, targetDb, d);
        std::array<double, N> e{};
        for (std::size_t i = 0; i < N; ++i) {
            const double lo = d[i > 0 ? i - 1 : 1], hi = d[i + 1 < N ? i + 1 : N - 2];
            const double s = 0.25 * lo + 0.5 * d[i] + 0.25 * hi;
            res.maxDeviationDb = std::max(res.maxDeviationDb, std::fabs(s));
            e[i] = s > 0 ? std::max(s - cfg_.deadbandDb, 0.0) : -std::max(-s - cfg_.deadbandDb, 0.0);
        }
        const double k = cfg_.blockSeconds / cfg_.tauSeconds();
        const double slew = cfg_.slewDbPerMin() * cfg_.blockSeconds / 60.0;
        for (std::size_t i = 0; i < N; ++i) {
            auto& bs = band_[i];
            double dc = -k * e[i];  // 4. integrator step
            // oscillation guard: count sign alternations of the applied direction
            const int sg = dc > 0 ? 1 : (dc < 0 ? -1 : 0);
            if (sg != 0) {
                if (bs.lastSign != 0 && sg != bs.lastSign) {
                    // drop flips outside the window, then record
                    int m = 0;
                    for (int j = 0; j < bs.nFlips; ++j)
                        if (t_ - bs.flipTimes[j] <= cfg_.oscillationWindowSeconds) bs.flipTimes[m++] = bs.flipTimes[j];
                    bs.nFlips = m;
                    if (bs.nFlips >= 8) { std::copy(bs.flipTimes + 1, bs.flipTimes + 8, bs.flipTimes); bs.nFlips = 7; }
                    bs.flipTimes[bs.nFlips++] = t_;
                    if (bs.nFlips > cfg_.oscillationFlips) {
                        bs.halvedUntil = t_ + cfg_.oscillationHalveSeconds;
                        bs.nFlips = 0;
                        res.guardTriggered |= 1u << i;
                    }
                }
                bs.lastSign = sg;
            }
            if (t_ < bs.halvedUntil) dc *= 0.5;
            dc = std::clamp(dc, -slew, slew);                                   // 5. slew
            c_[i] = std::clamp(c_[i] + dc, -cfg_.clampDb, cfg_.clampDb);        // 6. clamp
        }
        double mean = 0.0;
        for (std::size_t i = 0; i < N; ++i) mean += w[i] * c_[i];
        mean = wsum > 0.0 ? mean / wsum : 0.0;
        for (std::size_t i = 0; i < N; ++i) c_[i] = std::clamp(c_[i] - mean, -cfg_.clampDb, cfg_.clampDb);
    }

    // 7. redesign flag
    double acc = 0.0;
    for (std::size_t i = 0; i < N; ++i) acc = std::max(acc, std::fabs(c_[i] - cDesigned_[i]));
    if (acc >= cfg_.redesignThresholdDb && t_ - lastDesignT_ >= cfg_.redesignMinSpacingSeconds) {
        res.redesignNeeded = true;
        cDesigned_ = c_;
        lastDesignT_ = t_;
    }
    for (std::size_t i = 0; i < N; ++i) {
        if (t_ < band_[i].halvedUntil) res.guardActive |= 1u << i;
        if (std::fabs(c_[i]) > 4.0) res.large = true;
    }
    res.correction = c_;
    return res;
}

}  // namespace bf
