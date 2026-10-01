#include "core/corpus/CorpusImporter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <array>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "core/config/AtomicFile.h"
#include "core/config/Sha256.h"
#include "core/corpus/CorpusDb.h"
#include "core/corpus/FlacCache.h"
#include "core/corpus/ingest/Analyzer.h"
#include "core/corpus/ingest/Features.h"
#include "core/corpus/ingest/SpeakerIdentity.h"
#include "core/spectrum/SpectrumTarget.h"

namespace bf {

using namespace ingest;
namespace fs = std::filesystem;

namespace {

bool supportedExt(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* exts[] = {".wav", ".wave", ".bwf", ".aif", ".aiff", ".aifc", ".flac", ".mp3", ".ogg", ".oga", ".opus", ".m4a", ".aac"};
    for (const char* x : exts) if (e == x) return true;
    return false;
}

std::string utcNow() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string joinReasons(const std::vector<std::string>& r) {
    std::string s;
    for (const auto& x : r) { if (!s.empty()) s += ","; s += x; }
    return s;
}

struct PcaBasis {
    std::vector<double> mean;                       // 21
    std::array<std::vector<double>, 3> comp;        // 3 x 21
    std::array<double, 3> eigenvalue{};
};

// PCA of mean-removed speaker LTASS vectors (dB over the 21 operating bands), power iteration.
PcaBasis computePca(const std::vector<std::vector<double>>& X, std::vector<std::array<double, 3>>& scores) {
    constexpr std::size_t D = kNumOperatingBands;
    PcaBasis b;
    b.mean.assign(D, 0.0);
    const std::size_t S = X.size();
    scores.assign(S, {0.0, 0.0, 0.0});
    for (auto& c : b.comp) c.assign(D, 0.0);
    if (S == 0) return b;
    for (const auto& x : X) for (std::size_t d = 0; d < D; ++d) b.mean[d] += x[d] / static_cast<double>(S);
    if (S < 2) return b;
    std::vector<std::vector<double>> C(D, std::vector<double>(D, 0.0));
    for (const auto& x : X)
        for (std::size_t i = 0; i < D; ++i)
            for (std::size_t j = 0; j < D; ++j) C[i][j] += (x[i] - b.mean[i]) * (x[j] - b.mean[j]) / static_cast<double>(S - 1);
    for (std::size_t k = 0; k < 3; ++k) {
        std::vector<double> v(D);
        for (std::size_t d = 0; d < D; ++d) v[d] = 1.0 + 0.1 * static_cast<double>((d * (k + 3)) % 7);
        double lambda = 0.0;
        for (int it = 0; it < 500; ++it) {
            std::vector<double> w(D, 0.0);
            for (std::size_t i = 0; i < D; ++i) for (std::size_t j = 0; j < D; ++j) w[i] += C[i][j] * v[j];
            double nrm = 0.0;
            for (double t : w) nrm += t * t;
            nrm = std::sqrt(nrm);
            if (nrm < 1e-15) { lambda = 0.0; break; }
            for (std::size_t d = 0; d < D; ++d) w[d] /= nrm;
            lambda = nrm;
            double diff = 0.0;
            for (std::size_t d = 0; d < D; ++d) diff += std::fabs(w[d] - v[d]);
            v.swap(w);
            if (diff < 1e-12) break;
        }
        // Deterministic sign: the largest-magnitude loading is positive.
        std::size_t am = 0;
        for (std::size_t d = 1; d < D; ++d) if (std::fabs(v[d]) > std::fabs(v[am])) am = d;
        if (v[am] < 0) for (auto& t : v) t = -t;
        b.comp[k] = v;
        b.eigenvalue[k] = lambda;
        for (std::size_t i = 0; i < D; ++i) for (std::size_t j = 0; j < D; ++j) C[i][j] -= lambda * v[i] * v[j];
    }
    for (std::size_t s = 0; s < S; ++s)
        for (std::size_t k = 0; k < 3; ++k) {
            double t = 0.0;
            for (std::size_t d = 0; d < D; ++d) t += (X[s][d] - b.mean[d]) * b.comp[k][d];
            scores[s][k] = t;
        }
    return b;
}

struct SpeakerAgg {
    std::string externalId, language, labels;
    bool unknown = false;
    std::vector<std::size_t> usable;  // indices into analyses
    double speechS = 0;
    double f0Med = 0, f0Mean = 0, f0P5 = 0, f0P95 = 0, rate = 0, centroid = 0, asl = 0, quality = 0;
    std::array<double, kNumLtassBands> power{};
    bool haveF0 = false;
};

double weightedMedian(std::vector<std::pair<double, double>> v) {  // (value, weight)
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    double tot = 0.0;
    for (auto& p : v) tot += p.second;
    double acc = 0.0;
    for (auto& p : v) { acc += p.second; if (acc >= 0.5 * tot) return p.first; }
    return v.back().first;
}

}  // namespace

ImportResult importCorpus(const ImportOptions& opt) {
    ImportResult res;
    auto fail = [&](const std::string& m) { res.ok = false; res.error = m; return res; };
    std::error_code ec;
    if (!fs::is_directory(opt.inputDir, ec)) return fail("input directory not found: " + opt.inputDir.string());

    // 1. scan (sorted, deterministic ids)
    std::vector<std::pair<std::string, fs::path>> files;  // rel path ('/'), abs path
    for (fs::recursive_directory_iterator it(opt.inputDir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || !supportedExt(it->path())) continue;
        files.emplace_back(fs::relative(it->path(), opt.inputDir, ec).generic_string(), it->path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) return fail("no audio files found in " + opt.inputDir.string());

    // 2. speaker identities -> dense ids in sorted external-id order
    SpeakerResolver resolver;
    std::string err;
    if (!resolver.init(opt.speakersCsv, opt.speakerRegex, &err)) return fail(err);
    std::vector<SpeakerAssignment> assign;
    std::set<std::string> ids;
    for (const auto& f : files) { assign.push_back(resolver.resolve(f.first)); ids.insert(assign.back().externalId); }
    std::map<std::string, std::int64_t> speakerIdOf;
    for (const auto& id : ids) speakerIdOf[id] = static_cast<std::int64_t>(speakerIdOf.size() + 1);

    // 3. staging area
    const fs::path staging = opt.corpusRoot / ".staging";
    fs::remove_all(staging, ec);
    fs::create_directories(staging / "cache", ec);
    if (ec) return fail("cannot create staging directory: " + staging.string());

    // 4. analyse in parallel
    const std::size_t nFiles = files.size();
    std::vector<RecordingAnalysis> an(nFiles);
    std::atomic<std::size_t> next{0}, done{0};
    std::mutex progMu;
    std::mutex errMu;
    std::string writeErr;
    const auto cachePath = [&](std::size_t i) {
        return fs::path("cache") / std::to_string(speakerIdOf[assign[i].externalId]) / (std::to_string(i + 1) + ".flac");
    };
    auto worker = [&]() {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= nFiles) return;
            if (opt.cancel && opt.cancel->load()) return;
            RecordingAnalysis a = analyzeFile(files[i].second.string(), opt.analyzer);
            a.sourcePath = files[i].second.string();
            if (!a.decodeFailed) {
                std::string sha;
                {
                    // SHA-256 of the source file bytes.
                    std::ifstream f(files[i].second, std::ios::binary);
                    Sha256 h;
                    std::vector<char> buf(1 << 16);
                    while (f) { f.read(buf.data(), static_cast<std::streamsize>(buf.size())); h.update(buf.data(), static_cast<std::size_t>(f.gcount())); }
                    sha = h.finishHex();
                }
                a.sourceSha256 = sha;
            }
            if (a.qualityClass != QualityClass::Rejected && !a.audio48k.empty()) {
                std::string werr;
                if (!writeFlacCache(staging / cachePath(i), a.audio48k.data(), a.audio48k.size(), &werr)) {
                    std::lock_guard<std::mutex> lk(errMu);
                    writeErr = werr;
                }
            }
            std::vector<float>().swap(a.audio48k);
            std::vector<float>().swap(a.f0.track);
            an[i] = std::move(a);
            const std::size_t d = done.fetch_add(1) + 1;
            if (opt.progress) {
                std::lock_guard<std::mutex> lk(progMu);
                opt.progress(d, nFiles, files[i].first);
            }
        }
    };
    {
        unsigned nt = opt.threads > 0 ? static_cast<unsigned>(opt.threads)
                                      : std::max(1u, std::thread::hardware_concurrency() > 2 ? std::thread::hardware_concurrency() - 2 : 1u);
        nt = std::min<unsigned>(nt, static_cast<unsigned>(nFiles));
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < nt; ++t) pool.emplace_back(worker);
        worker();
        for (auto& t : pool) t.join();
    }
    if (opt.cancel && opt.cancel->load()) {
        fs::remove_all(staging, ec);
        return fail("cancelled");
    }
    if (!writeErr.empty()) return fail(writeErr);

    // 5. duplicate detection (sequential, in id order)
    {
        std::unordered_map<std::string, std::size_t> seen;
        FingerprintIndex index;
        for (std::size_t i = 0; i < nFiles; ++i) {
            auto& a = an[i];
            if (a.decodeFailed) continue;
            const auto it = seen.find(a.pcmSha256);
            if (it != seen.end()) {
                a.addReason("duplicate.exact");
                a.qualityClass = QualityClass::Rejected;
            } else {
                seen.emplace(a.pcmSha256, i);
            }
            if (a.qualityClass == QualityClass::Rejected || a.fingerprint.empty()) continue;
            std::uint32_t other = 0;
            const auto m = index.query(a.fingerprint, &other);
            if (m.found) {
                const auto shorter = static_cast<std::int64_t>(std::min(a.fingerprint.size(), an[other].fingerprint.size()));
                if (static_cast<double>(m.frames) >= 0.5 * static_cast<double>(shorter)) {
                    a.addReason("duplicate.near");
                    a.qualityClass = QualityClass::Rejected;
                } else {
                    a.addReason("duplicate.overlap");  // flag only; the passage stays usable, its anchors are excluded
                    // Fingerprint frame f spans 64/5512.5 s; map the aligned query range to 48 kHz samples.
                    const double sPerFrame = 64.0 / 5512.5 * static_cast<double>(kCacheRate);
                    const std::int64_t f0 = std::max<std::int64_t>(0, -m.offset);
                    a.overlapSpans.push_back({static_cast<std::int64_t>(static_cast<double>(f0) * sPerFrame),
                                              static_cast<std::int64_t>(static_cast<double>(f0 + m.frames) * sPerFrame)});
                }
            }
            if (a.qualityClass != QualityClass::Rejected) index.add(static_cast<std::uint32_t>(i), a.fingerprint);
        }
    }
    for (std::size_t i = 0; i < nFiles; ++i)
        if (an[i].qualityClass == QualityClass::Rejected) fs::remove(staging / cachePath(i), ec);

    // 6. speaker aggregation
    std::map<std::int64_t, SpeakerAgg> spk;  // by speaker id (ordered)
    for (std::size_t i = 0; i < nFiles; ++i) {
        auto& s = spk[speakerIdOf[assign[i].externalId]];
        s.externalId = assign[i].externalId;
        if (s.language.empty()) s.language = assign[i].language;
        if (s.labels.empty()) s.labels = assign[i].labelsJson;
        s.unknown = s.unknown || assign[i].unknown;
        if (an[i].qualityClass != QualityClass::Rejected) s.usable.push_back(i);
    }
    std::vector<std::int64_t> usableSpk;
    for (auto& [id, s] : spk) {
        double wsum = 0.0;
        std::vector<std::pair<double, double>> med;
        for (auto i : s.usable) {
            const auto& a = an[i];
            const double w = std::max(a.speechS, 1e-3);
            wsum += w;
            s.speechS += a.speechS;
            s.f0Mean += w * a.f0.meanHz; s.f0P5 += w * a.f0.p5Hz; s.f0P95 += w * a.f0.p95Hz;
            s.rate += w * a.speakingRate; s.centroid += w * a.spectralCentroidHz; s.asl += w * a.aslDb;
            s.quality += w * a.qualityScore;
            for (std::size_t b = 0; b < kNumLtassBands; ++b) s.power[b] += w * a.ltassPower[b];
            if (a.f0.medianHz > 0) med.emplace_back(a.f0.medianHz, w);
        }
        if (wsum > 0) {
            for (double* p : {&s.f0Mean, &s.f0P5, &s.f0P95, &s.rate, &s.centroid, &s.asl, &s.quality}) *p /= wsum;
            for (auto& p : s.power) p /= wsum;
            s.f0Med = weightedMedian(med);
            usableSpk.push_back(id);
        }
    }
    // PCA over usable speakers.
    std::vector<std::vector<double>> X;
    for (auto id : usableSpk) {
        std::vector<double> v(kNumOperatingBands);
        double tot = 0;
        for (double p : spk[id].power) tot += p;
        for (std::size_t d = 0; d < kNumOperatingBands; ++d)
            v[d] = 10.0 * std::log10(std::max(spk[id].power[kFirstOperatingBand + d] / std::max(tot, 1e-30), 1e-12));
        X.push_back(std::move(v));
    }
    std::vector<std::array<double, 3>> scores;
    const PcaBasis pca = computePca(X, scores);
    std::map<std::int64_t, std::array<double, 3>> scoreOf;
    for (std::size_t k = 0; k < usableSpk.size(); ++k) scoreOf[usableSpk[k]] = scores[k];

    // Possible same speaker under different IDs (feature rule only; flag, never merge).
    std::map<std::int64_t, std::uint32_t> spkFlags;
    for (auto& [id, s] : spk) if (s.unknown) spkFlags[id] |= kSpkUnknownIdentity;
    {
        std::array<double, 3> sd{};
        for (std::size_t k = 0; k < 3; ++k) sd[k] = std::sqrt(std::max(pca.eigenvalue[k], 1e-12));
        for (std::size_t x = 0; x < usableSpk.size(); ++x)
            for (std::size_t y = x + 1; y < usableSpk.size(); ++y) {
                const auto& a = spk[usableSpk[x]];
                const auto& b = spk[usableSpk[y]];
                if (a.f0Med <= 0 || b.f0Med <= 0) continue;
                if (std::fabs(12.0 * std::log2(a.f0Med / b.f0Med)) > 1.0) continue;
                const double ra = 12.0 * std::log2(std::max(a.f0P95, 1.0) / std::max(a.f0P5, 1.0));
                const double rb = 12.0 * std::log2(std::max(b.f0P95, 1.0) / std::max(b.f0P5, 1.0));
                if (std::fabs(ra - rb) > 2.0) continue;
                if (a.rate > 0 && b.rate > 0 && std::fabs(a.rate - b.rate) > 0.15 * std::max(a.rate, b.rate)) continue;
                double d2 = 0.0;
                for (std::size_t k = 0; k < 3; ++k) {
                    const double t = (scores[x][k] - scores[y][k]) / sd[k];
                    d2 += t * t;
                }
                if (std::sqrt(d2) >= 0.5) continue;
                spkFlags[usableSpk[x]] |= kSpkPossibleDuplicate;
                spkFlags[usableSpk[y]] |= kSpkPossibleDuplicate;
            }
    }

    // 7. corpus version
    std::vector<std::pair<std::size_t, std::string>> vparts;
    std::vector<std::vector<SegmentDbRow>> segRows(nFiles);
    for (std::size_t i = 0; i < nFiles; ++i) {
        if (an[i].qualityClass == QualityClass::Rejected) continue;
        Sha256 sh;
        for (const auto& s : an[i].segments) {
            const std::int64_t v[3] = {s.anchor, s.maxEnd, s.soloRisk ? 1 : 0};
            unsigned char b[24];
            for (int k = 0; k < 3; ++k) for (int j = 0; j < 8; ++j) b[k * 8 + j] = static_cast<unsigned char>(static_cast<std::uint64_t>(v[k]) >> (8 * j));
            sh.update(b, sizeof b);
        }
        vparts.emplace_back(i + 1, std::to_string(i + 1) + "|" + an[i].pcmSha256 + "|" + sh.finishHex() + ";");
    }
    std::sort(vparts.begin(), vparts.end());
    Sha256 vh;
    vh.update(std::string_view(kAnalyzerVersion));
    for (const auto& p : vparts) vh.update(p.second);
    const std::string fullHash = vh.finishHex();
    res.corpusVersion = fullHash.substr(0, 16);

    // 8. write the staging DB
    const std::string created = utcNow();
    nlohmann::json pcaJson;
    pcaJson["bands"] = {{"first", kFirstOperatingBand}, {"count", kNumOperatingBands}};
    pcaJson["mean"] = pca.mean;
    pcaJson["components"] = pca.comp;
    pcaJson["eigenvalues"] = pca.eigenvalue;
    pcaJson["analyzerVersion"] = kAnalyzerVersion;
    {
        auto db = CorpusDb::create((staging / "corpus.sqlite").string(), &err);
        if (!db) return fail(err);
        db->begin();
        db->setInfo("corpusVersion", res.corpusVersion);
        db->setInfo("analyzerVersion", kAnalyzerVersion);
        db->setInfo("created", created);
        db->setInfo("pcaBasis", pcaJson.dump());
        for (auto& [id, s] : spk) {
            SpeakerRow r;
            r.speakerId = id;
            r.externalId = s.externalId;
            r.language = s.language;
            r.nRecordings = static_cast<int>(s.usable.size());
            r.usableSpeechS = s.speechS;
            r.f0MedianHz = s.f0Med; r.f0MeanHz = s.f0Mean; r.f0P5Hz = s.f0P5; r.f0P95Hz = s.f0P95;
            r.f0RangeSt = (s.f0P5 > 0 && s.f0P95 > 0) ? 12.0 * std::log2(s.f0P95 / s.f0P5) : 0.0;
            r.speakingRateSps = s.rate;
            r.spectralCentroidHz = s.centroid;
            double tot = 0;
            for (double p : s.power) tot += p;
            for (double p : s.power) r.ltassThirdOctDb.push_back(static_cast<float>(10.0 * std::log10(std::max(p / std::max(tot, 1e-30), 1e-12))));
            if (auto it = scoreOf.find(id); it != scoreOf.end()) { r.pc1 = it->second[0]; r.pc2 = it->second[1]; r.pc3 = it->second[2]; }
            r.aslMeanDbfs = s.asl;
            r.qualityMean = s.quality;
            r.flags = spkFlags[id];
            r.enabled = !s.usable.empty();
            r.userLabels = s.labels;
            db->insertSpeaker(r);
        }
        std::int64_t segId = 0;
        for (std::size_t i = 0; i < nFiles; ++i) {
            const auto& a = an[i];
            RecordingRow r;
            r.recordingId = static_cast<std::int64_t>(i + 1);
            r.speakerId = speakerIdOf[assign[i].externalId];
            if (opt.storeSourcePaths) r.sourcePath = files[i].first;
            r.sourceSha256 = a.sourceSha256;
            r.pcmSha256 = a.pcmSha256;
            const bool usable = a.qualityClass != QualityClass::Rejected;
            if (usable) r.cacheFile = cachePath(i).generic_string();
            r.sourceSampleRate = static_cast<int>(a.sourceRate);
            r.sourceChannels = a.sourceChannels;
            r.sourceFormat = a.sourceFormat;
            r.lossy = a.lossy;
            r.durationS = a.durationS; r.speechS = a.speechS; r.speechRatio = a.speechRatio;
            r.peakDbfs = a.peakDb; r.truePeakDbtp = a.truePeakDb; r.rmsDbfs = a.rmsDb; r.aslDbfs = a.aslDb; r.activityPct = a.activityPct;
            r.lufsI = a.lufsI; r.noiseFloorDbfs = a.noiseFloorDb; r.snrDb = a.snrDb; r.bandwidthHz = a.bandwidthHz;
            r.clipRatio = a.clipRatio; r.dcOffset = a.dcOffset; r.reverberant = a.reverberant;
            r.f0MedianHz = a.f0.medianHz; r.f0P5Hz = a.f0.p5Hz; r.f0P95Hz = a.f0.p95Hz;
            r.f0Hist.assign(a.f0.hist.begin(), a.f0.hist.end());
            r.speakingRateSps = a.speakingRate; r.spectralCentroidHz = a.spectralCentroidHz;
            r.ltassThirdOctDb.assign(a.ltassDb.begin(), a.ltassDb.end());
            r.pauseHist.assign(a.pauseHist.begin(), a.pauseHist.end());
            r.pauseFracGt100 = a.pauseFracGt100; r.pauseFracGt250 = a.pauseFracGt250; r.pauseFracGt600 = a.pauseFracGt600;
            r.qualityScore = a.qualityScore;
            r.qualityClass = static_cast<int>(a.qualityClass);
            r.reasonCodes = joinReasons(a.reasons);
            r.fingerprint = a.fingerprint;
            r.vadEngine = a.vadEngine;
            r.analyzerVersion = kAnalyzerVersion;
            r.analyzedUtc = created;
            if (!db->insertRecording(r)) return fail("db: " + db->lastError());
            if (!usable) continue;
            for (const auto& reg : a.regions) db->insertRegion({r.recordingId, reg.start, reg.end});
            for (const auto& sg : a.segments) {
                SegmentDbRow g;
                g.segmentId = ++segId;
                g.recordingId = r.recordingId;
                g.anchorSample = sg.anchor;
                g.maxEndSample = sg.maxEnd;
                g.speechFrac10s = sg.speechFrac;
                g.longestPhraseS = sg.longestPhraseS;
                g.asl10sDbfs = sg.asl10sDb;
                g.soloRisk = sg.soloRisk;
                for (const auto& o : a.overlapSpans) if (sg.anchor >= o.start && sg.anchor < o.end) g.excluded = true;
                db->insertSegment(g);
            }
        }
        if (!db->commit()) return fail("db commit: " + db->lastError());
    }

    // 9. report
    for (std::size_t i = 0; i < nFiles; ++i) {
        const auto& a = an[i];
        ImportFileReport fr;
        fr.relPath = files[i].first;
        fr.speaker = assign[i].externalId;
        fr.cls = a.qualityClass;
        fr.reasons = a.reasons;
        fr.durationS = a.durationS; fr.speechS = a.speechS; fr.snrDb = a.snrDb; fr.aslDb = a.aslDb;
        fr.bandwidthHz = a.bandwidthHz; fr.qualityScore = a.qualityScore;
        res.totalS += a.durationS;
        if (a.qualityClass == QualityClass::Good) ++res.nGood;
        else if (a.qualityClass == QualityClass::Usable) ++res.nUsable;
        else ++res.nRejected;
        if (a.qualityClass != QualityClass::Rejected) { res.usableS += a.durationS; res.usableSpeechS += a.speechS; }
        res.files.push_back(std::move(fr));
    }
    res.nSpeakers = spk.size();
    res.nUsableSpeakers = usableSpk.size();

    // 10. manifest + source links, then atomic swap
    nlohmann::json man;
    man["format"] = 1;
    man["corpusVersion"] = res.corpusVersion;
    man["corpusHash"] = fullHash;
    man["analyzerVersion"] = kAnalyzerVersion;
    man["created"] = created;
    man["counts"] = {{"files", nFiles}, {"good", res.nGood}, {"usable", res.nUsable}, {"rejected", res.nRejected},
                     {"speakers", res.nSpeakers}, {"usableSpeakers", res.nUsableSpeakers}};
    man["durationS"] = {{"total", res.totalS}, {"usable", res.usableS}, {"usableSpeech", res.usableSpeechS}};
    man["pcaBasis"] = pcaJson;
    man["licenses"] = nlohmann::json::array();
    nlohmann::json links = nlohmann::json::object();
    if (opt.storeSourcePaths)
        for (std::size_t i = 0; i < nFiles; ++i) links[std::to_string(i + 1)] = files[i].second.generic_string();

    fs::create_directories(opt.corpusRoot, ec);
    // The previous cache is kept as cache.old-<version> (readers may still hold files from it);
    // older generations are purged at the start of the next import or via purgeOldCaches().
    CorpusDb::purgeOldCaches(opt.corpusRoot.string());
    if (fs::exists(opt.corpusRoot / "cache", ec)) {
        std::string oldVersion = "unknown";
        {
            std::ifstream mf(opt.corpusRoot / "manifest.json");
            const auto doc = nlohmann::json::parse(mf, nullptr, false);
            if (doc.is_object() && doc.contains("corpusVersion") && doc["corpusVersion"].is_string()) {
                std::string v = doc["corpusVersion"].get<std::string>();
                v.erase(std::remove_if(v.begin(), v.end(), [](unsigned char ch) { return !std::isalnum(ch); }), v.end());
                if (!v.empty()) oldVersion = v;
            }
        }
        const fs::path oldCache = opt.corpusRoot / ("cache.old-" + oldVersion);
        fs::remove_all(oldCache, ec);
        fs::rename(opt.corpusRoot / "cache", oldCache, ec);
    }
    ec.clear();
    fs::rename(staging / "cache", opt.corpusRoot / "cache", ec);
    if (ec) return fail("cannot install cache: " + ec.message());
    fs::rename(staging / "corpus.sqlite", opt.corpusRoot / "corpus.sqlite", ec);
    if (ec) return fail("cannot install database: " + ec.message());
    if (opt.storeSourcePaths) writeFileAtomic(opt.corpusRoot / "source_links.json", links.dump(2), nullptr);
    else fs::remove(opt.corpusRoot / "source_links.json", ec);
    if (!writeFileAtomic(opt.corpusRoot / "manifest.json", man.dump(2), &err)) return fail(err);
    fs::remove_all(staging, ec);
    res.ok = true;
    return res;
}

}  // namespace bf
