#include "core/corpus/CorpusLoader.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "core/corpus/CorpusDb.h"

namespace bf {

bool loadCorpus(const std::filesystem::path& root, LoadedCorpus& out, std::string* error) {
    std::string err;
    auto db = CorpusDb::open((root / "corpus.sqlite").string(), true, &err);
    if (!db) {
        if (error) *error = err;
        return false;
    }
    const auto speakers = db->speakers();
    const auto recs = db->recordings();
    const auto regions = db->regions();
    const auto segs = db->segments();

    std::set<std::string> langSet;
    for (const auto& s : speakers)
        if (s.enabled && !s.language.empty()) langSet.insert(s.language);
    out.languages.assign(langSet.begin(), langSet.end());

    std::map<std::int64_t, SpeakerId> spkIdx;
    std::vector<SpeakerInput> sin;
    out.speakerDbId.clear();
    for (const auto& s : speakers) {
        if (!s.enabled) continue;
        SpeakerInput si;
        auto& f = si.features;
        f[kFeatF0MedianSt] = s.f0MedianHz > 0 ? static_cast<float>(12.0 * std::log2(s.f0MedianHz / 100.0)) : 0.0f;
        f[kFeatF0RangeSt] = static_cast<float>(s.f0RangeSt);
        f[kFeatSpeakingRate] = static_cast<float>(s.speakingRateSps);
        f[kFeatCentroidLog2] = s.spectralCentroidHz > 0 ? static_cast<float>(std::log2(s.spectralCentroidHz)) : 0.0f;
        f[kFeatLtassPc1] = static_cast<float>(s.pc1);
        f[kFeatLtassPc2] = static_cast<float>(s.pc2);
        f[kFeatLtassPc3] = static_cast<float>(s.pc3);
        float lang = 0.0f;
        if (!s.language.empty())
            lang = static_cast<float>(std::find(out.languages.begin(), out.languages.end(), s.language) - out.languages.begin() + 1);
        f[kFeatLanguage] = lang;
        si.weight = std::max(0.01f, static_cast<float>(s.qualityMean / 100.0));
        spkIdx[s.speakerId] = static_cast<SpeakerId>(sin.size());
        out.speakerDbId.push_back(s.speakerId);
        sin.push_back(si);
    }

    std::map<std::int64_t, std::vector<SampleSpan>> regOf;
    for (const auto& r : regions) regOf[r.recordingId].push_back({r.startSample, r.endSample});
    std::map<std::int64_t, RecordingInput*> inOf;
    std::vector<RecordingInput> rin;
    std::vector<std::int64_t> dbIds;
    std::vector<std::filesystem::path> cachePaths;
    rin.reserve(recs.size());
    for (const auto& r : recs) {
        if (r.qualityClass <= 0 || r.cacheFile.empty()) continue;
        const auto sp = spkIdx.find(r.speakerId);
        if (sp == spkIdx.end()) continue;
        RecordingInput ri;
        ri.speaker = sp->second;
        ri.length = static_cast<std::int64_t>(std::llround(r.durationS * static_cast<double>(kCorpusRate)));
        if (const auto it = regOf.find(r.recordingId); it != regOf.end()) ri.speech = it->second;
        ri.aslDb = static_cast<float>(r.aslDbfs);
        ri.quality = static_cast<float>(std::clamp(r.qualityScore / 100.0, 0.0, 1.0));
        rin.push_back(std::move(ri));
        dbIds.push_back(r.recordingId);
        cachePaths.push_back(root / r.cacheFile);
    }
    for (std::size_t i = 0; i < rin.size(); ++i) inOf[dbIds[i]] = &rin[i];
    for (const auto& g : segs) {
        const auto it = inOf.find(g.recordingId);
        if (it == inOf.end()) continue;
        if (g.excluded) it->second->excludedAnchors.push_back(g.anchorSample);
        else it->second->anchorAsl.emplace_back(g.anchorSample, static_cast<float>(g.asl10sDbfs));
    }
    for (auto& ri : rin) {
        std::sort(ri.excludedAnchors.begin(), ri.excludedAnchors.end());
        std::sort(ri.anchorAsl.begin(), ri.anchorAsl.end());
    }

    std::string version;
    db->getInfo("corpusVersion", version);
    out.snapshot = CorpusSnapshot::build(version, std::move(sin), std::move(rin));
    const std::size_t n = out.snapshot->numRecordings();
    std::vector<std::filesystem::path> paths(n);
    out.recordingDbId.assign(n, 0);
    for (std::size_t r = 0; r < n; ++r) {
        const auto idx = out.snapshot->recording(static_cast<RecordingId>(r)).cacheFileIdx;
        paths[r] = cachePaths[idx];
        out.recordingDbId[r] = dbIds[idx];
    }
    out.audio = std::make_shared<FlacCacheAudioSource>(std::move(paths));
    return true;
}

}  // namespace bf
