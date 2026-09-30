#pragma once
// Output layout model and automatic spatial algorithm selection (docs/SPATIAL_ENGINE.md
// section 2). Control-thread only.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bf {

struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;
};

enum class LayoutKind { Mono, Stereo, Ring, Grid };

enum class SpatialAlgorithm {
  Mono,               // 3.1
  DistributedStereo,  // 3.2
  SmallMultichannel,  // 3.3 VBAP + MDAP
  LargeDistributed    // 3.4 neighbourhood rendering
};

struct OutputDef {
  std::uint8_t index = 0;          // logical output 0..N-1
  std::uint16_t deviceChannel = 0; // physical device channel (0-based); may be sparse
  std::string label;
  float azimuthDeg = 0.0f;         // 0 = front, + = left, -180..180
  float elevationDeg = 0.0f;       // informational in V1
  std::optional<Vec2> posM;        // grid / distributed layouts
  std::uint8_t zone = 0;           // 0..7
  bool enabled = true;
};

struct OutputLayout {
  std::string id;
  LayoutKind kind = LayoutKind::Mono;
  std::vector<OutputDef> outputs;

  int size() const noexcept { return static_cast<int>(outputs.size()); }
  int activeCount() const noexcept;
};

OutputLayout makeMonoLayout();
OutputLayout makeStereoLayout();                 // outputs: Left (+30), Right (-30)
OutputLayout makeRing4Layout();                  // +-45, +-135
OutputLayout makeRing6Layout();                  // +-30, +-90, +-150
OutputLayout makeRing8Layout();                  // +-22.5, +-67.5, +-112.5, +-157.5
// 3..16 outputs. Empty azimuths = equally spaced (symmetric about the front). Otherwise
// azimuths.size() must equal n. Throws std::invalid_argument on bad input.
OutputLayout makeCustomRingLayout(int n, std::span<const float> azimuthsDeg = {});
// 3..32 outputs at the given positions (metres). zones (optional) = zone per output.
OutputLayout makeGridLayout(std::span<const Vec2> positionsM,
                            std::span<const std::uint8_t> zones = {});
// Rows x cols grid with the given pitch (metres), origin at the first output.
OutputLayout makeRegularGridLayout(int rows, int cols, float pitchM);

// Automatic algorithm choice (section 2 table). Uses the number of enabled outputs.
SpatialAlgorithm selectAlgorithm(const OutputLayout& layout) noexcept;

}  // namespace bf
