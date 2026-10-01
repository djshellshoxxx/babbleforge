#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/config/Types.h"

namespace bf {

struct DataSet {
  std::map<std::string, AreaModel> areas;
  std::map<std::string, StrategyDef> strategies;
  std::vector<CharacterAnchor> characterAnchors;
  std::vector<CvrMapping> cvrMappings;
  std::vector<VoiceAmountAnchor> voiceAmountAnchors;
  // Macro interpolation metadata (see resources/data/macros/*.json).
  std::map<std::string, std::string> characterInterp;  // parameter -> linear|log|step
  std::vector<double> characterStepThresholds;
  double levelVarTruncationSigma = 2.0;
  std::map<std::string, std::string> cvrInterp;
  std::vector<double> cvrStepThresholds;
  VoiceAmountRules voiceAmountRules;
  std::map<std::string, SpectrumTargetDef> targets;
  EngineDefaults engineDefaults;
  // Lower-case hex SHA-256 over all files under the data dir, sorted by generic relative
  // path; for each file: path bytes, 0x00, 8-byte little-endian size, file bytes.
  std::string dataSetHash;
};

struct DataSetResult {
  bool ok = false;
  DataSet data;
  std::string error;  // includes the offending file
};

DataSetResult loadDataSet(const std::filesystem::path& dataDir);

}  // namespace bf
