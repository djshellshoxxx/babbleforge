// bfcorpus: offline corpus analyzer / inspector (docs/CORPUS.md §3-§5).
//   bfcorpus import <inputDir> <corpusRoot> [--speakers speakers.csv] [--speaker-regex RE] [--threads N]
//                   [--no-source-paths] [--add] [--verbose]
//   bfcorpus remove <corpusRoot> [--speaker id]... [--recording id]...
//   bfcorpus info <corpusRoot>
//
// `import --add` appends the recordings of <inputDir> to the existing corpus (ids and cache files
// of the existing recordings are kept, duplicates are detected against them, the speaker
// aggregates, PCA basis and corpusVersion are recomputed). `remove` disables speakers / recordings
// and rebuilds the corpusVersion.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/config/AtomicFile.h"
#include "core/corpus/CorpusDb.h"
#include "core/corpus/CorpusImporter.h"

namespace {

const char* className(int c) { return c == 2 ? "Good" : c == 1 ? "Usable" : "Rejected"; }

void usage() {
    std::cerr << "usage:\n"
                 "  bfcorpus import <inputDir> <corpusRoot> [--speakers speakers.csv] [--speaker-regex RE]\n"
                 "                  [--threads N] [--no-source-paths] [--add] [--verbose]\n"
                 "  bfcorpus remove <corpusRoot> [--speaker id]... [--recording id]...\n"
                 "  bfcorpus info <corpusRoot>\n";
}

std::string hms(double s) {
    char b[48];
    const long t = static_cast<long>(s + 0.5);
    std::snprintf(b, sizeof b, "%ldh %02ldm %02lds", t / 3600, (t / 60) % 60, t % 60);
    return b;
}

int cmdImport(int argc, char** argv) {
    if (argc < 4) { usage(); return 2; }
    bf::ImportOptions opt;
    opt.inputDir = argv[2];
    opt.corpusRoot = argv[3];
    bool verbose = false;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--speakers") { const char* v = val(); if (!v) { usage(); return 2; } opt.speakersCsv = v; }
        else if (a == "--speaker-regex") { const char* v = val(); if (!v) { usage(); return 2; } opt.speakerRegex = v; }
        else if (a == "--threads") { const char* v = val(); if (!v) { usage(); return 2; } opt.threads = std::atoi(v); }
        else if (a == "--no-source-paths") opt.storeSourcePaths = false;
        else if (a == "--add") opt.addToExisting = true;
        else if (a == "--verbose") verbose = true;
        else { std::cerr << "unknown option: " << a << "\n"; usage(); return 2; }
    }
    opt.progress = [&](std::size_t done, std::size_t total, const std::string& f) {
        std::fprintf(stderr, "\r[%zu/%zu] %-60.60s", done, total, f.c_str());
        if (done == total) std::fprintf(stderr, "\n");
    };
    const auto r = bf::importCorpus(opt);
    if (!r.ok) { std::cerr << "import failed: " << r.error << "\n"; return 1; }
    for (const auto& f : r.files) {
        if (!verbose && f.cls != bf::ingest::QualityClass::Rejected) continue;
        std::string reasons;
        for (const auto& c : f.reasons) reasons += (reasons.empty() ? "" : ",") + c;
        std::printf("%-8s %-40s q=%5.1f snr=%5.1f dB asl=%6.1f dBFS  %s\n",
                    className(static_cast<int>(f.cls)), f.relPath.c_str(), f.qualityScore, f.snrDb, f.aslDb, reasons.c_str());
    }
    if (r.added)
        std::printf("added to existing corpus (previous version %s): %zu new speakers, %zu recordings in total\n",
                    r.previousVersion.c_str(), r.nNewSpeakers, r.nTotalFiles);
    std::printf("corpusVersion %s\nfiles %zu: good %zu, usable %zu, rejected %zu; speakers %zu (%zu usable)\n"
                "duration %s, usable %s, usable speech %s\n",
                r.corpusVersion.c_str(), r.files.size(), r.nGood, r.nUsable, r.nRejected, r.nSpeakers,
                r.nUsableSpeakers, hms(r.totalS).c_str(), hms(r.usableS).c_str(), hms(r.usableSpeechS).c_str());
    return 0;
}

int cmdRemove(int argc, char** argv) {
    if (argc < 3) { usage(); return 2; }
    bf::RemoveOptions opt;
    opt.corpusRoot = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--speaker") { const char* v = val(); if (!v) { usage(); return 2; } opt.speakers.emplace_back(v); }
        else if (a == "--recording") { const char* v = val(); if (!v) { usage(); return 2; } opt.recordings.push_back(std::atoll(v)); }
        else { std::cerr << "unknown option: " << a << "\n"; usage(); return 2; }
    }
    const auto r = bf::removeFromCorpus(opt);
    if (!r.ok) { std::cerr << "remove failed: " << r.error << "\n"; return 1; }
    std::printf("removed %zu speakers, %zu recordings\nversion %s -> %s; speakers %zu (%zu usable)\n", r.speakersRemoved,
                r.recordingsRemoved, r.previousVersion.c_str(), r.corpusVersion.c_str(), r.nSpeakers, r.nUsableSpeakers);
    return 0;
}

int cmdInfo(int argc, char** argv) {
    if (argc < 3) { usage(); return 2; }
    const std::filesystem::path root = argv[2];
    std::string err;
    auto db = bf::CorpusDb::open((root / "corpus.sqlite").string(), true, &err);
    if (!db) { std::cerr << err << "\n"; return 1; }
    std::string version = "?", analyzer = "?", created = "?";
    db->getInfo("corpusVersion", version);
    db->getInfo("analyzerVersion", analyzer);
    db->getInfo("created", created);
    std::string man;
    if (bf::readFile(root / "manifest.json", man)) {
        const auto j = nlohmann::json::parse(man, nullptr, false);
        if (!j.is_discarded() && j.value("corpusVersion", "") != version)
            std::cerr << "warning: manifest.json corpusVersion differs from the database\n";
    }
    const auto recs = db->recordings();
    const auto spk = db->speakers();
    std::size_t cls[3] = {0, 0, 0};
    double total = 0, usable = 0, usableSpeech = 0;
    std::map<std::string, std::size_t> reasons;
    for (const auto& r : recs) {
        ++cls[std::clamp(r.qualityClass, 0, 2)];
        total += r.durationS;
        if (r.qualityClass > 0) { usable += r.durationS; usableSpeech += r.speechS; }
        else {
            std::size_t p = 0;
            while (p < r.reasonCodes.size()) {
                auto e = r.reasonCodes.find(',', p);
                if (e == std::string::npos) e = r.reasonCodes.size();
                const std::string c = r.reasonCodes.substr(p, e - p);
                if (c.rfind("warn.", 0) != 0 && c != "lossy" && c != "reverberant" && c != "multichannelSelected" && !c.empty()) ++reasons[c];
                p = e + 1;
            }
        }
    }
    std::size_t usableSpk = 0, possibleDup = 0;
    std::set<std::string> langs;
    for (const auto& s : spk) {
        if (s.enabled) { ++usableSpk; if (!s.language.empty()) langs.insert(s.language); }
        if (s.flags & 1) ++possibleDup;
    }
    std::printf("corpus        %s\nversion       %s\nanalyzer      %s\ncreated       %s\n", root.string().c_str(), version.c_str(), analyzer.c_str(), created.c_str());
    std::printf("recordings    %zu (Good %zu, Usable %zu, Rejected %zu)\n", recs.size(), cls[2], cls[1], cls[0]);
    std::printf("speakers      %zu (%zu usable, %zu flagged speaker.possibleDuplicate)\n", spk.size(), usableSpk, possibleDup);
    std::printf("duration      total %s, usable %s, usable speech %s\n", hms(total).c_str(), hms(usable).c_str(), hms(usableSpeech).c_str());
    std::printf("segments      %lld anchors, %lld speech regions\n", static_cast<long long>(db->countRows("segment")), static_cast<long long>(db->countRows("speech_region")));
    std::string l;
    for (const auto& x : langs) l += (l.empty() ? "" : ", ") + x;
    std::printf("languages     %s\n", l.empty() ? "(unknown)" : l.c_str());
    if (!reasons.empty()) {
        std::printf("rejections:\n");
        for (const auto& [c, n] : reasons) std::printf("  %-22s %zu\n", c.c_str(), n);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    const std::string cmd = argv[1];
    if (cmd == "import") return cmdImport(argc, argv);
    if (cmd == "remove") return cmdRemove(argc, argv);
    if (cmd == "info") return cmdInfo(argc, argv);
    usage();
    return 2;
}
