// Offline renderer (bfrender library path), experiment matrix, scenario parsing and bfanalyze
// metrics (docs/VALIDATION.md §2).
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "core/analysis/OfflineAnalysis.h"
#include "core/io/WavWriter.h"
#include "mask_engine_fixtures.h"

using namespace bf;
namespace fs = std::filesystem;

namespace {

nlohmann::json readJson(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    REQUIRE(f);
    std::stringstream ss;
    ss << f.rdbuf();
    return nlohmann::json::parse(ss.str());
}

fs::path outDir(const std::string& name) {
    const fs::path d = fs::path(BF_TEST_OUT_DIR) / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

}  // namespace

TEST_CASE("bfrender pipeline: scenario file -> float32 WAV + events.json + sidecar.json", "[render]") {
    Scenario sc;
    std::string err;
    INFO(err);
    REQUIRE(loadScenarioFile(fs::path(BF_TEST_DATA_DIR) / "render" / "smoke_scenario.json", sc, &err));
    CHECK(sc.seed == 1001);
    REQUIRE(sc.events.size() == 1);
    RenderOptions opt;
    opt.taps = {0, 3};
    const RenderResult r = renderScenario(bftest::dataSet(), sc, bftest::handle(bftest::shapedCorpus()), opt);
    INFO(r.error);
    REQUIRE(r.ok);
    const fs::path dir = outDir("render_smoke");
    nlohmann::json side;
    REQUIRE(writeRenderOutputs(r, sc, bftest::dataSet(), dir / "smoke.wav", &side, &err));
    for (const char* f : {"smoke.wav", "smoke.events.json", "smoke.sidecar.json", "smoke.T1.wav", "smoke.T4.wav"})
        CHECK(fs::exists(dir / f));

    // WAV: float32, 2 channels, exact duration; SHA-256 of the data matches the sidecar.
    WavData w;
    REQUIRE(readWav((dir / "smoke.wav").string(), w, &err));
    CHECK(w.isFloat);
    CHECK(w.bitsPerSample == 32);
    CHECK(w.channels == 2);
    CHECK(w.frames == 8 * 48000);
    std::vector<std::vector<float>> planar(2, std::vector<float>(w.frames));
    for (std::size_t i = 0; i < w.frames; ++i)
        for (std::size_t c = 0; c < 2; ++c) planar[c][i] = w.samples[i * 2 + c];
    CHECK(audioSha256(planar) == side["audioSha256"].get<std::string>());

    const nlohmann::json s = readJson(dir / "smoke.sidecar.json");
    CHECK(s["schema"] == "babbleforge.render-sidecar/1");
    CHECK(s["seed"] == 1001);
    CHECK(s["corpusVersion"] == bftest::shapedCorpus().snapshot()->corpusVersion());
    CHECK(s["dataSetHash"] == bftest::dataSet().dataSetHash);
    CHECK_FALSE(s["appVersion"].get<std::string>().empty());
    CHECK(s["scenario"]["preset"]["strategy"] == "balanced");
    CHECK(s["planHistory"].size() == 2);
    CHECK(s["planHistory"][1]["sample"] == 4 * 48000);
    for (const char* k : {"level", "spectrum", "temporal", "correlation", "talkers"}) CHECK(s["metrics"].contains(k));
    CHECK(std::fabs(s["metrics"]["level"]["rmsDb"].get<double>() + 26.0) < 1.5);
    CHECK(s["metrics"]["spectrum"].contains("deviation"));
    CHECK(s["engine"]["limiter"]["clipEvents"] == 0);
    CHECK(s["taps"].contains("T1"));

    const nlohmann::json ev = readJson(dir / "smoke.events.json");
    CHECK(ev["schema"] == "babbleforge.events/1");
    REQUIRE(ev["events"].size() > 4);
    for (const auto& e : ev["events"]) CHECK(e["start"].get<std::int64_t>() < 8 * 48000);
}

TEST_CASE("bfrender matrix: Cartesian product of axes + manifest + target-speech TODO", "[render][matrix]") {
    const nlohmann::json m = readJson(fs::path(BF_TEST_DATA_DIR) / "render" / "smoke_matrix.json");
    const fs::path dir = outDir("render_matrix");
    std::string err;
    nlohmann::json manifest;
    const int code = runMatrix(bftest::dataSet(), m, fs::path(BF_TEST_DATA_DIR) / "render",
                               bftest::handle(bftest::shapedCorpus()), dir, {}, &err, &manifest);
    INFO(err);
    CHECK(code == 0);
    REQUIRE(manifest["cells"].size() == 4);
    CHECK(fs::exists(dir / "matrix_manifest.json"));
    CHECK(manifest["targetSpeech"]["status"] == "not implemented");
    std::set<std::string> hashes;
    for (const auto& c : manifest["cells"]) {
        CHECK(c["exitCode"] == 0);
        CHECK(fs::exists(dir / c["wav"].get<std::string>()));
        hashes.insert(c["audioSha256"].get<std::string>());
        // Two-pass laboratory normalisation: every cell at L_ref.
        CHECK(std::fabs(c["rmsDb"].get<double>() + 26.0) < 0.02);
    }
    CHECK(hashes.size() == 4);
    const nlohmann::json s0 = readJson(dir / "cell_000.sidecar.json");
    CHECK(s0["scenario"]["laboratory"]["talkers"] == 1);
    CHECK(s0["laboratory"]["twoPass"].contains("staticGainDb"));
}

TEST_CASE("Scenario parsing: preset path + events + laboratory block + errors", "[render][scenario]") {
    const fs::path dir = outDir("scenario_parse");
    {
        std::ofstream f(dir / "p.json");
        f << R"({"schema":"babbleforge.preset","schemaVersion":"1.0","area":"office","strategy":"dense"})";
    }
    nlohmann::json doc = {{"schema", "babbleforge.scenario"}, {"preset", "p.json"}, {"seed", 5}, {"durationS", 2},
                          {"events", {{{"atS", 1.5}, {"set", {{"a.b", 1}}}}, {{"atS", 0.5}, {"set", nlohmann::json::object()}}}},
                          {"laboratory", {{"mode", "hybrid"}, {"babbleFraction", 0.25}}}};
    Scenario sc;
    std::string err;
    REQUIRE(parseScenario(doc, dir, sc, &err));
    CHECK(sc.preset["strategy"] == "dense");
    CHECK(sc.events.front().atS == 0.5);
    REQUIRE(sc.lab);
    CHECK(sc.lab->mode == LabSubMode::Hybrid);
    CHECK(*sc.lab->babbleFraction == 0.25);
    CHECK(sc.lab->twoPassRms);
    CHECK_FALSE(sc.lab->limiter);

    nlohmann::json j = nlohmann::json::object();
    setDottedPath(j, "macros.mix.babbleFraction", 0.5);
    CHECK(j["macros"]["mix"]["babbleFraction"] == 0.5);

    auto bad = doc;
    bad["schema"] = "x";
    CHECK_FALSE(parseScenario(bad, dir, sc, &err));
    bad = doc;
    bad.erase("preset");
    CHECK_FALSE(parseScenario(bad, dir, sc, &err));
    bad = doc;
    bad["laboratory"]["mode"] = "nope";
    CHECK_FALSE(parseScenario(bad, dir, sc, &err));
    bad = doc;
    bad["sampleRate"] = 22050;
    CHECK_FALSE(parseScenario(bad, dir, sc, &err));

    // A babble plan without a corpus is a configuration error (exit code 2).
    auto d2 = bftest::scenarioDoc("office", "balanced", "stereo", 1.0, 1);
    const RenderResult r = renderScenario(bftest::dataSet(), bftest::scenario(d2), {}, {});
    CHECK_FALSE(r.ok);
    CHECK(r.exitCode == kRenderConfigError);
}

TEST_CASE("bfanalyze metrics on known signals", "[render][analyze]") {
    const std::size_t n = 12 * 48000;
    std::vector<std::vector<float>> x(2, std::vector<float>(n));
    for (std::size_t i = 0; i < n; ++i) {
        x[0][i] = static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(i) / 48000.0));
        x[1][i] = x[0][i];
    }
    std::vector<const float*> p = {x[0].data(), x[1].data()};
    nlohmann::json ev = {{"events", {{{"start", 0}, {"end", 48000 * 6}, {"speaker", 1}},
                                     {{"start", 48000 * 3}, {"end", 48000 * 12}, {"speaker", 2}}}}};
    OfflineAnalysisOptions opt;
    opt.events = &ev;
    const nlohmann::json a = analyzeAudio(p.data(), 2, n, 48000.0, opt);
    CHECK(std::fabs(a["level"]["rmsDb"].get<double>() - (20.0 * std::log10(0.5) - 3.0103)) < 0.01);
    CHECK(std::fabs(a["level"]["truePeakDbtp"].get<double>() - 20.0 * std::log10(0.5)) < 0.1);
    CHECK(std::fabs(a["level"]["crestDb"].get<double>() - 3.01) < 0.1);
    CHECK(a["correlation"]["matrix"][0][1].get<double>() > 0.9999);
    const auto third = a["spectrum"]["thirdOctDb"].get<std::vector<double>>();
    CHECK(std::max_element(third.begin(), third.end()) - third.begin() == static_cast<std::ptrdiff_t>(kBand1k));
    CHECK(a["temporal"]["gaps"]["count"] == 0);
    CHECK(a["talkers"]["events"] == 2);
    CHECK(a["talkers"]["speakers"] == 2);
    CHECK(std::fabs(a["talkers"]["meanActive"].get<double>() - 15.0 / 12.0) < 1e-9);
    CHECK(a["talkers"]["minActive"] == 1);
    CHECK(a["talkers"]["maxActive"] == 2);
}
