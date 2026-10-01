#include "core/engine/HybridMixer.h"

#include <algorithm>
#include <cmath>

#include "core/math/DetMath.h"

namespace bf {

HybridMixer::Gains HybridMixer::gainsFor(double b) noexcept {
  b = std::clamp(b, 0.0, 1.0);
  return {static_cast<float>(detsqrt(b)), static_cast<float>(detsqrt(1.0 - b))};
}

double HybridMixer::zoneB(const Zone& z) const noexcept {
  return std::clamp(strategyB_ + z.offset, 0.0, 1.0);
}

void HybridMixer::prepare(double fs, const std::vector<int>& zoneOfChannel, double initialB) {
  fs_ = fs;
  rampSamples_ = static_cast<std::uint64_t>(std::llround(kRampSeconds * fs));
  pauseSamples_ = static_cast<std::uint64_t>(std::llround(kPauseAfterSeconds * fs));
  zoneOfChannel_ = zoneOfChannel;
  for (auto& c : zoneOfChannel_) c = std::clamp(c, 0, kMaxZones - 1);
  strategyB_ = std::clamp(initialB, 0.0, 1.0);
  t_ = 0;
  for (auto& z : z_) {
    z = Zone{};
    z.start = z.target = zoneB(z);
    z.t0 = 0;
  }
}

void HybridMixer::reset() noexcept {
  t_ = 0;
  for (auto& z : z_) {
    z.start = z.target = zoneB(z);
    z.t0 = 0;
  }
}

double HybridMixer::eval(const Zone& z, std::uint64_t t) const noexcept {
  if (t <= z.t0) return z.start;
  const std::uint64_t d = t - z.t0;
  if (d >= rampSamples_) return z.target;
  return z.start + (z.target - z.start) * (static_cast<double>(d) / static_cast<double>(rampSamples_));
}

void HybridMixer::retarget(Zone& z, double newTarget) noexcept {
  if (newTarget == z.target) return;
  z.start = eval(z, t_);
  z.t0 = t_;
  z.target = newTarget;
}

void HybridMixer::setBabbleFraction(double b) noexcept {
  strategyB_ = std::clamp(b, 0.0, 1.0);
  for (auto& z : z_) retarget(z, zoneB(z));
}

void HybridMixer::setZoneOffset(int zone, double offset) noexcept {
  auto& z = z_[static_cast<std::size_t>(std::clamp(zone, 0, kMaxZones - 1))];
  z.offset = std::clamp(offset, -0.5, 0.5);
  retarget(z, zoneB(z));
}

void HybridMixer::jumpToTarget() noexcept {
  for (auto& z : z_) {
    z.start = z.target;
    z.t0 = t_;
  }
}

double HybridMixer::zoneFraction(int zone) const noexcept {
  return eval(z_[static_cast<std::size_t>(zone)], t_);
}

bool HybridMixer::unusedFor(const Zone& z, double value) const noexcept {
  if (z.target != value) return false;
  const std::uint64_t settledAt = z.start == value ? z.t0 : z.t0 + rampSamples_;
  return t_ > settledAt + pauseSamples_;
}

bool HybridMixer::babbleUnused(int zone) const noexcept {
  return unusedFor(z_[static_cast<std::size_t>(zone)], 0.0);
}
bool HybridMixer::stationaryUnused(int zone) const noexcept {
  return unusedFor(z_[static_cast<std::size_t>(zone)], 1.0);
}
bool HybridMixer::babbleUnusedInAllZones() const noexcept {
  for (int c : zoneOfChannel_)
    if (!babbleUnused(c)) return false;
  return true;
}
bool HybridMixer::stationaryUnusedInAllZones() const noexcept {
  for (int c : zoneOfChannel_)
    if (!stationaryUnused(c)) return false;
  return true;
}

void HybridMixer::process(const float* const* babble, const float* const* stationary,
                          float* const* out, int n) noexcept {
  const std::size_t nch = zoneOfChannel_.size();
  int done = 0;
  while (done < n) {
    const std::uint64_t cell = t_ / kGrid;
    const std::uint64_t cellStart = cell * kGrid;
    const int offsetInCell = static_cast<int>(t_ - cellStart);
    const int len = std::min(kGrid - offsetInCell, n - done);

    std::array<Gains, kMaxZones> g0{}, g1{};
    for (int zi = 0; zi < kMaxZones; ++zi) {
      g0[static_cast<std::size_t>(zi)] = gainsFor(eval(z_[static_cast<std::size_t>(zi)], cellStart));
      g1[static_cast<std::size_t>(zi)] = gainsFor(eval(z_[static_cast<std::size_t>(zi)], cellStart + kGrid));
    }
    for (std::size_t c = 0; c < nch; ++c) {
      const auto zi = static_cast<std::size_t>(zoneOfChannel_[c]);
      const Gains a = g0[zi], b = g1[zi];
      const float db = b.babble - a.babble, ds = b.stationary - a.stationary;
      const float* pb = babble ? babble[c] : nullptr;
      const float* ps = stationary ? stationary[c] : nullptr;
      float* po = out[c];
      for (int i = 0; i < len; ++i) {
        const float f = static_cast<float>(offsetInCell + i) * (1.0f / static_cast<float>(kGrid));
        const float gb = a.babble + db * f;
        const float gs = a.stationary + ds * f;
        const float xb = pb ? pb[done + i] : 0.0f;
        const float xs = ps ? ps[done + i] : 0.0f;
        po[done + i] = gb * xb + gs * xs;
      }
    }
    done += len;
    t_ += static_cast<std::uint64_t>(len);
  }
}

}  // namespace bf
