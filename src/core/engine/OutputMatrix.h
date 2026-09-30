#pragma once
// Output matrix (docs/SPATIAL_ENGINE.md section 7.1/7.2): zone stage, output stage, calibration
// EQ slot and device channel map. Applied after all masker generation.
//
//   zone gain . zone enable -> output gain . mute . (polarity, delay) -> CalEQ slot -> map
//
//  * output/zone gain changes: 20 ms linear ramp; mute: 20 ms ramp; zone enable: 200 ms ramp
//  * polarity and delay changes: 20 ms fade-out, jump, 20 ms fade-in (applied after the delay
//    line, so the audible output goes through exact silence at the switch)
//  * delay: integer samples, 0..100 ms, preallocated line, sample-exact
//  * CalEQ: per-output interface slot (nullptr = identity in V1)
//  * device map: sparse; unmapped device channels receive silence, unmapped logical outputs
//    are dropped, several outputs on one device channel are summed
// All ramps are per sample, so the output is independent of the block partitioning
// (bit-exact) if parameter changes are applied at the same absolute sample positions.
// Setters and process() are meant for the audio thread (setters between blocks); no
// allocation happens outside prepare().
#include <cstdint>
#include <vector>

namespace bf {

/// Per-output calibration EQ slot (V2 fills it with a FIR/biquad; V1 leaves it empty).
class ICalEq {
 public:
  virtual ~ICalEq() = default;
  virtual void process(float* buffer, int n) noexcept = 0;
};

class OutputMatrix {
 public:
  static constexpr double kGainRampSeconds = 0.020;
  static constexpr double kMuteRampSeconds = 0.020;
  static constexpr double kFadeSeconds = 0.020;
  static constexpr double kZoneEnableRampSeconds = 0.200;
  static constexpr double kMaxDelaySeconds = 0.100;
  static constexpr int kMaxZones = 8;

  /// zoneOfOutput / deviceChannelOfOutput: one entry per logical output (device channel -1 =
  /// unmapped). numDeviceChannels: size of the device output (>= max mapped + 1).
  void prepare(double fs, int maxBlock, const std::vector<int>& zoneOfOutput,
               const std::vector<int>& deviceChannelOfOutput, int numDeviceChannels);
  void reset() noexcept;  // clears delay lines, snaps all ramps

  void setZoneGainDb(int zone, double db) noexcept;   // -24..+6
  void setZoneEnabled(int zone, bool enabled) noexcept;
  void setOutputGainDb(int o, double db) noexcept;    // -40..+6
  void setMute(int o, bool mute) noexcept;
  void setOutputEnabled(int o, bool enabled) noexcept;
  void setPolarityInvert(int o, bool invert) noexcept;
  void setDelayMs(int o, double ms) noexcept;         // clamped to 0..100 ms
  void setDelaySamples(int o, int samples) noexcept;
  void setCalEq(int o, ICalEq* eq) noexcept;
  /// Snap every ramp/fade to its target (start-up, tests).
  void applyImmediately() noexcept;

  int maxDelaySamples() const noexcept { return maxDelay_; }
  int activeDelaySamples(int o) const noexcept { return out_[idx(o)].delay; }
  bool activePolarityInverted(int o) const noexcept { return out_[idx(o)].pol < 0.0f; }
  bool fading(int o) const noexcept { return out_[idx(o)].state != Fade::Idle; }
  int numOutputs() const noexcept { return static_cast<int>(out_.size()); }
  int numDeviceChannels() const noexcept { return numDev_; }

  /// in: [numOutputs][n]; deviceOut: [numDeviceChannels][n]. deviceOut must not alias in.
  void process(const float* const* in, float* const* deviceOut, int n) noexcept;

 private:
  struct Ramp {
    double cur = 1.0, target = 1.0, step = 0.0;
    std::int64_t rem = 0;
    void set(double t, std::int64_t n) noexcept {
      target = t;
      if (n <= 0) {
        cur = t;
        rem = 0;
        step = 0.0;
      } else {
        rem = n;
        step = (t - cur) / static_cast<double>(n);
      }
    }
    double next() noexcept {
      if (rem > 0) {
        cur += step;
        if (--rem == 0) cur = target;
      }
      return cur;
    }
    void snap() noexcept {
      cur = target;
      rem = 0;
    }
  };
  enum class Fade { Idle, Out, In };
  struct Out {
    int zone = 0;
    int device = -1;
    Ramp gain, mute, fade;
    bool muted = false, enabled = true;
    float pol = 1.0f, pendPol = 1.0f;
    int delay = 0, pendDelay = 0;
    Fade state = Fade::Idle;
    std::vector<float> line;
    std::size_t pos = 0;
    ICalEq* eq = nullptr;
  };
  struct Zone {
    Ramp gain, enable;
  };
  static std::size_t idx(int i) noexcept { return static_cast<std::size_t>(i); }
  void requestFade(Out& o) noexcept;
  void updateMute(Out& o) noexcept;
  void processChunk(const float* const* in, float* const* deviceOut, int off, int n) noexcept;

  double fs_ = 48000.0;
  std::int64_t gainRamp_ = 960, muteRamp_ = 960, fadeRamp_ = 960, zoneEnableRamp_ = 9600;
  int maxDelay_ = 4800;
  int maxBlock_ = 0;
  int numDev_ = 0;
  std::vector<Out> out_;
  Zone zone_[kMaxZones];
  std::vector<double> zoneF_;  // kMaxZones x maxBlock
  std::vector<float> scratch_;
};

}  // namespace bf
