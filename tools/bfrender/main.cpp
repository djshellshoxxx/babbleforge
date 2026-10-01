// bfrender — deterministic offline renderer (docs/VALIDATION.md §2.1, §2.3).
//
//   bfrender --scenario scenario.json (--corpus <root> | --synthetic-corpus <speakers>)
//            --out out.wav [--events events.json] [--data-dir <dir>] [--no-limiter]
//            [--taps T1,T2,T3,T4] [--block <frames>] [--threads-sync]
//   bfrender --matrix experiment.json --out-dir <dir> (--corpus <root> | --synthetic-corpus <n>)
//            [--data-dir <dir>] [--no-limiter]
//
// Outputs: float32 WAV, <stem>.events.json (D1 timeline), <stem>.sidecar.json (scenario, seed,
// corpus version, data-set hash, app version, engine statistics, bfanalyze metrics, SHA-256 of
// the audio data), optional tap WAVs <stem>.T<k>.wav.
// Exit codes: 0 ok, 1 usage / I/O error, 2 configuration error, 3 source failure (Strict).
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "core/Version.h"
#include "core/config/DataSet.h"
#include "core/corpus/SyntheticCorpus.h"
#include "core/engine/OfflineRenderer.h"
#include "core/engine/Scenario.h"
#if BF_WITH_CORPUS_DB
#include "core/corpus/CorpusLoader.h"
#endif

#ifndef BF_DEFAULT_DATA_DIR
#define BF_DEFAULT_DATA_DIR "resources/data"
#endif

namespace {

int usage(const char* msg = nullptr) {
    if (msg) std::cerr << "bfrender: " << msg << "\n";
    std::cerr << "usage:\n"
                 "  bfrender --scenario s.json (--corpus <root> | --synthetic-corpus <speakers>) --out out.wav\n"
                 "           [--events e.json] [--data-dir <dir>] [--no-limiter] [--taps T1,T2,T3,T4] [--block N]\n"
                 "  bfrender --matrix m.json --out-dir <dir> (--corpus <root> | --synthetic-corpus <n>)\n"
                 "           [--data-dir <dir>] [--no-limiter]\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string scenarioPath, matrixPath, corpusRoot, outPath, outDir, eventsPath, dataDir = BF_DEFAULT_DATA_DIR;
    int synthSpeakers = 0;
    bf::RenderOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage(("missing value for " + a).c_str());
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "--scenario") scenarioPath = next();
        else if (a == "--matrix") matrixPath = next();
        else if (a == "--corpus") corpusRoot = next();
        else if (a == "--synthetic-corpus") synthSpeakers = std::atoi(next().c_str());
        else if (a == "--out") outPath = next();
        else if (a == "--out-dir") outDir = next();
        else if (a == "--events") eventsPath = next();
        else if (a == "--data-dir") dataDir = next();
        else if (a == "--no-limiter") opt.noLimiter = true;
        else if (a == "--threads-sync") {}  // the offline renderer is always synchronous
        else if (a == "--block") opt.blockSize = std::atoi(next().c_str());
        else if (a == "--taps") {
            std::stringstream ss(next());
            std::string t;
            while (std::getline(ss, t, ',')) {
                if (t.size() != 2 || (t[0] != 'T' && t[0] != 't') || t[1] < '1' || t[1] > '4')
                    return usage(("bad tap " + t).c_str());
                opt.taps.push_back(t[1] - '1');
            }
        } else if (a == "--version") {
            std::cout << "bfrender " << bf::versionString() << "\n";
            return 0;
        } else if (a == "--help" || a == "-h") {
            return usage();
        } else {
            return usage(("unknown argument " + a).c_str());
        }
    }
    if (scenarioPath.empty() == matrixPath.empty()) return usage("exactly one of --scenario / --matrix is required");
    if (!scenarioPath.empty() && outPath.empty()) return usage("--out is required with --scenario");
    if (!matrixPath.empty() && outDir.empty()) return usage("--out-dir is required with --matrix");
    if (opt.blockSize < 1 || opt.blockSize > 1 << 16) return usage("--block must be 1..65536");

    const bf::DataSetResult dsr = bf::loadDataSet(dataDir);
    if (!dsr.ok) {
        std::cerr << "bfrender: data set: " << dsr.error << "\n";
        return 2;
    }

    // Corpus.
    bf::CorpusHandle corpus;
    std::unique_ptr<bf::SyntheticCorpus> synth;
#if BF_WITH_CORPUS_DB
    bf::LoadedCorpus loaded;
#endif
    if (!corpusRoot.empty() && synthSpeakers > 0) return usage("--corpus and --synthetic-corpus are exclusive");
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
            std::cerr << "bfrender: corpus: " << err << "\n";
            return 2;
        }
        corpus.snapshot = loaded.snapshot;
        corpus.audio = loaded.audio.get();
#else
        std::cerr << "bfrender: built without the corpus database (BF_WITH_CORPUS_DB=OFF)\n";
        return 2;
#endif
    }

    if (!matrixPath.empty()) {
        std::ifstream f(matrixPath, std::ios::binary);
        if (!f) {
            std::cerr << "bfrender: cannot open " << matrixPath << "\n";
            return 1;
        }
        nlohmann::json m;
        try {
            std::stringstream ss;
            ss << f.rdbuf();
            m = nlohmann::json::parse(ss.str());
        } catch (const std::exception& e) {
            std::cerr << "bfrender: " << matrixPath << ": " << e.what() << "\n";
            return 2;
        }
        std::string err;
        nlohmann::json manifest;
        const int code = bf::runMatrix(dsr.data, m, std::filesystem::path(matrixPath).parent_path(), corpus, outDir,
                                       opt, &err, &manifest);
        if (!err.empty()) std::cerr << "bfrender: " << err << "\n";
        if (manifest.contains("cells"))
            for (const auto& c : manifest["cells"])
                std::cerr << c.value("name", std::string()) << ": exit " << c.value("exitCode", 0)
                          << (c.contains("error") ? " " + c["error"].get<std::string>() : std::string()) << "\n";
        return code;
    }

    bf::Scenario sc;
    std::string err;
    if (!bf::loadScenarioFile(scenarioPath, sc, &err)) {
        std::cerr << "bfrender: " << err << "\n";
        return 2;
    }
    if (!sc.seedGiven) std::cerr << "bfrender: warning: scenario has no seed; using " << sc.seed << "\n";
    const bf::RenderResult r = bf::renderScenario(dsr.data, sc, corpus, opt);
    for (const auto& w : r.warnings) std::cerr << "bfrender: warning: " << w << "\n";
    if (!r.ok) {
        std::cerr << "bfrender: " << r.error << "\n";
        return r.exitCode ? r.exitCode : 2;
    }
    nlohmann::json side;
    if (!bf::writeRenderOutputs(r, sc, dsr.data, outPath, &side, &err, eventsPath)) {
        std::cerr << "bfrender: " << err << "\n";
        return 1;
    }
    std::cout << "bfrender: wrote " << outPath << " (" << r.numChannels << " ch, " << sc.durationS << " s, rms "
              << side["metrics"]["level"]["rmsDb"].get<double>() << " dBFS, sha256 "
              << side["audioSha256"].get<std::string>() << ")\n";
    return 0;
}
