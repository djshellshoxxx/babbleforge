#pragma once
// Per-channel babble balance (docs/SPATIAL_ENGINE.md section 4). Pure logic; the engine
// applies gain(c) = feed-forward normalisation x feedback trim to the babble bus.
//
// Feed-forward: E_c = sum_v a_v g_vc^2, g_bnorm,c = 1/sqrt(E_c); recomputed every ~1 s from
// the planned events and smoothed with tau = 10 s.
// Feedback: per-channel trim toward equal T1 power per channel (measured power averaged
// with tau = 60 s), limited to +-3 dB and slewed at <= 0.25 dB/s; frozen on request.
#include <span>
#include <vector>

namespace bf {

class ChannelBalance {
 public:
  static constexpr double kFeedForwardTauS = 10.0;
  static constexpr double kFeedbackTauS = 60.0;
  static constexpr double kTrimLimitDb = 3.0;
  static constexpr double kTrimSlewDbPerS = 0.25;

  void prepare(int numChannels);
  void reset();

  /// E_c = sum_v activity[v] * gains[v][c]^2 (gains: one gain vector per slot).
  static std::vector<double> expectedPowers(std::span<const double> activity,
                                            std::span<const std::vector<float>> gains,
                                            int numChannels);

  /// Feed-forward update. dtSeconds = time since the previous call (first call initialises
  /// without smoothing). Relative floor: E_c is limited to >= 1e-6 * max(E).
  void updateFeedForward(std::span<const double> expectedPower, double dtSeconds);

  /// Feedback update from measured per-channel T1 babble power (linear, measured after the
  /// trim; the trim is divided out internally so the loop is not self-referential).
  void updateFeedback(std::span<const double> measuredPower, double dtSeconds);

  void setFrozen(bool frozen) noexcept { frozen_ = frozen; }
  bool frozen() const noexcept { return frozen_; }

  int numChannels() const noexcept { return static_cast<int>(ff_.size()); }
  double feedForwardGain(int c) const noexcept { return ff_[static_cast<std::size_t>(c)]; }
  double trimDb(int c) const noexcept { return trimDb_[static_cast<std::size_t>(c)]; }
  /// Linear gain to apply on channel c.
  double gain(int c) const noexcept;

 private:
  std::vector<double> ff_;       // smoothed feed-forward gain (linear)
  std::vector<double> trimDb_;   // applied trim (dB)
  std::vector<double> avgPower_; // feedback power average
  bool ffInit_ = false;
  bool fbInit_ = false;
  bool frozen_ = false;
};

}  // namespace bf
