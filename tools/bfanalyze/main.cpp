// bfanalyze — offline analyzer (docs/VALIDATION.md §2.2).
//
//   bfanalyze file.wav [--events events.json] [--target <id>] [--data-dir <dir>] [--out metrics.json]
//
// Prints (or writes) a JSON document: RMS, LUFS-S/I, true peak, crest factor, 1/3-octave and
// octave levels (+ shape deviation vs the target's ideal band levels with --target), envelope
// L10/L90, modulation spectrum, gap statistics, channel correlation matrix and, with --events,
// talker statistics. Exit codes: 0 ok, 1 usage / I/O error, 2 unknown target.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "core/Version.h"
#include "core/analysis/OfflineAnalysis.h"
#include "core/config/DataSet.h"
#include "core/io/WavWriter.h"
#include "core/spectrum/FirDesigner.h"

#ifndef BF_DEFAULT_DATA_DIR
#define BF_DEFAULT_DATA_DIR "resources/data"
#endif

namespace {
int usage(const char* msg = nullptr) {
    if (msg) std::cerr << "bfanalyze: " << msg << "\n";
    std::cerr << "usage: bfanalyze file.wav [--events events.json] [--target <id>] [--data-dir <dir>] [--out m.json]\n";
    return 1;
}
}  // namespace

int main(int argc, char** argv) {
    std::string wavPath, eventsPath, targetId, outPath, dataDir = BF_DEFAULT_DATA_DIR;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--events" || a == "--target" || a == "--data-dir" || a == "--out") {
            const char* v = next();
            if (!v) return usage(("missing value for " + a).c_str());
            (a == "--events" ? eventsPath : a == "--target" ? targetId : a == "--data-dir" ? dataDir : outPath) = v;
        } else if (a == "--version") {
            std::cout << "bfanalyze " << bf::versionString() << "\n";
            return 0;
        } else if (a == "--help" || a == "-h") {
            return usage();
        } else if (!a.empty() && a[0] == '-') {
            return usage(("unknown argument " + a).c_str());
        } else if (wavPath.empty()) {
            wavPath = a;
        } else {
            return usage("only one input file is supported");
        }
    }
    if (wavPath.empty()) return usage("no input file");

    bf::WavData wav;
    std::string err;
    if (!bf::readWav(wavPath, wav, &err)) {
        std::cerr << "bfanalyze: " << wavPath << ": " << err << "\n";
        return 1;
    }
    const std::size_t nCh = wav.channels, n = static_cast<std::size_t>(wav.frames);
    std::vector<std::vector<float>> planar(nCh, std::vector<float>(n));
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t c = 0; c < nCh; ++c) planar[c][i] = wav.samples[i * nCh + c];
    std::vector<const float*> ptr;
    for (const auto& ch : planar) ptr.push_back(ch.data());

    bf::OfflineAnalysisOptions opt;
    nlohmann::json events;
    if (!eventsPath.empty()) {
        std::ifstream f(eventsPath, std::ios::binary);
        if (!f) {
            std::cerr << "bfanalyze: cannot open " << eventsPath << "\n";
            return 1;
        }
        try {
            std::stringstream ss;
            ss << f.rdbuf();
            events = nlohmann::json::parse(ss.str());
        } catch (const std::exception& e) {
            std::cerr << "bfanalyze: " << eventsPath << ": " << e.what() << "\n";
            return 1;
        }
        opt.events = &events;
    }
    if (!targetId.empty()) {
        const bf::DataSetResult ds = bf::loadDataSet(dataDir);
        if (!ds.ok) {
            std::cerr << "bfanalyze: data set: " << ds.error << "\n";
            return 1;
        }
        const bf::SpectrumTargetDef* def = nullptr;
        for (const auto& [key, d] : ds.data.targets)
            if (key == targetId || d.id == targetId) def = &d;
        if (!def) {
            std::cerr << "bfanalyze: unknown target \"" << targetId << "\"\n";
            return 2;
        }
        const bf::SpectrumTargetBuild b = bf::buildSpectrumTarget(*def);
        if (!b.target) {
            std::cerr << "bfanalyze: target \"" << targetId << "\": " << b.error << "\n";
            return 2;
        }
        bf::FirDesignParams fp;
        fp.fs = wav.sampleRate;
        fp.lfLimitHz = b.target->lfLimitHz;
        fp.hfLimitHz = b.target->hfLimitHz;
        opt.targetDb = bf::idealBandLevelsDb(b.target->effectiveThirdOctDb(), fp);
        opt.targetId = targetId;
    }
    nlohmann::json j = bf::analyzeAudio(ptr.data(), nCh, n, wav.sampleRate, opt);
    j["file"] = wavPath;
    j["tool"] = std::string("bfanalyze ") + bf::versionString();
    const std::string text = j.dump(1);
    if (outPath.empty()) {
        std::cout << text << "\n";
    } else {
        std::ofstream o(outPath, std::ios::binary | std::ios::trunc);
        if (!o || !(o << text << "\n")) {
            std::cerr << "bfanalyze: cannot write " << outPath << "\n";
            return 1;
        }
    }
    return 0;
}
