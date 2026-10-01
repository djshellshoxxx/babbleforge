// bfbench — DSP load measurement tool (docs/ENGINE.md §7).
//
//   bfbench --seconds 60 (--corpus <root> | --synthetic-corpus <speakers>)
//           [--json output.json] [--data-dir <dir>] [--block <frames>]
//
// Measures callback time per process() call for each configuration in §7's table.
// Outputs: table to stdout, optional JSON file. Exit code 0 always.
//
// Configurations (ENGINE.md §7):
//  1. Stereo, Balanced, 10 talker slots, 48 kHz
//  2. 8 channels, 24 talker slots, 48 kHz
//  3. 16 channels, 48 talker slots (distributed), 48 kHz
//  4. 96 kHz, 8 channels (if engine accepts it)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Version.h"
#include "core/config/DataSet.h"
#include "core/corpus/SyntheticCorpus.h"
#include "core/engine/MaskEngine.h"
#include "core/engine/OfflineRenderer.h"
#include "core/engine/Scenario.h"
#include "core/spatial/OutputLayout.h"
#include "core/strategy/StrategyTypes.h"
#if BF_WITH_CORPUS_DB
#include "core/corpus/CorpusLoader.h"
#endif

#ifndef BF_DEFAULT_DATA_DIR
#define BF_DEFAULT_DATA_DIR "resources/data"
#endif

namespace {

struct Config {
    int numChannels = 0;
    int numTalkerSlots = 0;
    double sampleRate = 48000.0;
    std::string name;
};

struct Result {
    Config cfg;
    bool ok = false;
    std::string error;
    std::vector<double> dspLoads;  // in percent, one per process() call
    double p50 = -1.0, p99 = -1.0, maxLoad = -1.0;
    double totalRealtimeFactor = 0.0;
};

// Convert mono/stereo/4/6/8/16 to OutputLayout
bf::OutputLayout makeLayout(int channels) {
    if (channels == 1) {
        return bf::makeMonoLayout();
    } else if (channels == 2) {
        return bf::makeStereoLayout();
    } else if (channels == 4) {
        return bf::makeRing4Layout();
    } else if (channels == 6) {
        return bf::makeRing6Layout();
    } else if (channels == 8) {
        return bf::makeRing8Layout();
    } else if (channels == 16) {
        return bf::makeCustomRingLayout(16);
    }
    return bf::makeStereoLayout();  // fallback
}

// Build a simple preset for the benchmark
nlohmann::json makePreset(int numTalkerSlots) {
    nlohmann::json preset = nlohmann::json::object();
    preset["schema"] = "babbleforge.preset";
    preset["schemaVersion"] = "1.0";
    preset["area"] = "office";
    preset["strategy"] = "balanced";

    // Strategy-specific parameters
    preset["strategies"]["balanced"]["pool"] = numTalkerSlots;
    preset["strategies"]["balanced"]["minActive"] = 2;
    preset["strategies"]["balanced"]["maxActive"] = numTalkerSlots;
    preset["strategies"]["balanced"]["meanActive"] = numTalkerSlots / 2.0;
    preset["strategies"]["balanced"]["spectrumTarget"] = "universal-ltass";

    preset["output"]["layout"] = "auto";
    preset["output"]["spread"] = 0.6;
    preset["macros"]["voiceAmount"] = 0.5;
    preset["macros"]["character"] = 0.5;

    return preset;
}

Result benchmarkConfig(const Config& cfg, const bf::CorpusHandle& corpus,
                       const bf::DataSet& ds, double durationS, int blockSize) {
    Result result;
    result.cfg = cfg;

    // Create a preset
    nlohmann::json presetDoc = makePreset(cfg.numTalkerSlots);

    // Build the plan
    bf::CorpusSummary corpusSummary = bf::CorpusSummary::from(*corpus.snapshot);
    bf::ScenarioPlan sp = bf::buildScenarioPlan(ds, presetDoc, std::nullopt, corpusSummary, 1u);

    if (!sp.ok) {
        result.error = "Failed to build plan: " + sp.error;
        return result;
    }

    // Create engine config
    bf::MaskEngineConfig cfg_engine;
    cfg_engine.seed = 1;
    cfg_engine.corpus = corpus.snapshot;
    cfg_engine.audio = corpus.audio;

    // Create engine
    bf::MaskEngine engine(cfg_engine);

    // Prepare engine
    bf::OutputLayout layout = makeLayout(cfg.numChannels);
    std::string prepare_err;
    if (!engine.prepare(cfg.sampleRate, layout, blockSize, &prepare_err)) {
        result.error = "Engine prepare failed: " + prepare_err;
        return result;
    }

    // Set initial plan
    if (!engine.setPlan(sp.plan, 0, &prepare_err)) {
        result.error = "Failed to set plan: " + prepare_err;
        return result;
    }

    // Allocate output buffers
    std::vector<std::vector<float>> outBuffers(cfg.numChannels, std::vector<float>(blockSize));
    std::vector<float*> outPtrs(cfg.numChannels);
    for (int i = 0; i < cfg.numChannels; ++i) {
        outPtrs[i] = outBuffers[i].data();
    }

    // Render and measure
    int64_t totalFrames = static_cast<int64_t>(durationS * cfg.sampleRate);
    double bufferDurationS = static_cast<double>(blockSize) / cfg.sampleRate;
    std::vector<double> measuredTimes;  // in microseconds
    measuredTimes.reserve(totalFrames / blockSize + 1);

    auto startTime = std::chrono::high_resolution_clock::now();
    for (int64_t pos = 0; pos < totalFrames; pos += blockSize) {
        int nFrames = std::min(static_cast<int>(blockSize), static_cast<int>(totalFrames - pos));

        auto callStart = std::chrono::high_resolution_clock::now();
        engine.process(outPtrs.data(), nFrames);
        auto callEnd = std::chrono::high_resolution_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(callEnd - callStart).count();
        measuredTimes.push_back(static_cast<double>(duration));
    }
    auto endTime = std::chrono::high_resolution_clock::now();

    // Convert times to DSP load percentages
    double bufferDurationUs = bufferDurationS * 1e6;
    for (double timeUs : measuredTimes) {
        double dspLoad = (timeUs / bufferDurationUs) * 100.0;
        result.dspLoads.push_back(dspLoad);
    }

    // Calculate statistics
    if (!result.dspLoads.empty()) {
        std::vector<double> sorted = result.dspLoads;
        std::sort(sorted.begin(), sorted.end());

        result.p50 = sorted[sorted.size() / 2];
        result.p99 = sorted[static_cast<size_t>(static_cast<double>(sorted.size()) * 0.99)];
        result.maxLoad = sorted.back();

        double totalTimeS = std::chrono::duration_cast<std::chrono::duration<double>>(endTime - startTime).count();
        result.totalRealtimeFactor = durationS / totalTimeS;
    }

    result.ok = true;
    return result;
}

int usage(const char* msg = nullptr) {
    if (msg) std::cerr << "bfbench: " << msg << "\n";
    std::cerr << "usage:\n"
                 "  bfbench --seconds N (--corpus <root> | --synthetic-corpus <speakers>)\n"
                 "          [--json output.json] [--data-dir <dir>] [--block N]\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string corpusRoot, jsonPath, dataDir = BF_DEFAULT_DATA_DIR;
    int synthSpeakers = 0;
    double durationS = 60.0;
    int blockSize = 480;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage(("missing value for " + a).c_str());
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "--seconds") durationS = std::atof(next().c_str());
        else if (a == "--corpus") corpusRoot = next();
        else if (a == "--synthetic-corpus") synthSpeakers = std::atoi(next().c_str());
        else if (a == "--json") jsonPath = next();
        else if (a == "--data-dir") dataDir = next();
        else if (a == "--block") blockSize = std::atoi(next().c_str());
        else if (a == "--version") {
            std::cout << "bfbench " << bf::versionString() << "\n";
            return 0;
        } else if (a == "--help" || a == "-h") {
            return usage();
        } else {
            return usage(("unknown argument " + a).c_str());
        }
    }

    if (!corpusRoot.empty() == (synthSpeakers > 0)) {
        return usage("exactly one of --corpus / --synthetic-corpus is required");
    }
    if (blockSize < 1 || blockSize > (1 << 16)) {
        return usage("--block must be 1..65536");
    }

    // Load dataset
    const bf::DataSetResult dsr = bf::loadDataSet(dataDir);
    if (!dsr.ok) {
        std::cerr << "bfbench: data set: " << dsr.error << "\n";
        return 1;
    }

    // Load corpus
    bf::CorpusHandle corpus;
    std::unique_ptr<bf::SyntheticCorpus> synth;
#if BF_WITH_CORPUS_DB
    bf::LoadedCorpus loaded;
#endif
    if (synthSpeakers > 0) {
        bf::SyntheticCorpusParams p;
        p.numSpeakers = static_cast<std::uint32_t>(synthSpeakers);
        p.recordingsPerSpeaker = 2;
        p.recordingSeconds = 60.0;
        synth = std::make_unique<bf::SyntheticCorpus>(p);
        corpus.snapshot = synth->snapshot();
        corpus.audio = synth.get();
    } else if (!corpusRoot.empty()) {
#if BF_WITH_CORPUS_DB
        std::string err;
        if (!bf::loadCorpus(corpusRoot, loaded, &err)) {
            std::cerr << "bfbench: corpus: " << err << "\n";
            return 1;
        }
        corpus.snapshot = loaded.snapshot;
        corpus.audio = loaded.audio.get();
#else
        std::cerr << "bfbench: built without corpus database (BF_WITH_CORPUS_DB=OFF)\n";
        return 1;
#endif
    }

    // Define benchmark configurations (ENGINE.md §7)
    std::vector<Config> configs = {
        {2, 10, 48000.0, "Stereo/10 Balanced"},
        {8, 24, 48000.0, "8ch/24 slots"},
        {16, 48, 48000.0, "16ch/48 distributed"},
        {8, 24, 96000.0, "96kHz/8ch"}  // if supported
    };

    std::vector<Result> results;
    std::cout << "Running DSP load benchmark (" << durationS << " seconds per config)...\n";

    for (const auto& cfg : configs) {
        std::cout << "  Testing " << cfg.name << " (" << cfg.numChannels << "ch, "
                  << cfg.sampleRate << " Hz)... ";
        std::cout.flush();

        Result r = benchmarkConfig(cfg, corpus, dsr.data, durationS, blockSize);

        if (r.ok) {
            std::cout << "done\n";
            results.push_back(r);
        } else {
            // Check if it's a "skipped" case (96 kHz might not be supported)
            if (cfg.sampleRate == 96000.0) {
                std::cout << "skipped: " << r.error << "\n";
            } else {
                std::cout << "error: " << r.error << "\n";
                results.push_back(r);
            }
        }
    }

    // Print results table
    std::cout << "\n";
    std::cout << "DSP Load Results (buffer size " << blockSize << " frames)\n";
    std::cout << "================================================================\n";
    std::cout << std::left << std::setw(28) << "Configuration"
              << std::right << std::setw(12) << "p50 (%)"
              << std::setw(12) << "p99 (%)"
              << std::setw(12) << "max (%)"
              << std::setw(12) << "RT Factor\n";
    std::cout << "================================================================\n";

    for (const auto& r : results) {
        if (r.ok) {
            std::cout << std::left << std::setw(28) << r.cfg.name
                      << std::right << std::fixed << std::setprecision(2)
                      << std::setw(12) << r.p50
                      << std::setw(12) << r.p99
                      << std::setw(12) << r.maxLoad
                      << std::setw(12) << r.totalRealtimeFactor
                      << "\n";
        }
    }
    std::cout << "================================================================\n";

    // Write JSON output if requested
    if (!jsonPath.empty()) {
        nlohmann::json j = nlohmann::json::array();
        for (const auto& r : results) {
            if (r.ok) {
                nlohmann::json item;
                item["config"]["channels"] = r.cfg.numChannels;
                item["config"]["talkerSlots"] = r.cfg.numTalkerSlots;
                item["config"]["sampleRate"] = r.cfg.sampleRate;
                item["config"]["name"] = r.cfg.name;
                item["metrics"]["p50Percent"] = r.p50;
                item["metrics"]["p99Percent"] = r.p99;
                item["metrics"]["maxPercent"] = r.maxLoad;
                item["metrics"]["realtimeFactor"] = r.totalRealtimeFactor;
                j.push_back(item);
            }
        }

        std::ofstream out(jsonPath);
        if (out) {
            out << j.dump(2) << "\n";
            std::cout << "JSON output written to " << jsonPath << "\n";
        } else {
            std::cerr << "bfbench: failed to write " << jsonPath << "\n";
        }
    }

    return 0;
}
