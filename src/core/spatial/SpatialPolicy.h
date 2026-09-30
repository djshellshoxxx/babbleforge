#pragma once
// Non-real-time, deterministic per-talker placement and motion (docs/SPATIAL_ENGINE.md
// section 3). The talker engine asks for a gain vector at each segment start and pulls
// updated vectors while the talker plays; the RT side only applies the vectors.
//
// RNG streams: placement draws come from "spatial.slot.<j>", motion/target re-draws from
// "spatial.motion" (one stream per slot: epoch mixed with the slot number, so a slot's
// trajectory does not depend on how other slots are called).
//
// All gain vectors have one entry per logical output (disabled outputs get 0) and are
// power-normalised (sum g^2 = 1).
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/dsp/AllpassDecorrelator.h"  // SpeakerVariation
#include "core/random/Random.h"
#include "core/spatial/OutputLayout.h"

namespace bf {

using GainVector = std::vector<float>;

struct SpatialParams {
  std::optional<SpatialAlgorithm> algorithm;  // empty = automatic
  float spread = 0.6f;                        // 0..1
  float motion = 0.5f;                        // 0..1
  SpeakerVariation variation = SpeakerVariation::Medium;
  int neighbourhoodSize = 3;                  // k_n, 2..5
  float balanceProbability = 0.7f;            // placement balancing probability
};

/// Density scaling for Large Distributed layouts (section 3.4). Inputs are per neighbourhood.
struct DensityScaling {
  int vTotal = 0;             // total simultaneous slots (<= cap)
  double mTotal = 0.0;        // total mean simultaneous talkers (after the cap)
  int minTotal = 0;
  double mEffective = 0.0;    // per-neighbourhood mean after the cap
  bool capped = false;        // spatial.densityCapped
};
DensityScaling scaleDensity(double meanPerNeighbourhood, int minPerNeighbourhood,
                            int maxPerNeighbourhood, int activeOutputs, int neighbourhoodSize,
                            int cap = 96);
/// P_total = max(P, V_total + 8) (before the corpus limit).
int totalPoolSize(int pool, int vTotal) noexcept;

class SpatialPolicy {
 public:
  static constexpr double kMaxPan = 0.70;
  static constexpr double kMaxPanRatePerS = 0.02;   // x motion
  static constexpr double kMaxAzimuthRateDegPerS = 3.0;  // x motion
  static constexpr double kMinSecondGainDb = -9.0;
  static constexpr double kMigrationSeconds = 3.0;

  SpatialPolicy(OutputLayout layout, const SpatialParams& params, std::uint64_t masterSeed,
                double sampleRate = 48000.0, std::uint64_t epoch = 0);

  SpatialAlgorithm algorithm() const noexcept { return algorithm_; }
  int numOutputs() const noexcept { return layout_.size(); }
  const OutputLayout& layout() const noexcept { return layout_; }

  /// Recent (10 s) babble energy per output, provided by the engine. Empty = no balancing.
  void setRecentEnergy(std::span<const double> perOutput);

  /// Place a talker at a segment start. zone >= 0 restricts a distributed talker to that zone.
  /// segmentSeconds bounds the (optional) migration time of a distributed talker.
  GainVector placeTalker(int slot, double segmentSeconds = 15.0, int zone = -1);
  /// Advance the slot's motion by dtSamples and return the new gains.
  GainVector updateMotion(int slot, std::int64_t dtSamples);
  void releaseTalker(int slot);
  const GainVector& gains(int slot) const;

  // Introspection (tests / diagnostics)
  double slotPan(int slot) const;
  double slotAzimuthDeg(int slot) const;
  int slotHome(int slot) const;
  bool slotMigrated(int slot) const;
  double stereoMaxPan() const noexcept { return pMax_; }
  int effectiveNeighbourhoodSize() const noexcept { return kn_; }
  /// Neighbourhood of output o (o first, then nearest neighbours, same zone only).
  std::span<const int> neighbourhood(int o) const;

  /// 2D VBAP for one direction (pair selection + 2x2 inverse; 1 degree table, interpolated).
  GainVector vbapGains(double azimuthDeg) const;
  /// MDAP: five directions at azimuth + deltaDeg*{-1,-.5,0,.5,1}, summed, power-normalised,
  /// with the minimum-spread rule (delta grows until the second gain is >= -9 dB).
  GainVector mdapGains(double azimuthDeg, double deltaDeg) const;
  static GainVector panGains(double pan);  // stereo constant-power pan law

 private:
  struct Slot {
    bool placed = false;
    std::optional<RngStream> place;
    std::optional<RngStream> motion;
    double pan = 0.0, panTarget = 0.0;
    double azimuth = 0.0, azTarget = 0.0;
    double retargetIn = 0.0;
    int home = -1;
    int loadOutput = -1;
    double age = 0.0;
    double migrateAt = -1.0;
    int migrateTo = -1;
    double migrateProgress = -1.0;
    bool migrated = false;
    GainVector migFrom, migTo;
    GainVector gains;
  };

  Slot& slotRef(int slot);
  const Slot& slotRef(int slot) const;
  void buildNeighbourhoods();
  void buildVbapTable();
  double distance(int a, int b) const;
  double normEnergy(int o) const;
  int leastLoaded(std::span<const int> candidates) const;
  GainVector distributedGains(int home) const;
  std::vector<double> tableGains(double azimuthDeg) const;
  std::vector<double> mdapVector(double azimuthDeg, double deltaDeg) const;
  GainVector expand(const std::vector<double>& spk) const;
  double baseDeltaDeg() const;
  void retarget(Slot& s);

  OutputLayout layout_;
  SpatialParams params_;
  SpatialAlgorithm algorithm_;
  std::uint64_t seed_;
  std::uint64_t epoch_;
  double fs_;
  double spread_, motion_;
  double pMax_ = 0.0;
  int kn_ = 3;
  std::vector<int> active_;         // enabled output indices
  std::vector<double> energy_;
  std::vector<int> loadCount_;      // talkers homed per output
  std::vector<Slot> slots_;
  // small multichannel
  std::vector<int> spk_;            // output index per speaker (sorted by azimuth)
  std::vector<double> spkAz_;
  std::vector<float> table_;        // 360 x nSpk
  // large distributed
  std::vector<std::vector<int>> nb_;
};

}  // namespace bf
