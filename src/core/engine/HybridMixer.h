#pragma once
// Per-zone constant-power hybrid mixer (docs/MASK_STRATEGIES.md section 4).
//
//   y = sqrt(b) * babble + sqrt(1 - b) * stationary       (independent inputs)
//
// b per zone = clamp(strategy b + zone offset, 0, 1). Changes to b are linear ramps of 2 s
// on b itself (each change restarts a ramp from the current value). Gains are evaluated on
// a 32-sample grid anchored to the absolute sample count and interpolated linearly inside a
// cell, so the output is independent of the block partitioning (bit-exact) provided
// parameter changes are applied at the same absolute sample positions.
// process() is RT-safe (no allocation).
#include <array>
#include <cstdint>
#include <vector>

namespace bf {

class HybridMixer {
 public:
  static constexpr int kGrid = 32;
  static constexpr double kRampSeconds = 2.0;
  static constexpr double kPauseAfterSeconds = 5.0;
  static constexpr int kMaxZones = 8;

  struct Gains {
    float babble;
    float stationary;
  };
  /// Constant-power law: g_b = sqrt(b), g_s = sqrt(1 - b).
  static Gains gainsFor(double b) noexcept;

  /// zoneOfChannel[c] in 0..7. Initial b applies immediately (no ramp).
  void prepare(double fs, const std::vector<int>& zoneOfChannel, double initialBabbleFraction);
  void reset() noexcept;

  /// Strategy-level babble energy fraction (all zones); ramps over 2 s.
  void setBabbleFraction(double b) noexcept;
  /// Per-zone offset in [-0.5, 0.5]; ramps over 2 s.
  void setZoneOffset(int zone, double offset) noexcept;
  /// Jump every zone to its current target immediately (start-up / offline rendering).
  void jumpToTarget() noexcept;

  /// babble/stationary/out: [ch][n]. A nullptr for babble or stationary (the whole array) is
  /// treated as silence (paused generator); out may alias either input. Any n >= 0.
  void process(const float* const* babble, const float* const* stationary, float* const* out,
               int n) noexcept;

  std::uint64_t sampleCount() const noexcept { return t_; }
  /// Current babble fraction of a zone (evaluated at the current sample count).
  double zoneFraction(int zone) const noexcept;
  double zoneTarget(int zone) const noexcept { return z_[static_cast<std::size_t>(zone)].target; }
  /// Hints (MASK_STRATEGIES 4.2): the component has been exactly unused (b == 1 for the
  /// stationary generator, b == 0 for babble) for more than 5 s. The caller decides whether
  /// to pause it (LaboratoryMask / fallback policy exceptions live in the caller).
  bool babbleUnused(int zone) const noexcept;
  bool stationaryUnused(int zone) const noexcept;
  bool babbleUnusedInAllZones() const noexcept;
  bool stationaryUnusedInAllZones() const noexcept;

 private:
  struct Zone {
    double offset = 0.0;
    double start = 0.0, target = 0.0;
    std::uint64_t t0 = 0;
  };
  double eval(const Zone& z, std::uint64_t t) const noexcept;
  void retarget(Zone& z, double newTarget) noexcept;
  double zoneB(const Zone& z) const noexcept;
  bool unusedFor(const Zone& z, double value) const noexcept;

  double fs_ = 48000.0;
  std::uint64_t rampSamples_ = 96000;
  std::uint64_t pauseSamples_ = 240000;
  double strategyB_ = 0.5;
  std::vector<int> zoneOfChannel_;
  std::array<Zone, kMaxZones> z_{};
  std::uint64_t t_ = 0;
};

}  // namespace bf
