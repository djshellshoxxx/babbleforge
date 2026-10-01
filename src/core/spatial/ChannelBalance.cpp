#include "core/spatial/ChannelBalance.h"

#include <algorithm>
#include <cmath>

#include "core/math/DetMath.h"

namespace bf {

void ChannelBalance::prepare(int numChannels) {
  const auto n = static_cast<std::size_t>(std::max(numChannels, 0));
  ff_.assign(n, 1.0);
  trimDb_.assign(n, 0.0);
  avgPower_.assign(n, 0.0);
  ffInit_ = fbInit_ = false;
  frozen_ = false;
}

void ChannelBalance::reset() { prepare(numChannels()); }

std::vector<double> ChannelBalance::expectedPowers(std::span<const double> activity,
                                                   std::span<const std::vector<float>> gains,
                                                   int numChannels) {
  std::vector<double> e(static_cast<std::size_t>(std::max(numChannels, 0)), 0.0);
  const auto v = std::min(activity.size(), gains.size());
  for (std::size_t i = 0; i < v; ++i)
    for (std::size_t c = 0; c < e.size() && c < gains[i].size(); ++c) {
      const double g = static_cast<double>(gains[i][c]);
      e[c] += activity[i] * g * g;
    }
  return e;
}

void ChannelBalance::updateFeedForward(std::span<const double> expectedPower, double dtSeconds) {
  const auto n = std::min(expectedPower.size(), ff_.size());
  double maxE = 0.0;
  for (std::size_t c = 0; c < n; ++c) maxE = std::max(maxE, expectedPower[c]);
  const double alpha = ffInit_ ? 1.0 - detexp(-std::max(dtSeconds, 0.0) / kFeedForwardTauS) : 1.0;
  for (std::size_t c = 0; c < n; ++c) {
    double target = 1.0;
    if (maxE > 0.0) target = 1.0 / detsqrt(std::max(expectedPower[c], 1.0e-6 * maxE));
    ff_[c] += alpha * (target - ff_[c]);
  }
  ffInit_ = true;
}

void ChannelBalance::updateFeedback(std::span<const double> measuredPower, double dtSeconds) {
  if (frozen_) return;
  const auto n = std::min(measuredPower.size(), avgPower_.size());
  if (n == 0) return;
  const double dt = std::max(dtSeconds, 0.0);
  const double alpha = fbInit_ ? 1.0 - detexp(-dt / kFeedbackTauS) : 1.0;
  // The measurement is taken after the trim, so refer it back to the un-trimmed power.
  for (std::size_t c = 0; c < n; ++c) {
    const double trimLin = detexp(trimDb_[c] * (detlog(10.0) / 10.0));  // power factor
    const double raw = std::max(measuredPower[c], 0.0) / trimLin;
    avgPower_[c] += alpha * (raw - avgPower_[c]);
  }
  fbInit_ = true;

  double mean = 0.0;
  for (std::size_t c = 0; c < n; ++c) mean += avgPower_[c];
  mean /= static_cast<double>(n);
  if (mean <= 1.0e-30) return;
  const double maxStep = kTrimSlewDbPerS * dt;
  for (std::size_t c = 0; c < n; ++c) {
    // Trim is applied on amplitude; equalising power P_c -> mean needs 10*log10(mean/P_c) dB.
    const double p = std::max(avgPower_[c], mean * 1.0e-6);
    double desired = 10.0 * detlog(mean / p) / detlog(10.0);
    desired = std::clamp(desired, -kTrimLimitDb, kTrimLimitDb);
    const double step = std::clamp(desired - trimDb_[c], -maxStep, maxStep);
    trimDb_[c] += step;
  }
}

double ChannelBalance::gain(int c) const noexcept {
  const auto i = static_cast<std::size_t>(c);
  return ff_[i] * detexp(trimDb_[i] * (detlog(10.0) / 20.0));
}

}  // namespace bf
