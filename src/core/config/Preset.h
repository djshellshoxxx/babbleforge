#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "core/config/Types.h"

namespace bf {

constexpr int kPresetSchemaMajor = 1;
constexpr int kPresetSchemaMinor = 0;

struct PresetParseResult {
  bool ok = false;
  Preset preset;
  std::vector<Adjustment> adjustments;
  std::vector<std::string> warnings;
  std::string error;  // set when !ok; includes the path of the offending field for type errors
};

// Parses and validates a preset document. Out-of-range values are clamped (recorded in
// `adjustments`); type errors, a wrong schema id or a newer major version reject the file.
PresetParseResult parsePreset(std::string_view json);

// Canonical JSON: sorted keys, 2-space indent, unknown fields preserved,
// contentHash recomputed.
std::string serializePreset(const Preset& p);

// Canonical JSON of the preset with the contentHash field removed (what is hashed).
std::string canonicalPresetJson(const Preset& p);

// "sha256:" + hex(SHA-256(canonicalPresetJson(p))).
std::string computePresetContentHash(const Preset& p);

}  // namespace bf
