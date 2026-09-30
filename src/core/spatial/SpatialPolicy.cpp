#include "core/spatial/SpatialPolicy.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include "core/math/DetMath.h"

namespace bf {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;

double wrap360(double a) {
  a = std::fmod(a, 360.0);
  if (a < 0.0) a += 360.0;
  return a;
}
double wrap180(double a) {
  a = wrap360(a + 180.0) - 180.0;
  return a;
}

double dbToLin(double db) { return detexp(db * (detlog(10.0) / 20.0)); }

void normalise(std::vector<double>& g) {
  double s = 0.0;
  for (double v : g) s += v * v;
  if (s <= 0.0) return;
  const double k = 1.0 / detsqrt(s);
  for (double& v : g) v *= k;
}

// Move x toward target by at most maxStep.
double approach(double x, double target, double maxStep) {
  const double d = target - x;
  if (std::fabs(d) <= maxStep) return target;
  return x + (d > 0 ? maxStep : -maxStep);
}

}  // namespace

DensityScaling scaleDensity(double m, int minPer, int maxPer, int activeOutputs, int kn, int cap) {
  DensityScaling r;
  kn = std::max(kn, 1);
  const double n = static_cast<double>(std::max(activeOutputs, 0));
  const double k = static_cast<double>(kn);
  double v = std::ceil(static_cast<double>(maxPer) * n / k - 1e-9);
  r.mTotal = m * n / k;
  r.minTotal = static_cast<int>(std::ceil(static_cast<double>(minPer) * n / k - 1e-9));
  r.mEffective = m;
  if (v > static_cast<double>(cap)) {
    const double f = static_cast<double>(cap) / v;
    v = static_cast<double>(cap);
    r.mTotal *= f;
    r.mEffective = m * f;
    r.minTotal = static_cast<int>(std::floor(static_cast<double>(r.minTotal) * f));
    r.capped = true;
  }
  r.vTotal = static_cast<int>(v);
  return r;
}

int totalPoolSize(int pool, int vTotal) noexcept { return std::max(pool, vTotal + 8); }

GainVector SpatialPolicy::panGains(double pan) {
  double s = 0.0, c = 1.0;
  detsincos((pan + 1.0) * kPi / 4.0, s, c);
  return {static_cast<float>(c), static_cast<float>(s)};
}

SpatialPolicy::SpatialPolicy(OutputLayout layout, const SpatialParams& params,
                             std::uint64_t masterSeed, double sampleRate, std::uint64_t epoch)
    : layout_(std::move(layout)),
      params_(params),
      algorithm_(SpatialAlgorithm::Mono),
      seed_(masterSeed),
      epoch_(epoch),
      fs_(sampleRate) {
  if (layout_.outputs.empty()) throw std::invalid_argument("empty layout");
  spread_ = std::clamp(static_cast<double>(params.spread), 0.0, 1.0);
  motion_ = std::clamp(static_cast<double>(params.motion), 0.0, 1.0);
  for (int i = 0; i < layout_.size(); ++i)
    if (layout_.outputs[static_cast<std::size_t>(i)].enabled) active_.push_back(i);
  if (active_.empty()) throw std::invalid_argument("no enabled outputs");

  algorithm_ = params.algorithm.value_or(selectAlgorithm(layout_));
  const int n = static_cast<int>(active_.size());
  // Fall back to automatic when a forced algorithm cannot work on this layout.
  if ((algorithm_ == SpatialAlgorithm::DistributedStereo && n < 2) ||
      (algorithm_ == SpatialAlgorithm::SmallMultichannel && n < 3) ||
      (algorithm_ == SpatialAlgorithm::LargeDistributed && n < 2))
    algorithm_ = selectAlgorithm(layout_);
  if (n == 1) algorithm_ = SpatialAlgorithm::Mono;

  energy_.assign(static_cast<std::size_t>(layout_.size()), 0.0);
  loadCount_.assign(static_cast<std::size_t>(layout_.size()), 0);

  double pm = kMaxPan * spread_;
  if (params_.variation == SpeakerVariation::High) pm *= 1.1;
  pMax_ = std::min(pm, kMaxPan);

  int kn = std::clamp(params.neighbourhoodSize, 2, 5);
  if (params_.variation == SpeakerVariation::High) kn = std::max(2, kn - 1);
  kn_ = kn;

  if (algorithm_ == SpatialAlgorithm::SmallMultichannel) buildVbapTable();
  if (algorithm_ == SpatialAlgorithm::LargeDistributed) buildNeighbourhoods();
}

// ---------------------------------------------------------------- helpers

SpatialPolicy::Slot& SpatialPolicy::slotRef(int slot) {
  if (slot < 0) throw std::out_of_range("slot");
  if (static_cast<std::size_t>(slot) >= slots_.size()) slots_.resize(static_cast<std::size_t>(slot) + 1);
  return slots_[static_cast<std::size_t>(slot)];
}
const SpatialPolicy::Slot& SpatialPolicy::slotRef(int slot) const {
  if (slot < 0 || static_cast<std::size_t>(slot) >= slots_.size()) throw std::out_of_range("slot");
  return slots_[static_cast<std::size_t>(slot)];
}

void SpatialPolicy::setRecentEnergy(std::span<const double> perOutput) {
  std::fill(energy_.begin(), energy_.end(), 0.0);
  for (std::size_t i = 0; i < perOutput.size() && i < energy_.size(); ++i)
    energy_[i] = std::max(perOutput[i], 0.0);
}

double SpatialPolicy::normEnergy(int o) const {
  double mean = 0.0;
  for (int a : active_) mean += energy_[static_cast<std::size_t>(a)];
  mean /= static_cast<double>(active_.size());
  if (mean <= 1e-30) return 0.0;
  return energy_[static_cast<std::size_t>(o)] / mean;
}

int SpatialPolicy::leastLoaded(std::span<const int> candidates) const {
  int best = candidates.front();
  double bestLoad = 1e300;
  for (int o : candidates) {
    const double load = normEnergy(o) + 0.1 * loadCount_[static_cast<std::size_t>(o)];
    if (load < bestLoad - 1e-12) {
      bestLoad = load;
      best = o;
    }
  }
  return best;
}

double SpatialPolicy::distance(int a, int b) const {
  const auto& oa = layout_.outputs[static_cast<std::size_t>(a)];
  const auto& ob = layout_.outputs[static_cast<std::size_t>(b)];
  if (oa.posM && ob.posM) {
    const double dx = static_cast<double>(oa.posM->x) - static_cast<double>(ob.posM->x);
    const double dy = static_cast<double>(oa.posM->y) - static_cast<double>(ob.posM->y);
    return detsqrt(dx * dx + dy * dy);
  }
  if (layout_.kind == LayoutKind::Ring || layout_.kind == LayoutKind::Stereo)
    return std::fabs(wrap180(static_cast<double>(oa.azimuthDeg) - static_cast<double>(ob.azimuthDeg)));
  return static_cast<double>(std::abs(a - b));
}

// ---------------------------------------------------------------- large distributed

void SpatialPolicy::buildNeighbourhoods() {
  nb_.assign(static_cast<std::size_t>(layout_.size()), {});
  for (int o : active_) {
    const auto zo = layout_.outputs[static_cast<std::size_t>(o)].zone;
    std::vector<std::pair<double, int>> cand;
    for (int p : active_) {
      if (p == o || layout_.outputs[static_cast<std::size_t>(p)].zone != zo) continue;
      cand.emplace_back(distance(o, p), p);
    }
    std::sort(cand.begin(), cand.end());
    auto& nb = nb_[static_cast<std::size_t>(o)];
    nb.push_back(o);
    for (std::size_t i = 0; i < cand.size() && static_cast<int>(nb.size()) < kn_; ++i)
      nb.push_back(cand[i].second);
  }
}

std::span<const int> SpatialPolicy::neighbourhood(int o) const {
  if (nb_.empty()) return {};
  return nb_[static_cast<std::size_t>(o)];
}

GainVector SpatialPolicy::distributedGains(int home) const {
  const auto& nb = nb_[static_cast<std::size_t>(home)];
  double dmax = 0.0;
  for (std::size_t i = 1; i < nb.size(); ++i) dmax = std::max(dmax, distance(home, nb[i]));
  std::vector<double> g(static_cast<std::size_t>(layout_.size()), 0.0);
  g[static_cast<std::size_t>(home)] = 1.0;
  for (std::size_t i = 1; i < nb.size(); ++i) {
    const double ratio = dmax > 0.0 ? distance(home, nb[i]) / dmax : 0.0;
    g[static_cast<std::size_t>(nb[i])] = dbToLin(-3.0 - 3.0 * ratio);
  }
  normalise(g);
  return GainVector(g.begin(), g.end());
}

// ---------------------------------------------------------------- VBAP / MDAP

void SpatialPolicy::buildVbapTable() {
  std::vector<std::pair<double, int>> v;
  for (int o : active_)
    v.emplace_back(static_cast<double>(layout_.outputs[static_cast<std::size_t>(o)].azimuthDeg), o);
  std::sort(v.begin(), v.end());
  for (auto& p : v) {
    spkAz_.push_back(p.first);
    spk_.push_back(p.second);
  }
  const std::size_t n = spk_.size();
  table_.assign(360 * n, 0.0f);
  for (int a = 0; a < 360; ++a) {
    std::vector<double> g(n, 0.0);
    const double az = static_cast<double>(a);
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t j = (i + 1) % n;
      double arc = wrap360(spkAz_[j] - spkAz_[i]);
      if (arc <= 0.0) arc = 360.0;
      const double rel = wrap360(az - spkAz_[i]);
      if (rel > arc + 1e-9) continue;
      double si, ci, sj, cj, sp, cp;
      detsincos(spkAz_[i] * kDegToRad, si, ci);
      detsincos(spkAz_[j] * kDegToRad, sj, cj);
      detsincos(az * kDegToRad, sp, cp);
      const double det = ci * sj - si * cj;  // l_i x l_j
      double gi, gj;
      if (arc >= 179.0 || std::fabs(det) < 1e-6) {
        const double t = rel / arc * kPi / 2.0;  // degenerate pair: constant-power fallback
        gi = detcos(t);
        gj = detsin(t);
      } else {
        gi = (cp * sj - sp * cj) / det;  // p x l_j / (l_i x l_j)
        gj = (ci * sp - si * cp) / det;  // l_i x p / (l_i x l_j)
      }
      g[i] = std::max(gi, 0.0);
      g[j] = std::max(gj, 0.0);
      break;
    }
    normalise(g);
    for (std::size_t i = 0; i < n; ++i) table_[static_cast<std::size_t>(a) * n + i] = static_cast<float>(g[i]);
  }
}

std::vector<double> SpatialPolicy::tableGains(double azimuthDeg) const {
  const std::size_t n = spk_.size();
  const double a = wrap360(azimuthDeg);
  const auto i0 = static_cast<std::size_t>(std::floor(a)) % 360;
  const std::size_t i1 = (i0 + 1) % 360;
  const double f = a - std::floor(a);
  std::vector<double> g(n);
  for (std::size_t i = 0; i < n; ++i)
    g[i] = (1.0 - f) * static_cast<double>(table_[i0 * n + i]) +
           f * static_cast<double>(table_[i1 * n + i]);
  return g;
}

GainVector SpatialPolicy::expand(const std::vector<double>& spk) const {
  GainVector out(static_cast<std::size_t>(layout_.size()), 0.0f);
  for (std::size_t i = 0; i < spk.size(); ++i) out[static_cast<std::size_t>(spk_[i])] = static_cast<float>(spk[i]);
  return out;
}

GainVector SpatialPolicy::vbapGains(double azimuthDeg) const {
  if (spk_.empty()) throw std::logic_error("VBAP needs the small multichannel algorithm");
  auto g = tableGains(azimuthDeg);
  normalise(g);
  return expand(g);
}

std::vector<double> SpatialPolicy::mdapVector(double azimuthDeg, double deltaDeg) const {
  static constexpr double kOffsets[5] = {-1.0, -0.5, 0.0, 0.5, 1.0};
  std::vector<double> sum(spk_.size(), 0.0);
  for (double k : kOffsets) {
    const auto g = tableGains(azimuthDeg + deltaDeg * k);
    for (std::size_t i = 0; i < sum.size(); ++i) sum[i] += g[i];
  }
  normalise(sum);
  return sum;
}

GainVector SpatialPolicy::mdapGains(double azimuthDeg, double deltaDeg) const {
  if (spk_.empty()) throw std::logic_error("MDAP needs the small multichannel algorithm");
  const double ratio = 0.3550;  // slightly above 10^(-9/20) = 0.35481 (float margin)
  double d = std::max(deltaDeg, 0.0);
  std::vector<double> g;
  for (;;) {
    g = mdapVector(azimuthDeg, d);
    double m1 = 0.0, m2 = 0.0;
    for (double v : g) {
      if (v > m1) {
        m2 = m1;
        m1 = v;
      } else if (v > m2) {
        m2 = v;
      }
    }
    if (m2 >= ratio * m1 && m2 > 0.0) break;
    if (d >= 180.0) break;
    d = std::min(d + 1.0, 180.0);
  }
  return expand(g);
}

double SpatialPolicy::baseDeltaDeg() const {
  double d = spread_ * 45.0;
  if (params_.variation == SpeakerVariation::High) d *= 1.2;
  return d;
}

// ---------------------------------------------------------------- placement & motion

void SpatialPolicy::retarget(Slot& s) {
  auto& r = *s.motion;
  switch (algorithm_) {
    case SpatialAlgorithm::DistributedStereo:
      s.panTarget = (r.uniform01() * 2.0 - 1.0) * pMax_;
      s.retargetIn = 4.0 + 4.0 * r.uniform01();
      break;
    case SpatialAlgorithm::SmallMultichannel:
      s.azTarget = wrap180(s.azimuth + (r.uniform01() * 2.0 - 1.0) * 30.0);
      s.retargetIn = 5.0 + 5.0 * r.uniform01();
      break;
    default:
      break;
  }
}

GainVector SpatialPolicy::placeTalker(int slot, double segmentSeconds, int zone) {
  Slot& s = slotRef(slot);
  if (!s.place) {
    s.place.emplace(seed_, "spatial.slot." + std::to_string(slot), epoch_);
    s.motion.emplace(seed_, "spatial.motion", epoch_ * 4096u + static_cast<std::uint64_t>(slot) + 1u);
  }
  if (s.placed && s.loadOutput >= 0) --loadCount_[static_cast<std::size_t>(s.loadOutput)];
  s.placed = true;
  s.age = 0.0;
  s.migrateProgress = -1.0;
  s.migrateAt = -1.0;
  s.migrated = false;
  s.loadOutput = -1;
  auto& r = *s.place;
  const double pBal = std::clamp(static_cast<double>(params_.balanceProbability), 0.0, 1.0);
  const std::size_t nOut = static_cast<std::size_t>(layout_.size());

  switch (algorithm_) {
    case SpatialAlgorithm::Mono: {
      s.gains.assign(nOut, 0.0f);
      s.gains[static_cast<std::size_t>(active_.front())] = 1.0f;
      break;
    }
    case SpatialAlgorithm::DistributedStereo: {
      const double u1 = r.uniform01(), u2 = r.uniform01();
      double p = (u1 * 2.0 - 1.0) * pMax_;
      const int oL = active_[0], oR = active_[1];
      const double eL = energy_[static_cast<std::size_t>(oL)], eR = energy_[static_cast<std::size_t>(oR)];
      if (u2 < pBal && eL != eR) {
        const bool lowerIsLeft = eL < eR;  // p < 0 = left
        if ((lowerIsLeft && p > 0.0) || (!lowerIsLeft && p < 0.0)) p = -p;
      }
      s.pan = p;
      retarget(s);
      const auto g2 = panGains(p);
      s.gains.assign(nOut, 0.0f);
      s.gains[static_cast<std::size_t>(oL)] = g2[0];
      s.gains[static_cast<std::size_t>(oR)] = g2[1];
      break;
    }
    case SpatialAlgorithm::SmallMultichannel: {
      const double u1 = r.uniform01(), u2 = r.uniform01();
      double az;
      int least = leastLoaded(active_);
      if (u1 < pBal) {
        az = static_cast<double>(layout_.outputs[static_cast<std::size_t>(least)].azimuthDeg) +
             (u2 * 2.0 - 1.0) * 15.0;
      } else {
        az = u2 * 360.0 - 180.0;
      }
      s.azimuth = wrap180(az);
      retarget(s);
      s.gains = mdapGains(s.azimuth, baseDeltaDeg());
      // Load bookkeeping: the speaker with the largest gain.
      std::size_t arg = 0;
      for (std::size_t i = 1; i < s.gains.size(); ++i)
        if (s.gains[i] > s.gains[arg]) arg = i;
      s.loadOutput = static_cast<int>(arg);
      s.home = s.loadOutput;
      break;
    }
    case SpatialAlgorithm::LargeDistributed: {
      const double u1 = r.uniform01(), u2 = r.uniform01(), u3 = r.uniform01(), u4 = r.uniform01(),
                   u5 = r.uniform01();
      std::vector<int> cand;
      for (int o : active_)
        if (zone < 0 || layout_.outputs[static_cast<std::size_t>(o)].zone == zone) cand.push_back(o);
      if (cand.empty()) cand = active_;
      int home;
      if (u1 < pBal)
        home = leastLoaded(cand);
      else
        home = cand[std::min(static_cast<std::size_t>(u2 * static_cast<double>(cand.size())), cand.size() - 1)];
      s.home = home;
      s.loadOutput = home;
      s.gains = distributedGains(home);
      const auto& nb = nb_[static_cast<std::size_t>(home)];
      const double window = segmentSeconds - kMigrationSeconds;
      if (nb.size() > 1 && window > 0.5 && u3 < 0.3 * motion_) {
        s.migrateAt = window * (0.1 + 0.8 * u4);
        s.migrateTo = nb[1 + std::min(static_cast<std::size_t>(u5 * static_cast<double>(nb.size() - 1)),
                                      nb.size() - 2)];
      }
      break;
    }
  }
  if (s.loadOutput >= 0) ++loadCount_[static_cast<std::size_t>(s.loadOutput)];
  return s.gains;
}

GainVector SpatialPolicy::updateMotion(int slot, std::int64_t dtSamples) {
  Slot& s = slotRef(slot);
  if (!s.placed) return s.gains;
  double rem = static_cast<double>(dtSamples) / fs_;
  if (rem <= 0.0) return s.gains;
  const std::size_t nOut = static_cast<std::size_t>(layout_.size());

  switch (algorithm_) {
    case SpatialAlgorithm::DistributedStereo:
    case SpatialAlgorithm::SmallMultichannel: {
      const bool stereo = algorithm_ == SpatialAlgorithm::DistributedStereo;
      const double rate = stereo ? kMaxPanRatePerS * motion_ : kMaxAzimuthRateDegPerS * motion_;
      while (rem > 1e-12) {
        const double step = std::min(rem, s.retargetIn);
        if (stereo) {
          s.pan = std::clamp(approach(s.pan, s.panTarget, rate * step), -pMax_, pMax_);
        } else {
          const double d = wrap180(s.azTarget - s.azimuth);
          s.azimuth = wrap180(s.azimuth + (d > 0 ? 1.0 : -1.0) * std::min(std::fabs(d), rate * step));
        }
        s.retargetIn -= step;
        rem -= step;
        if (s.retargetIn <= 1e-12) retarget(s);
      }
      if (stereo) {
        const auto g2 = panGains(s.pan);
        s.gains.assign(nOut, 0.0f);
        s.gains[static_cast<std::size_t>(active_[0])] = g2[0];
        s.gains[static_cast<std::size_t>(active_[1])] = g2[1];
      } else {
        s.gains = mdapGains(s.azimuth, baseDeltaDeg());
      }
      break;
    }
    case SpatialAlgorithm::LargeDistributed: {
      s.age += rem;
      if (s.migrateTo >= 0 && s.migrateProgress < 0.0 && s.age >= s.migrateAt) {
        s.migrateProgress = 0.0;
        s.migFrom = s.gains;
        s.migTo = distributedGains(s.migrateTo);
        s.migrateProgress = std::min(1.0, (s.age - s.migrateAt) / kMigrationSeconds);
      } else if (s.migrateProgress >= 0.0 && s.migrateProgress < 1.0) {
        s.migrateProgress = std::min(1.0, (s.age - s.migrateAt) / kMigrationSeconds);
      }
      if (s.migrateProgress >= 0.0) {
        double sn, cs;
        detsincos(s.migrateProgress * kPi / 2.0, sn, cs);
        std::vector<double> g(nOut);
        for (std::size_t i = 0; i < nOut; ++i)
          g[i] = cs * static_cast<double>(s.migFrom[i]) + sn * static_cast<double>(s.migTo[i]);
        normalise(g);
        s.gains.assign(g.begin(), g.end());
        if (s.migrateProgress >= 1.0) {
          if (s.loadOutput >= 0) --loadCount_[static_cast<std::size_t>(s.loadOutput)];
          s.home = s.migrateTo;
          s.loadOutput = s.home;
          ++loadCount_[static_cast<std::size_t>(s.home)];
          s.migrated = true;
          s.migrateTo = -1;
          s.migrateProgress = -1.0;
          s.gains = s.migTo;
        }
      }
      break;
    }
    case SpatialAlgorithm::Mono:
      break;
  }
  return s.gains;
}

void SpatialPolicy::releaseTalker(int slot) {
  Slot& s = slotRef(slot);
  if (s.placed && s.loadOutput >= 0) --loadCount_[static_cast<std::size_t>(s.loadOutput)];
  s.placed = false;
  s.loadOutput = -1;
}

const GainVector& SpatialPolicy::gains(int slot) const { return slotRef(slot).gains; }
double SpatialPolicy::slotPan(int slot) const { return slotRef(slot).pan; }
double SpatialPolicy::slotAzimuthDeg(int slot) const { return slotRef(slot).azimuth; }
int SpatialPolicy::slotHome(int slot) const { return slotRef(slot).home; }
bool SpatialPolicy::slotMigrated(int slot) const { return slotRef(slot).migrated; }

}  // namespace bf
