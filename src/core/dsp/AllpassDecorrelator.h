#pragma once
// Per-channel Schroeder all-pass decorrelator ("Speaker Variation", docs/SPATIAL_ENGINE.md
// section 5.2). Applied to the babble bus only, after the babble shaper.
//
//   A_i(z) = (-g_i + z^-D_i) / (1 - g_i z^-D_i);  y = A_S(...A_1(x))
//
// Stages S: Low 0 (bypass), Medium 3 (D 0.4-3.0 ms), High 5 (D 0.4-4.5 ms). Delays are
// distinct primes (samples) drawn without replacement from stream "decorrelator.ch.<c>"
// (unique within a channel; adjacent channels use disjoint prime sets whenever the pool
// allows). |g| is drawn in 0.45-0.60 with a sign alternating with (channel + stage). At
// design time each cascade is checked: the energy-decay curve must reach -60 dB by 25 ms
// (otherwise all |g| of that channel are scaled down), and the impulse-response inner
// product with the previous channel must stay small (redraw otherwise). Output is
// independent of the block partitioning (bit-exact); all memory is allocated in prepare().
#include <cstdint>
#include <vector>

namespace bf {

enum class SpeakerVariation { Low, Medium, High };

class AllpassDecorrelator {
 public:
  static constexpr double kDecayLimitSeconds = 0.025;
  static constexpr double kDecayLimitEnergy = 1.0e-6;  // -60 dB

  void prepare(double fs, int numChannels, SpeakerVariation level, std::uint64_t masterSeed,
               std::uint64_t epoch = 0);
  void reset() noexcept;

  /// in/out: [ch][n]; in may alias out. Any n >= 0. RT-safe.
  void process(const float* const* in, float* const* out, int n) noexcept;

  int numChannels() const noexcept { return static_cast<int>(channels_.size()); }
  int numStages() const noexcept { return stages_; }
  int delaySamples(int ch, int stage) const noexcept { return channels_[idx(ch)].st[idx(stage)].delay; }
  /// Signed g of the stage as used by the filter (after any decay-driven scaling).
  float gain(int ch, int stage) const noexcept { return channels_[idx(ch)].st[idx(stage)].g; }
  double gainScale(int ch) const noexcept { return channels_[idx(ch)].scale; }
  /// Energy fraction of the impulse response remaining after 25 ms (must be <= 1e-6).
  double tailEnergy(int ch) const noexcept { return channels_[idx(ch)].tail; }
  /// Nominal delay range in samples used for the prime draw.
  int minDelaySamples() const noexcept { return dLo_; }
  int maxDelaySamples() const noexcept { return dHi_; }

 private:
  struct Stage {
    int delay = 0;
    float g = 0.0f;
    float magnitude = 0.0f;  // unscaled |g|
    float sign = 1.0f;
    std::vector<float> buf;
    int pos = 0;
  };
  struct Channel {
    std::vector<Stage> st;
    double scale = 1.0;
    double tail = 0.0;
  };
  static std::size_t idx(int i) noexcept { return static_cast<std::size_t>(i); }

  std::vector<Channel> channels_;
  int stages_ = 0;
  int dLo_ = 0, dHi_ = 0;
  double fs_ = 48000.0;
};

}  // namespace bf
