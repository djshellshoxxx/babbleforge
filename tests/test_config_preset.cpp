#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#include <nlohmann/json.hpp>

#include "core/config/AtomicFile.h"
#include "core/config/Compose.h"
#include "core/config/DataSet.h"
#include "core/config/Preset.h"

using Catch::Approx;
namespace {

std::string exampleText() {
  std::string s;
  REQUIRE(bf::readFile(std::string(BF_TEST_DATA_DIR) + "/example.bfpreset", s));
  return s;
}

bool hasAdjustment(const bf::PresetParseResult& r, const std::string& path) {
  return std::any_of(r.adjustments.begin(), r.adjustments.end(),
                     [&](const bf::Adjustment& a) { return a.fieldPath == path; });
}

std::string mutate(void (*f)(nlohmann::json&)) {
  auto j = nlohmann::json::parse(exampleText());
  f(j);
  return j.dump();
}

}  // namespace

TEST_CASE("All data files load", "[config][dataset]") {
  auto r = bf::loadDataSet(BF_DATA_DIR);
  INFO(r.error);
  REQUIRE(r.ok);
  CHECK(r.data.areas.size() == 9);
  CHECK(r.data.strategies.size() == 7);
  CHECK(r.data.targets.size() == 6);
  CHECK(r.data.characterAnchors.size() == 3);
  CHECK(r.data.cvrMappings.size() == 3);
  CHECK(r.data.voiceAmountAnchors.size() == 3);
  CHECK(r.data.engineDefaults.advancedMinDb == Approx(-40.0));
  CHECK(r.data.areas.at("office").talkers.pool == 14);
  CHECK(r.data.targets.at("pink").bandCentersHz.size() == r.data.targets.at("pink").levelsDb.size());
  CHECK(r.data.dataSetHash.size() == 64);
  auto r2 = bf::loadDataSet(BF_DATA_DIR);
  CHECK(r2.data.dataSetHash == r.data.dataSetHash);
  CHECK_FALSE(bf::loadDataSet("/nonexistent/dir").ok);
}

TEST_CASE("PRESETS.md example loads", "[config][preset]") {
  auto r = bf::parsePreset(exampleText());
  INFO(r.error);
  REQUIRE(r.ok);
  CHECK(r.warnings.empty());
  CHECK(r.adjustments.empty());
  CHECK(r.preset.name == "Server Room Privacy");
  CHECK(r.preset.area == "open_office");
  CHECK(*r.preset.macros.babbleFraction == Approx(0.5));
  CHECK(*r.preset.talkers.pool == 20);
  CHECK_FALSE(r.preset.talkers.segmentMedianS.has_value());
  CHECK(r.preset.outputs.channels.size() == 4);
  CHECK(r.preset.outputs.channels[2].gainDb == Approx(-1.5));
  CHECK(*r.preset.outputs.limiterCeilingDbtp == Approx(-1.0));
  CHECK_FALSE(r.preset.seed.has_value());
}

TEST_CASE("Unknown and x- fields round-trip; content hash is stable", "[config][preset]") {
  auto r = bf::parsePreset(mutate([](nlohmann::json& j) {
    j["futureField"] = {{"a", 1}};
    j["talkers"]["futureTalker"] = "keep";
    j["outputs"]["channels"][1]["x-note"] = "hi";
    j["overrides"] = {{"anything", 3}};
  }));
  REQUIRE(r.ok);
  std::string out = bf::serializePreset(r.preset);
  auto j = nlohmann::json::parse(out);
  CHECK(j["x-experiment"]["condition"] == "B");
  CHECK(j["futureField"]["a"] == 1);
  CHECK(j["talkers"]["futureTalker"] == "keep");
  CHECK(j["outputs"]["channels"][1]["x-note"] == "hi");
  CHECK(j["overrides"]["anything"] == 3);
  CHECK(j["contentHash"].get<std::string>().rfind("sha256:", 0) == 0);
  CHECK(j["contentHash"].get<std::string>().size() == 7 + 64);

  auto r2 = bf::parsePreset(out);
  REQUIRE(r2.ok);
  CHECK(r2.preset.contentHash == bf::computePresetContentHash(r.preset));
  CHECK(bf::computePresetContentHash(r2.preset) == r2.preset.contentHash);
  CHECK(bf::serializePreset(r2.preset) == out);  // canonical & idempotent
  CHECK(out.find("\n  \"area\"") != std::string::npos);  // 2-space indent, sorted keys
}

TEST_CASE("Schema versioning", "[config][preset]") {
  auto minor = bf::parsePreset(mutate([](nlohmann::json& j) { j["schemaVersion"] = "1.7"; }));
  REQUIRE(minor.ok);
  CHECK(minor.warnings.size() == 1);

  auto major = bf::parsePreset(mutate([](nlohmann::json& j) { j["schemaVersion"] = "2.0"; }));
  CHECK_FALSE(major.ok);
  CHECK(major.error.find("Created by a newer BabbleForge") != std::string::npos);

  CHECK_FALSE(bf::parsePreset(mutate([](nlohmann::json& j) { j["schema"] = "other"; })).ok);
  CHECK_FALSE(bf::parsePreset(mutate([](nlohmann::json& j) { j["schemaVersion"] = "one"; })).ok);
  CHECK_FALSE(bf::parsePreset("not json").ok);
}

TEST_CASE("Type errors reject with field path", "[config][preset]") {
  auto r = bf::parsePreset(mutate([](nlohmann::json& j) { j["macros"]["character"] = "high"; }));
  CHECK_FALSE(r.ok);
  CHECK(r.error.find("macros.character") != std::string::npos);

  r = bf::parsePreset(mutate([](nlohmann::json& j) { j["outputs"]["channels"][2]["gainDb"] = true; }));
  CHECK_FALSE(r.ok);
  CHECK(r.error.find("outputs.channels[2].gainDb") != std::string::npos);

  r = bf::parsePreset(mutate([](nlohmann::json& j) { j["talkers"] = 5; }));
  CHECK_FALSE(r.ok);
  CHECK(r.error.find("talkers") != std::string::npos);
}

TEST_CASE("Out-of-range values are clamped and recorded", "[config][preset]") {
  auto r = bf::parsePreset(mutate([](nlohmann::json& j) {
    j["macros"]["strengthDb"] = -80.0;
    j["outputs"]["channels"][0]["gainDb"] = 12.0;
    j["outputs"]["channels"][0]["delayMs"] = 500.0;
    j["outputs"]["zones"][0]["levelDb"] = -60.0;
    j["outputs"]["zones"][0]["babbleFractionOffset"] = 0.9;
    j["outputs"]["limiter"]["ceilingDbtp"] = 0.0;
    j["talkers"]["pool"] = 10;  // min 6 <= mean 8.5 <= max 11 > pool 10
  }));
  REQUIRE(r.ok);
  CHECK(*r.preset.macros.strengthDb == Approx(-40.0));
  CHECK(r.preset.outputs.channels[0].gainDb == Approx(6.0));
  CHECK(r.preset.outputs.channels[0].delayMs == Approx(100.0));
  CHECK(r.preset.outputs.zones[0].levelDb == Approx(-24.0));
  CHECK(r.preset.outputs.zones[0].babbleFractionOffset == Approx(0.5));
  CHECK(*r.preset.outputs.limiterCeilingDbtp == Approx(-0.1));
  CHECK(*r.preset.talkers.maxActive == 10);
  CHECK(hasAdjustment(r, "macros.strengthDb"));
  CHECK(hasAdjustment(r, "outputs.channels[0].gainDb"));
  CHECK(hasAdjustment(r, "outputs.limiter.ceilingDbtp"));
  CHECK(hasAdjustment(r, "talkers.maxActive"));
  auto a = *std::find_if(r.adjustments.begin(), r.adjustments.end(),
                         [](const bf::Adjustment& x) { return x.fieldPath == "macros.strengthDb"; });
  CHECK(a.from == Approx(-80.0));
  CHECK(a.to == Approx(-40.0));
  CHECK_FALSE(a.reason.empty());
}

TEST_CASE("compose: office + balanced", "[config][compose]") {
  auto ds = bf::loadDataSet(BF_DATA_DIR);
  REQUIRE(ds.ok);
  bf::Preset p;
  p.area = "office";
  p.strategy = "balanced";
  auto c = bf::compose(ds.data, p);
  REQUIRE(c.ok);
  CHECK(c.config.meanActive == Approx(6.5));
  CHECK(c.config.pool == 14);
  CHECK(c.config.minActive == 4);
  CHECK(c.config.maxActive == 9);
  CHECK(c.config.stationaryFraction() == Approx(0.30));
  CHECK(c.config.character == Approx(0.55));
  CHECK(c.config.lfTrimDb125 == Approx(-1.5));
  CHECK(c.config.limiterCeilingDbtp == Approx(-1.0));
  CHECK(c.adjustments.empty());

  p.strategy = "multi_voice";
  CHECK(bf::compose(ds.data, p).config.babbleFraction == Approx(1.0));
  p.strategy = "dense";  // characterDefault 0.9 (same default the plan uses)
  auto d = bf::compose(ds.data, p);
  CHECK(d.config.character == Approx(0.9));
  p.strategy = "nope";
  CHECK_FALSE(bf::compose(ds.data, p).ok);
}

TEST_CASE("compose: preset fields override area/strategy", "[config][compose]") {
  auto ds = bf::loadDataSet(BF_DATA_DIR);
  REQUIRE(ds.ok);
  auto r = bf::parsePreset(exampleText());
  REQUIRE(r.ok);
  auto c = bf::compose(ds.data, r.preset);
  REQUIRE(c.ok);
  CHECK(c.config.pool == 20);
  CHECK(c.config.meanActive == Approx(8.5));
  CHECK(c.config.babbleFraction == Approx(0.5));
  CHECK(c.config.character == Approx(0.65));
  CHECK(c.config.spread == Approx(0.85));
  CHECK(c.config.channels.size() == 4);
  CHECK(c.config.segmentMaxS.value() == Approx(15.0));
}
