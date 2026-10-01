#include "core/spatial/OutputLayout.h"

#include <cmath>
#include <stdexcept>

namespace bf {

namespace {

float wrap180(float a) {
  while (a > 180.0f) a -= 360.0f;
  while (a <= -180.0f) a += 360.0f;
  return a;
}

OutputLayout ringFromAzimuths(std::string id, const std::vector<float>& az,
                              const std::vector<std::string>& labels = {}) {
  OutputLayout l;
  l.id = std::move(id);
  l.kind = LayoutKind::Ring;
  for (std::size_t i = 0; i < az.size(); ++i) {
    OutputDef o;
    o.index = static_cast<std::uint8_t>(i);
    o.deviceChannel = static_cast<std::uint16_t>(i);
    o.azimuthDeg = az[i];
    o.label = i < labels.size() ? labels[i] : "Output " + std::to_string(i + 1);
    l.outputs.push_back(std::move(o));
  }
  return l;
}

}  // namespace

int OutputLayout::activeCount() const noexcept {
  int n = 0;
  for (const auto& o : outputs) n += o.enabled ? 1 : 0;
  return n;
}

OutputLayout makeMonoLayout() {
  OutputLayout l;
  l.id = "mono";
  l.kind = LayoutKind::Mono;
  OutputDef o;
  o.label = "Mono";
  l.outputs.push_back(o);
  return l;
}

OutputLayout makeStereoLayout() {
  OutputLayout l;
  l.id = "stereo";
  l.kind = LayoutKind::Stereo;
  OutputDef a;
  a.index = 0;
  a.deviceChannel = 0;
  a.label = "Left";
  a.azimuthDeg = 30.0f;
  OutputDef b;
  b.index = 1;
  b.deviceChannel = 1;
  b.label = "Right";
  b.azimuthDeg = -30.0f;
  l.outputs = {a, b};
  return l;
}

OutputLayout makeRing4Layout() {
  return ringFromAzimuths("ring4", {135.0f, 45.0f, -45.0f, -135.0f},
                          {"Rear Left", "Front Left", "Front Right", "Rear Right"});
}

OutputLayout makeRing6Layout() {
  return ringFromAzimuths("ring6", {150.0f, 90.0f, 30.0f, -30.0f, -90.0f, -150.0f});
}

OutputLayout makeRing8Layout() {
  return ringFromAzimuths("ring8", {157.5f, 112.5f, 67.5f, 22.5f, -22.5f, -67.5f, -112.5f,
                                    -157.5f});
}

OutputLayout makeCustomRingLayout(int n, std::span<const float> azimuthsDeg) {
  if (n < 3 || n > 16) throw std::invalid_argument("custom ring needs 3..16 outputs");
  std::vector<float> az;
  if (azimuthsDeg.empty()) {
    for (int i = 0; i < n; ++i)
      az.push_back(wrap180((static_cast<float>(i) + 0.5f) * 360.0f / static_cast<float>(n) -
                           180.0f));
  } else {
    if (static_cast<int>(azimuthsDeg.size()) != n)
      throw std::invalid_argument("azimuth count must equal n");
    for (float a : azimuthsDeg) az.push_back(wrap180(a));
  }
  return ringFromAzimuths("ring" + std::to_string(n), az);
}

OutputLayout makeGridLayout(std::span<const Vec2> positionsM, std::span<const std::uint8_t> zones) {
  const auto n = positionsM.size();
  if (n < 3 || n > 32) throw std::invalid_argument("grid needs 3..32 outputs");
  if (!zones.empty() && zones.size() != n) throw std::invalid_argument("zones size mismatch");
  OutputLayout l;
  l.id = "grid" + std::to_string(n);
  l.kind = LayoutKind::Grid;
  for (std::size_t i = 0; i < n; ++i) {
    OutputDef o;
    o.index = static_cast<std::uint8_t>(i);
    o.deviceChannel = static_cast<std::uint16_t>(i);
    o.label = "Ceiling " + std::to_string(i + 1);
    o.posM = positionsM[i];
    o.zone = zones.empty() ? std::uint8_t{0} : zones[i];
    if (o.zone > 7) throw std::invalid_argument("zone out of range");
    l.outputs.push_back(std::move(o));
  }
  return l;
}

OutputLayout makeRegularGridLayout(int rows, int cols, float pitchM) {
  std::vector<Vec2> pos;
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < cols; ++c)
      pos.push_back({static_cast<float>(c) * pitchM, static_cast<float>(r) * pitchM});
  return makeGridLayout(pos);
}

SpatialAlgorithm selectAlgorithm(const OutputLayout& layout) noexcept {
  const int n = layout.activeCount();
  if (n <= 1) return SpatialAlgorithm::Mono;
  if (n == 2) return SpatialAlgorithm::DistributedStereo;
  if (layout.kind == LayoutKind::Ring && n <= 8) return SpatialAlgorithm::SmallMultichannel;
  return SpatialAlgorithm::LargeDistributed;
}

}  // namespace bf
