#include "core/corpus/CorpusImporter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <array>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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

// One usable recording's contribution to its speaker's aggregate.
struct Contrib {
    double w = 0, speechS = 0, f0Mean = 0, f0P5 = 0, f0P95 = 0, f0Med = 0, rate = 0, centroid = 0, asl = 0, quality = 0;
    std::array<double, kNumLtassBands> power{};
};

Contrib contribOf(const RecordingAnalysis& a) {
    Contrib c;
    c.w = std::max(a.speechS, 1e-3);
    c.speechS = a.speechS;
    c.f0Mean = a.f0.meanHz; c.f0P5 = a.f0.p5Hz; c.f0P95 = a.f0.p95Hz; c.f0Med = a.f0.medianHz;
    c.rate = a.speakingRate; c.centroid = a.spectralCentroidHz; c.asl = a.aslDb; c.quality = a.qualityScore;
    c.power = a.ltassPower;
    return c;
}

// From a stored row: the per-recording F0 mean is not stored, the median stands in for it (the mean
// is informational only; the selector features use the median).
Contrib contribOf(const RecordingRow& r) {
    Contrib c;
    c.w = std::max(r.speechS, 1e-3);
    c.speechS = r.speechS;
    c.f0Mean = r.f0MedianHz; c.f0P5 = r.f0P5Hz; c.f0P95 = r.f0P95Hz; c.f0Med = r.f0MedianHz;
    c.rate = r.speakingRateSps; c.centroid = r.spectralCentroidHz; c.asl = r.aslDbfs; c.quality = r.qualityScore;
    for (std::size_t b = 0; b < kNumLtassBands; ++b)
        c.power[b] = b < r.ltassThirdOctDb.size() ? std::pow(10.0, static_cast<double>(r.ltassThirdOctDb[b]) / 10.0) : 0.0;
    return c;
}

struct SpeakerAgg {
    std::string externalId, language, labels;
    bool unknown = false;
    std::vector<Contrib> contribs;  // usable recordings (empty for a stored, unchanged speaker)
    bool stored = false;            // unchanged existing speaker: `row` is authoritative
    SpeakerRow row;
    bool usable = false;            // enters the PCA / is enabled
    double speechS = 0;
    double f0Med = 0, f0Mean = 0, f0P5 = 0, f0P95 = 0, rate = 0, centroid = 0, asl = 0, quality = 0;
    std::array<double, kNumLtassBands> power{};
    std::int64_t flagsBase = 0;
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

void aggregate(SpeakerAgg& s) {
    double wsum = 0.0;
    std::vector<std::pair<double, double>> med;
    s.speechS = 0;
    for (const auto& c : s.contribs) {
        wsum += c.w;
        s.speechS += c.speechS;
        s.f0Mean += c.w * c.f0Mean; s.f0P5 += c.w * c.f0P5; s.f0P95 += c.w * c.f0P95;
        s.rate += c.w * c.rate; s.centroid += c.w * c.centroid; s.asl += c.w * c.asl;
        s.quality += c.w * c.quality;
        for (std::size_t b = 0; b < kNumLtassBands; ++b) s.power[b] += c.w * c.power[b];
        if (c.f0Med > 0) med.emplace_back(c.f0Med, c.w);
    }
    s.usable = false;
    if (wsum > 0) {
        for (double* p : {&s.f0Mean, &s.f0P5, &s.f0P95, &s.rate, &s.centroid, &s.asl, &s.quality}) *p /= wsum;
        for (auto& p : s.power) p /= wsum;
        s.f0Med = weightedMedian(med);
        s.usable = true;
    }
}

// Existing speaker kept as stored.
SpeakerAgg storedAgg(const SpeakerRow& r) {
    SpeakerAgg s;
    s.stored = true;
    s.row = r;
    s.externalId = r.externalId;
    s.language = r.language;
    s.labels = r.userLabels;
    s.unknown = (r.flags & kSpkUnknownIdentity) != 0;
    s.flagsBase = r.flags;
    s.usable = r.enabled;
    s.speechS = r.usableSpeechS;
    s.f0Med = r.f0MedianHz; s.f0Mean = r.f0MeanHz; s.f0P5 = r.f0P5Hz; s.f0P95 = r.f0P95Hz;
    s.rate = r.speakingRateSps; s.centroid = r.spectralCentroidHz; s.asl = r.aslMeanDbfs; s.quality = r.qualityMean;
    double tot = 0;
    for (std::size_t b = 0; b < kNumLtassBands; ++b) {
        s.power[b] = b < r.ltassThirdOctDb.size() ? std::pow(10.0, static_cast<double>(r.ltassThirdOctDb[b]) / 10.0) : 0.0;
        tot += s.power[b];
    }
    if (tot <= 0) s.usable = false;
    return s;
}

struct PcaResult {
    PcaBasis basis;
    std::vector<std::int64_t> usableSpk;
    std::map<std::int64_t, std::array<double, 3>> scoreOf;
    std::map<std::int64_t, std::uint32_t> flags;
};

// PCA over the usable speakers (ordered by speaker id) and the speaker flags.
PcaResult runPca(std::map<std::int64_t, SpeakerAgg>& spk) {
    PcaResult out;
    for (auto& [id, s] : spk) if (s.usable) out.usableSpk.push_back(id);
    std::vector<std::vector<double>> X;
    for (auto id : out.usableSpk) {
        std::vector<double> v(kNumOperatingBands);
        double tot = 0;
        for (double p : spk[id].power) tot += p;
        for (std::size_t d = 0; d < kNumOperatingBands; ++d)
            v[d] = 10.0 * std::log10(std::max(spk[id].power[kFirstOperatingBand + d] / std::max(tot, 1e-30), 1e-12));
        X.push_back(std::move(v));
    }
    std::vector<std::array<double, 3>> scores;
    out.basis = computePca(X, scores);
    for (std::size_t k = 0; k < out.usableSpk.size(); ++k) out.scoreOf[out.usableSpk[k]] = scores[k];

    // Possible same speaker under different IDs (feature rule only; flag, never merge).
    for (auto& [id, s] : spk) if (s.unknown) out.flags[id] |= kSpkUnknownIdentity;
    std::array<double, 3> sd{};
    for (std::size_t k = 0; k < 3; ++k) sd[k] = std::sqrt(std::max(out.basis.eigenvalue[k], 1e-12));
    for (std::size_t x = 0; x < out.usableSpk.size(); ++x)
        for (std::size_t y = x + 1; y < out.usableSpk.size(); ++y) {
            const auto& a = spk[out.usableSpk[x]];
            const auto& b = spk[out.usableSpk[y]];
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
            out.flags[out.usableSpk[x]] |= kSpkPossibleDuplicate;
            out.flags[out.usableSpk[y]] |= kSpkPossibleDuplicate;
        }
    return out;
}

nlohmann::json pcaToJson(const PcaBasis& pca) {
    nlohmann::json j;
    j["bands"] = {{"first", kFirstOperatingBand}, {"count", kNumOperatingBands}};
    j["mean"] = pca.mean;
    j["components"] = pca.comp;
    j["eigenvalues"] = pca.eigenvalue;
    j["analyzerVersion"] = kAnalyzerVersion;
    return j;
}

SpeakerRow speakerRowOf(std::int64_t id, const SpeakerAgg& s, const PcaResult& pca) {
    SpeakerRow r;
    if (s.stored) r = s.row;
    else {
        r.speakerId = id;
        r.externalId = s.externalId;
        r.language = s.language;
        r.nRecordings = static_cast<int>(s.contribs.size());
        r.usableSpeechS = s.speechS;
        r.f0MedianHz = s.f0Med; r.f0MeanHz = s.f0Mean; r.f0P5Hz = s.f0P5; r.f0P95Hz = s.f0P95;
        r.f0RangeSt = (s.f0P5 > 0 && s.f0P95 > 0) ? 12.0 * std::log2(s.f0P95 / s.f0P5) : 0.0;
        r.speakingRateSps = s.rate;
        r.spectralCentroidHz = s.centroid;
        double tot = 0;
        for (double p : s.power) tot += p;
        for (double p : s.power) r.ltassThirdOctDb.push_back(static_cast<float>(10.0 * std::log10(std::max(p / std::max(tot, 1e-30), 1e-12))));
        r.aslMeanDbfs = s.asl;
        r.qualityMean = s.quality;
        r.enabled = !s.contribs.empty();
        r.userLabels = s.labels;
    }
    r.pc1 = r.pc2 = r.pc3 = 0.0;
    if (auto it = pca.scoreOf.find(id); it != pca.scoreOf.end()) { r.pc1 = it->second[0]; r.pc2 = it->second[1]; r.pc3 = it->second[2]; }
    std::uint32_t f = 0;
    if (auto it = pca.flags.find(id); it != pca.flags.end()) f = it->second;
    if (s.stored) f |= static_cast<std::uint32_t>(s.flagsBase & ~static_cast<std::int64_t>(kSpkPossibleDuplicate));
    r.flags = f;
    return r;
}

std::string segmentsHash(const std::vector<std::array<std::int64_t, 3>>& segs) {
    Sha256 sh;
    for (const auto& v : segs) {
        unsigned char b[24];
        for (int k = 0; k < 3; ++k) for (int j = 0; j < 8; ++j) b[k * 8 + j] = static_cast<unsigned char>(static_cast<std::uint64_t>(v[static_cast<std::size_t>(k)]) >> (8 * j));
        sh.update(b, sizeof b);
    }
    return sh.finishHex();
}

// corpusVersion over (recording id, pcm hash, segment table hash) of the usable recordings.
std::string corpusHashOf(std::vector<std::pair<std::int64_t, std::string>> parts) {
    std::sort(parts.begin(), parts.end());
    Sha256 vh;
    vh.update(std::string_view(kAnalyzerVersion));
    for (const auto& p : parts) vh.update(p.second);
    return vh.finishHex();
}
std::string versionPart(std::int64_t recId, const std::string& pcm, const std::string& segHash) {
    return std::to_string(recId) + "|" + pcm + "|" + segHash + ";";
}

struct Tally {
    std::size_t files = 0, good = 0, usable = 0, rejected = 0, speakers = 0, usableSpeakers = 0;
    double totalS = 0, usableS = 0, usableSpeechS = 0;
};
Tally tally(const std::vector<RecordingRow>& recs, const std::vector<SpeakerRow>& spk) {
    Tally t;
    for (const auto& r : recs) {
        ++t.files;
        t.totalS += r.durationS;
        if (r.qualityClass == 2) ++t.good;
        else if (r.qualityClass == 1) ++t.usable;
        else ++t.rejected;
        if (r.qualityClass > 0) { t.usableS += r.durationS; t.usableSpeechS += r.speechS; }
    }
    t.speakers = spk.size();
    for (const auto& s : spk) if (s.enabled) ++t.usableSpeakers;
    return t;
}

nlohmann::json manifestOf(const std::string& version, const std::string& fullHash, const std::string& created, const Tally& t,
                          const nlohmann::json& pcaJson) {
    nlohmann::json man;
    man["format"] = 1;
    man["corpusVersion"] = version;
    man["corpusHash"] = fullHash;
    man["analyzerVersion"] = kAnalyzerVersion;
    man["created"] = created;
    man["counts"] = {{"files", t.files}, {"good", t.good}, {"usable", t.usable}, {"rejected", t.rejected},
                     {"speakers", t.speakers}, {"usableSpeakers", t.usableSpeakers}};
    man["durationS"] = {{"total", t.totalS}, {"usable", t.usableS}, {"usableSpeech", t.usableSpeechS}};
    man["pcaBasis"] = pcaJson;
    man["licenses"] = nlohmann::json::array();
    return man;
}

// Everything about an existing corpus that an incremental import / removal needs.
struct Existing {
    bool have = false;
    std::string version;
    std::vector<SpeakerRow> speakers;
    std::vector<RecordingRow> recs;
    std::map<std::int64_t, std::vector<SpeechRegionRow>> regions;
    std::map<std::int64_t, std::vector<SegmentDbRow>> segs;
    std::int64_t maxSpeaker = 0, maxRecording = 0, maxSegment = 0;
    std::map<std::int64_t, bool> speakerEnabled;
};

bool loadExisting(const fs::path& root, Existing& e, std::string* err) {
    auto db = CorpusDb::open((root / "corpus.sqlite").string(), true, err);
    if (!db) return false;
    e.have = true;
    db->getInfo("corpusVersion", e.version);
    e.speakers = db->speakers();
    e.recs = db->recordings();
    for (const auto& r : db->regions()) e.regions[r.recordingId].push_back(r);
    for (const auto& g : db->segments()) { e.segs[g.recordingId].push_back(g); e.maxSegment = std::max(e.maxSegment, g.segmentId); }
    for (const auto& s : e.speakers) { e.maxSpeaker = std::max(e.maxSpeaker, s.speakerId); e.speakerEnabled[s.speakerId] = s.enabled; }
    for (const auto& r : e.recs) e.maxRecording = std::max(e.maxRecording, r.recordingId);
    return true;
}

bool activeRecording(const Existing& e, const RecordingRow& r) {
    if (r.qualityClass <= 0 || r.cacheFile.empty()) return false;
    const auto it = e.speakerEnabled.find(r.speakerId);
    return it != e.speakerEnabled.end() && it->second;
}

std::string sanitizeVersion(std::string v) {
    v.erase(std::remove_if(v.begin(), v.end(), [](unsigned char ch) { return !std::isalnum(ch); }), v.end());
    return v.empty() ? std::string("unknown") : v;
}

void linkOrCopy(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    ec.clear();
    fs::create_hard_link(from, to, ec);
    if (ec) {
        ec.clear();
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    }
}

bool writeDbFile(const fs::path& path, const std::string& version, const std::string& created, const nlohmann::json& pcaJson,
                 const std::vector<SpeakerRow>& speakers, const std::vector<RecordingRow>& recs,
                 const std::map<std::int64_t, std::vector<SpeechRegionRow>>& regions,
                 const std::map<std::int64_t, std::vector<SegmentDbRow>>& segs, std::string* err) {
    auto db = CorpusDb::create(path.string(), err);
    if (!db) return false;
    db->begin();
    db->setInfo("corpusVersion", version);
    db->setInfo("analyzerVersion", kAnalyzerVersion);
    db->setInfo("created", created);
    db->setInfo("pcaBasis", pcaJson.dump());
    for (const auto& s : speakers) db->insertSpeaker(s);
    for (const auto& r : recs) {
        if (!db->insertRecording(r)) { if (err) *err = "db: " + db->lastError(); return false; }
        if (r.qualityClass <= 0) continue;
        if (const auto it = regions.find(r.recordingId); it != regions.end())
            for (const auto& g : it->second) db->insertRegion(g);
        if (const auto it = segs.find(r.recordingId); it != segs.end())
            for (const auto& g : it->second) db->insertSegment(g);
    }
    if (!db->commit()) { if (err) *err = "db commit: " + db->lastError(); return false; }
    return true;
}

}  // namespace

ImportResult importCorpus(const ImportOptions& opt) {
    ImportResult res;
    auto fail = [&](const std::string& m) { res.ok = false; res.error = m; return res; };
    std::error_code ec;
    if (!fs::is_directory(opt.inputDir, ec)) return fail("input directory not found: " + opt.inputDir.string());

    // 0. incremental mode: the existing corpus
    Existing old;
    std::string err;
    const fs::path baseRoot = opt.baseRoot.empty() ? opt.corpusRoot : opt.baseRoot;
    if (opt.addToExisting && fs::exists(baseRoot / "corpus.sqlite", ec)) {
        if (!loadExisting(baseRoot, old, &err)) return fail("cannot read the existing library: " + err);
        res.added = true;
        res.previousVersion = old.version;
    }
    const std::int64_t baseRecId = old.maxRecording;

    // 1. scan (sorted, deterministic ids)
    std::vector<std::pair<std::string, fs::path>> files;  // rel path ('/'), abs path
    for (fs::recursive_directory_iterator it(opt.inputDir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || !supportedExt(it->path())) continue;
        files.emplace_back(fs::relative(it->path(), opt.inputDir, ec).generic_string(), it->path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) return fail("no audio files found in " + opt.inputDir.string());

    // 2. speaker identities -> dense ids in sorted external-id order (incremental: existing
    //    speakers keep their ids, new ones continue after the highest id)
    SpeakerResolver resolver;
    if (!resolver.init(opt.speakersCsv, opt.speakerRegex, &err)) return fail(err);
    std::vector<SpeakerAssignment> assign;
    std::set<std::string> ids;
    for (const auto& f : files) { assign.push_back(resolver.resolve(f.first)); ids.insert(assign.back().externalId); }
    std::map<std::int64_t, SpeakerAgg> spk;  // by speaker id (ordered)
    std::map<std::string, std::int64_t> speakerIdOf;
    if (res.added)
        for (const auto& s : old.speakers) { speakerIdOf[s.externalId] = s.speakerId; spk[s.speakerId] = storedAgg(s); }
    {
        std::int64_t next = old.maxSpeaker;
        for (const auto& id : ids)
            if (!speakerIdOf.count(id)) { speakerIdOf[id] = ++next; ++res.nNewSpeakers; }
    }
    const auto recIdOf = [&](std::size_t i) { return baseRecId + static_cast<std::int64_t>(i + 1); };

    // 3. staging area (incremental: the still-active old cache files are carried over first)
    const fs::path staging = opt.corpusRoot / ".staging";
    fs::remove_all(staging, ec);
    fs::create_directories(staging / "cache", ec);
    if (ec) return fail("cannot create staging directory: " + staging.string());
    if (res.added)
        for (const auto& r : old.recs)
            if (activeRecording(old, r)) {
                const fs::path src = baseRoot / fs::path(std::u8string(r.cacheFile.begin(), r.cacheFile.end()));
                if (fs::exists(src, ec)) linkOrCopy(src, staging / fs::path(std::u8string(r.cacheFile.begin(), r.cacheFile.end())));
            }

    // 4. analyse in parallel
    const std::size_t nFiles = files.size();
    std::vector<RecordingAnalysis> an(nFiles);
    std::atomic<std::size_t> next{0}, done{0};
    std::mutex progMu;
    std::mutex errMu;
    std::string writeErr;
    const auto cachePath = [&](std::size_t i) {
        return fs::path("cache") / std::to_string(speakerIdOf[assign[i].externalId]) / (std::to_string(recIdOf(i)) + ".flac");
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

    // 5. duplicate detection (sequential, in id order; the existing active recordings come first)
    {
        std::unordered_set<std::string> seen;
        FingerprintIndex index;
        std::vector<const RecordingRow*> oldFp;  // index id = nFiles + position
        for (const auto& r : old.recs) {
            if (!activeRecording(old, r)) continue;
            seen.insert(r.pcmSha256);
            if (!r.fingerprint.empty()) {
                index.add(static_cast<std::uint32_t>(nFiles + oldFp.size()), r.fingerprint);
                oldFp.push_back(&r);
            }
        }
        const auto fpSize = [&](std::uint32_t id) -> std::size_t {
            return id < nFiles ? an[id].fingerprint.size() : oldFp[id - nFiles]->fingerprint.size();
        };
        for (std::size_t i = 0; i < nFiles; ++i) {
            auto& a = an[i];
            if (a.decodeFailed) continue;
            if (!seen.insert(a.pcmSha256).second) {
                a.addReason("duplicate.exact");
                a.qualityClass = QualityClass::Rejected;
            }
            if (a.qualityClass == QualityClass::Rejected || a.fingerprint.empty()) continue;
            std::uint32_t other = 0;
            const auto m = index.query(a.fingerprint, &other);
            if (m.found) {
                const auto shorter = static_cast<std::int64_t>(std::min(a.fingerprint.size(), fpSize(other)));
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
    std::map<std::int64_t, std::vector<std::size_t>> usableOf;  // speaker id -> new usable file indices
    for (std::size_t i = 0; i < nFiles; ++i) {
        const std::int64_t id = speakerIdOf[assign[i].externalId];
        auto& s = spk[id];
        if (s.externalId.empty()) s.externalId = assign[i].externalId;
        if (!s.stored) {
            if (s.language.empty()) s.language = assign[i].language;
            if (s.labels.empty()) s.labels = assign[i].labelsJson;
            s.unknown = s.unknown || assign[i].unknown;
        }
        if (an[i].qualityClass != QualityClass::Rejected) usableOf[id].push_back(i);
    }
    for (auto& [id, s] : spk) {
        if (s.stored && !usableOf.count(id)) continue;  // existing speaker without new usable recordings
        if (s.stored) {                                 // existing speaker with new usable recordings: re-aggregate
            SpeakerAgg r;
            r.externalId = s.externalId; r.language = s.language; r.labels = s.labels; r.unknown = s.unknown;
            r.flagsBase = s.flagsBase;
            for (const auto& rr : old.recs)
                if (rr.speakerId == id && activeRecording(old, rr)) r.contribs.push_back(contribOf(rr));
            if (r.language.empty())
                for (std::size_t i = 0; i < nFiles; ++i)
                    if (speakerIdOf[assign[i].externalId] == id && !assign[i].language.empty()) { r.language = assign[i].language; break; }
            s = std::move(r);
        }
        if (const auto it = usableOf.find(id); it != usableOf.end())
            for (auto i : it->second) s.contribs.push_back(contribOf(an[i]));
        aggregate(s);
    }
    const PcaResult pcaRes = runPca(spk);
    const PcaBasis& pca = pcaRes.basis;
    const auto& usableSpk = pcaRes.usableSpk;

    // 7. corpus version
    std::vector<std::pair<std::int64_t, std::string>> vparts;
    std::map<std::int64_t, std::vector<SegmentDbRow>> segOut;
    std::map<std::int64_t, std::vector<SpeechRegionRow>> regOut;
    for (const auto& r : old.recs) {
        if (!activeRecording(old, r)) continue;
        std::vector<std::array<std::int64_t, 3>> sg;
        if (const auto it = old.segs.find(r.recordingId); it != old.segs.end())
            for (const auto& g : it->second) sg.push_back({g.anchorSample, g.maxEndSample, g.soloRisk ? 1 : 0});
        vparts.emplace_back(r.recordingId, versionPart(r.recordingId, r.pcmSha256, segmentsHash(sg)));
    }
    for (std::size_t i = 0; i < nFiles; ++i) {
        if (an[i].qualityClass == QualityClass::Rejected) continue;
        std::vector<std::array<std::int64_t, 3>> sg;
        for (const auto& s : an[i].segments) sg.push_back({s.anchor, s.maxEnd, s.soloRisk ? 1 : 0});
        vparts.emplace_back(recIdOf(i), versionPart(recIdOf(i), an[i].pcmSha256, segmentsHash(sg)));
    }
    const std::string fullHash = corpusHashOf(std::move(vparts));
    res.corpusVersion = fullHash.substr(0, 16);

    // 8. the merged rows
    const std::string created = utcNow();
    const nlohmann::json pcaJson = pcaToJson(pca);
    std::vector<SpeakerRow> spkRows;
    for (auto& [id, s] : spk) spkRows.push_back(speakerRowOf(id, s, pcaRes));
    std::vector<RecordingRow> recRows;
    if (res.added) {
        recRows = old.recs;
        regOut = old.regions;
        segOut = old.segs;
    }
    std::int64_t segId = old.maxSegment;
    for (std::size_t i = 0; i < nFiles; ++i) {
        const auto& a = an[i];
        RecordingRow r;
        r.recordingId = recIdOf(i);
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
        recRows.push_back(r);
        if (!usable) continue;
        for (const auto& reg : a.regions) regOut[r.recordingId].push_back({r.recordingId, reg.start, reg.end});
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
            segOut[r.recordingId].push_back(g);
        }
    }
    if (!writeDbFile(staging / "corpus.sqlite", res.corpusVersion, created, pcaJson, spkRows, recRows, regOut, segOut, &err)) return fail(err);

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
    const Tally tot = tally(recRows, spkRows);
    res.nSpeakers = tot.speakers;
    res.nUsableSpeakers = tot.usableSpeakers;
    res.nTotalFiles = tot.files;
    (void)usableSpk;

    // 10. manifest + source links, then atomic swap
    const nlohmann::json man = manifestOf(res.corpusVersion, fullHash, created, tot, pcaJson);
    nlohmann::json links = nlohmann::json::object();
    if (opt.storeSourcePaths) {
        if (res.added) {
            std::ifstream lf(baseRoot / "source_links.json");
            const auto doc = nlohmann::json::parse(lf, nullptr, false);
            if (doc.is_object()) links = doc;
        }
        for (std::size_t i = 0; i < nFiles; ++i) links[std::to_string(recIdOf(i))] = files[i].second.generic_string();
    }

    fs::create_directories(opt.corpusRoot, ec);
    // The previous cache is kept as cache.old-<version> (readers may still hold files from it);
    // older generations that nothing reads any more are purged at the start of the next import or
    // via purgeOldCaches().
    CorpusDb::purgeOldCaches(opt.corpusRoot.string());
    if (fs::exists(opt.corpusRoot / "cache", ec)) {
        std::string oldVersion = "unknown";
        {
            std::ifstream mf(opt.corpusRoot / "manifest.json");
            const auto doc = nlohmann::json::parse(mf, nullptr, false);
            if (doc.is_object() && doc.contains("corpusVersion") && doc["corpusVersion"].is_string())
                oldVersion = sanitizeVersion(doc["corpusVersion"].get<std::string>());
        }
        fs::path oldCache = opt.corpusRoot / ("cache.old-" + oldVersion);
        // A generation of the same version that a live source still reads from is never replaced.
        for (int n = 2; FlacCacheAudioSource::isPinned(oldCache) && n < 1000; ++n)
            oldCache = opt.corpusRoot / ("cache.old-" + oldVersion + "-" + std::to_string(n));
        fs::remove_all(oldCache, ec);
        FlacCacheAudioSource::retireCacheDir(opt.corpusRoot / "cache", oldCache, ec);  // live readers follow the rename
        if (!ec) res.retiredCacheDir = oldCache;
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

RemoveResult removeFromCorpus(const RemoveOptions& opt) {
    RemoveResult res;
    auto fail = [&](const std::string& m) { res.ok = false; res.error = m; return res; };
    std::error_code ec;
    Existing old;
    std::string err;
    if (!loadExisting(opt.corpusRoot, old, &err)) return fail("cannot read the library: " + err);
    res.previousVersion = old.version;

    std::set<std::int64_t> dropSpk, dropRec(opt.recordings.begin(), opt.recordings.end());
    for (const auto& id : opt.speakers) {
        bool found = false;
        for (const auto& s : old.speakers)
            if (s.externalId == id) { dropSpk.insert(s.speakerId); found = true; }
        if (!found) {
            char* endp = nullptr;
            const long long n = std::strtoll(id.c_str(), &endp, 10);
            if (endp && *endp == '\0' && !id.empty() && old.speakerEnabled.count(n)) { dropSpk.insert(n); found = true; }
        }
        if (!found) return fail("no such speaker: " + id);
    }
    std::set<std::int64_t> knownRec;
    for (const auto& r : old.recs) knownRec.insert(r.recordingId);
    for (auto id : dropRec) if (!knownRec.count(id)) return fail("no such recording: " + std::to_string(id));
    if (dropSpk.empty() && dropRec.empty()) return fail("nothing to remove");

    std::set<std::int64_t> touched;  // speakers whose aggregate changes
    std::vector<RecordingRow> recs = old.recs;
    for (auto& r : recs) {
        const bool byRec = dropRec.count(r.recordingId) != 0, bySpk = dropSpk.count(r.speakerId) != 0;
        if (!byRec && !bySpk) continue;
        if (r.qualityClass > 0) {
            ++res.recordingsRemoved;
            touched.insert(r.speakerId);
            r.qualityClass = 0;
            r.cacheFile.clear();
            if (!r.reasonCodes.empty()) r.reasonCodes += ",";
            r.reasonCodes += "user.removed";
        }
    }
    std::map<std::int64_t, SpeakerAgg> spk;
    for (const auto& s : old.speakers) {
        SpeakerAgg a = storedAgg(s);
        if (dropSpk.count(s.speakerId)) {
            ++res.speakersRemoved;
            a.row.enabled = false;
            a.row.nRecordings = 0;
            a.row.usableSpeechS = 0;
            a.usable = false;
        } else if (touched.count(s.speakerId)) {
            SpeakerAgg r;
            r.externalId = a.externalId; r.language = a.language; r.labels = a.labels; r.unknown = a.unknown;
            r.flagsBase = a.flagsBase;
            for (const auto& rr : recs)
                if (rr.speakerId == s.speakerId && rr.qualityClass > 0 && !rr.cacheFile.empty()) r.contribs.push_back(contribOf(rr));
            aggregate(r);
            a = std::move(r);
        }
        spk[s.speakerId] = std::move(a);
    }
    const PcaResult pcaRes = runPca(spk);
    std::vector<SpeakerRow> spkRows;
    for (auto& [id, s] : spk) {
        SpeakerRow r = speakerRowOf(id, s, pcaRes);
        if (dropSpk.count(id)) { r.enabled = false; r.pc1 = r.pc2 = r.pc3 = 0; }
        else if (!s.stored && !s.usable) r.enabled = false;
        spkRows.push_back(std::move(r));
    }

    std::vector<std::pair<std::int64_t, std::string>> vparts;
    std::map<std::int64_t, bool> spkOn;
    for (const auto& r : spkRows) spkOn[r.speakerId] = r.enabled;
    for (const auto& r : recs) {
        if (r.qualityClass <= 0 || r.cacheFile.empty() || !spkOn[r.speakerId]) continue;
        std::vector<std::array<std::int64_t, 3>> sg;
        if (const auto it = old.segs.find(r.recordingId); it != old.segs.end())
            for (const auto& g : it->second) sg.push_back({g.anchorSample, g.maxEndSample, g.soloRisk ? 1 : 0});
        vparts.emplace_back(r.recordingId, versionPart(r.recordingId, r.pcmSha256, segmentsHash(sg)));
    }
    const std::string fullHash = corpusHashOf(std::move(vparts));
    res.corpusVersion = fullHash.substr(0, 16);

    const std::string created = utcNow();
    const nlohmann::json pcaJson = pcaToJson(pcaRes.basis);
    const fs::path tmp = opt.corpusRoot / ".staging-remove";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    if (!writeDbFile(tmp / "corpus.sqlite", res.corpusVersion, created, pcaJson, spkRows, recs, old.regions, old.segs, &err)) {
        fs::remove_all(tmp, ec);
        return fail(err);
    }
    const Tally t = tally(recs, spkRows);
    res.nSpeakers = t.speakers;
    res.nUsableSpeakers = t.usableSpeakers;
    nlohmann::json man = manifestOf(res.corpusVersion, fullHash, created, t, pcaJson);
    {
        std::ifstream mf(opt.corpusRoot / "manifest.json");
        const auto doc = nlohmann::json::parse(mf, nullptr, false);
        if (doc.is_object()) {
            if (doc.contains("created")) man["created"] = doc["created"];
            if (doc.contains("licenses")) man["licenses"] = doc["licenses"];
        }
    }
    fs::remove(opt.corpusRoot / "corpus.sqlite", ec);
    fs::rename(tmp / "corpus.sqlite", opt.corpusRoot / "corpus.sqlite", ec);
    if (ec) { fs::remove_all(tmp, ec); return fail("cannot install database: " + ec.message()); }
    fs::remove_all(tmp, ec);
    if (!writeFileAtomic(opt.corpusRoot / "manifest.json", man.dump(2), &err)) return fail(err);
    res.ok = true;
    return res;
}

}  // namespace bf
