#include "core/talker/TalkerPlanParams.h"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#include "core/math/DetMath.h"
#include "core/random/Random.h"

namespace bf {

namespace {
// Piecewise-linear through (0, a), (0.5, b), (1, c).
double pw3(double r, double a, double b, double c) {
    r = std::clamp(r, 0.0, 1.0);
    return r <= 0.5 ? a + (b - a) * (r / 0.5) : b + (c - b) * ((r - 0.5) / 0.5);
}
}  // namespace

void TalkerPlanParams::applyCvr(double r) {
    r = std::clamp(r, 0.0, 1.0);
    cvrMinFloor = r >= 0.66 ? 3u : (r >= 0.33 ? 2u : 1u);
    cvrDominanceCapDb = pw3(r, 6.0, 4.0, 2.5);
    cvrLevelSigmaMult = pw3(r, 1.0, 0.8, 0.6);
    overlapMinMs = std::max(overlapMinMs, pw3(r, 0.0, 150.0, 400.0));
    cvrOnsetWindowMs = r >= 0.66 ? 100.0 : (r >= 0.33 ? 300.0 : 0.0);
    cvrSoloRiskWeight = pw3(r, 1.0, 0.6, 0.3);
    if (r < 0.33) {
        cvrMaxPhraseS = 0.0;
    } else {
        const double x = std::clamp((r - 0.5) / 0.5, 0.0, 1.0);  // log interpolation 10 s -> 6 s
        cvrMaxPhraseS = detexp(detlog(10.0) + x * (detlog(6.0) - detlog(10.0)));
    }
}

std::uint32_t TalkerPlanParams::effectiveMin() const noexcept {
    if (mode != PlanMode::Stochastic) return maxActive;
    std::uint32_t mn = minActive;
    // CVR raises the floor only while min <= mean - 1 stays satisfiable (MASK §6.2).
    if (cvrMinFloor > mn && static_cast<double>(cvrMinFloor) <= mean - 1.0) mn = cvrMinFloor;
    return std::min(mn, maxActive);
}

std::string TalkerPlanParams::toJsonString() const {
    nlohmann::ordered_json j;
    j["mode"] = static_cast<int>(mode);
    j["pool"] = pool;
    j["mean"] = mean;
    j["min"] = minActive;
    j["max"] = maxActive;
    j["segMinS"] = segMinS;
    j["segMaxS"] = segMaxS;
    j["medianS"] = medianS;
    j["sigmaLn"] = sigmaLn;
    j["maxGapMs"] = maxGapMs;
    j["gainSigmaDb"] = gainSigmaDb;
    j["fadeInMs"] = fadeInMs;
    j["fadeOutMs"] = fadeOutMs;
    j["reEntryCooldownMs"] = reEntryCooldownMs;
    j["overlapMinMs"] = overlapMinMs;
    j["talkerRefDbfs"] = talkerRefDbfs;
    j["cvrMinFloor"] = cvrMinFloor;
    j["cvrDominanceCapDb"] = cvrDominanceCapDb;
    j["cvrLevelSigmaMult"] = cvrLevelSigmaMult;
    j["cvrOnsetWindowMs"] = cvrOnsetWindowMs;
    j["cvrSoloRiskWeight"] = cvrSoloRiskWeight;
    j["cvrMaxPhraseS"] = cvrMaxPhraseS;
    j["seed"] = seed;
    return j.dump();
}

std::uint64_t TalkerPlanParams::hash() const { return fnv1a64(toJsonString()); }

}  // namespace bf
