// BabbleForge web demo: C API over the offline (synchronous) MaskEngine for the browser.
//
// The browser worker drives one engine: bf_start(preset) prepares it, bf_render(n) renders n
// frames (planar float, returned through a pointer into the WASM heap), bf_set_preset() rebuilds
// the plan through the same compose/validate/buildPlan path as the desktop app (Scenario
// buildScenarioPlan) and schedules it at the current position so the engine's own plan
// crossfades apply. Strings cross the boundary as UTF-8 JSON; returned strings live in a
// static buffer until the next call.
//
// The factory data set (resources/data) is embedded at /data.
#include <emscripten/emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Version.h"
#include "core/config/Compose.h"
#include "core/config/DataSet.h"
#include "core/config/Preset.h"
#include "core/corpus/CorpusSnapshot.h"
#include "core/corpus/SyntheticCorpus.h"
#include "core/engine/MaskEngine.h"
#include "core/engine/OfflineRenderer.h"
#include "core/engine/Scenario.h"
#include "core/analysis/SpectrumAnalyzer.h"
#include "core/spectrum/FirDesigner.h"
#include "core/spectrum/SpectrumTarget.h"
#include "core/strategy/PlanComposer.h"

using nlohmann::json;

namespace {

std::string g_ret;
const char* ret(const json& j) {
    g_ret = j.dump();
    return g_ret.c_str();
}
const char* retErr(const std::string& e) { return ret(json{{"ok", false}, {"error", e}}); }

std::unique_ptr<bf::DataSet> g_ds;

// In-memory 48 kHz mono audio source (index = input recording order = cacheFileIdx).
class MemoryAudioSource final : public bf::IAudioSource {
public:
    std::vector<std::vector<float>> audio;
    std::shared_ptr<const bf::CorpusSnapshot> snap;
    bool read(bf::RecordingId rec, std::uint64_t start, float* dst, std::size_t n) override {
        std::fill(dst, dst + n, 0.0f);
        if (!snap || rec >= snap->numRecordings()) return false;
        const auto idx = snap->recording(rec).cacheFileIdx;
        if (idx >= audio.size() || audio[idx].empty()) return false;
        const auto& a = audio[idx];
        if (start >= a.size()) return true;
        const std::size_t m = std::min<std::size_t>(n, a.size() - static_cast<std::size_t>(start));
        std::memcpy(dst, a.data() + start, m * sizeof(float));
        return true;
    }
};

struct Corpus {
    std::shared_ptr<const bf::CorpusSnapshot> snapshot;
    bf::IAudioSource* audio = nullptr;
    std::unique_ptr<bf::SyntheticCorpus> synth;
    std::unique_ptr<MemoryAudioSource> mem;
    json info;
};
Corpus g_corpus;
// Pending real corpus (between bf_corpus_begin and bf_corpus_finish).
std::vector<bf::SpeakerInput> g_pendSpk;
std::vector<bf::RecordingInput> g_pendRec;
std::unique_ptr<MemoryAudioSource> g_pendMem;
json g_pendInfo;
std::string g_pendVersion;

struct Live {
    std::unique_ptr<bf::MaskEngine> engine;
    bf::OutputLayout layout;
    json presetDoc;
    std::uint64_t seed = 1;
    bool correctionEnabled = true;
    std::string correctionSpeed = "normal";
    std::vector<std::vector<float>> buf;
    std::vector<float*> ptr;
    std::vector<float> planar;  // N x n, contiguous for JS
};
Live g_live;

bf::CorrectionSpeed speedFrom(const std::string& s) {
    if (s == "slow") return bf::CorrectionSpeed::Slow;
    if (s == "fast") return bf::CorrectionSpeed::Fast;
    return bf::CorrectionSpeed::Normal;
}

bf::CorpusSummary summary() {
    return g_corpus.snapshot ? bf::CorpusSummary::from(*g_corpus.snapshot) : bf::CorpusSummary{};
}

json readJsonFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    std::string s;
    char b[4096];
    std::size_t k;
    while ((k = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, k);
    std::fclose(f);
    try {
        return json::parse(s);
    } catch (...) {
        return nullptr;
    }
}

json layoutJson(const bf::OutputLayout& l) {
    json outs = json::array();
    for (const auto& o : l.outputs) outs.push_back({{"label", o.label}, {"azimuthDeg", o.azimuthDeg}});
    return {{"id", l.id}, {"outputs", outs}};
}

}  // namespace

extern "C" {

// Loads the embedded data set. Returns {"ok", "version", "dataSetHash"}.
EMSCRIPTEN_KEEPALIVE const char* bf_init() {
    const bf::DataSetResult r = bf::loadDataSet("/data");
    if (!r.ok) return retErr("data set: " + r.error);
    g_ds = std::make_unique<bf::DataSet>(r.data);
    return ret({{"ok", true}, {"version", bf::versionString()}, {"dataSetHash", g_ds->dataSetHash}});
}

// Raw factory data documents for the UI (areas, strategies, targets, engine defaults, macros).
EMSCRIPTEN_KEEPALIVE const char* bf_catalog() {
    if (!g_ds) return retErr("not initialised");
    json c;
    c["ok"] = true;
    c["engineDefaults"] = readJsonFile("/data/engine_defaults.json");
    for (const char* kind : {"areas", "strategies", "targets"}) {
        json arr = json::array();
        const auto add = [&](const std::string& id) {
            json d = readJsonFile(std::string("/data/") + kind + "/" + id + ".json");
            if (!d.is_null()) arr.push_back(d);
        };
        if (std::string(kind) == "areas")
            for (const auto& [id, a] : g_ds->areas) add(id);
        else if (std::string(kind) == "strategies")
            for (const auto& [id, s] : g_ds->strategies) add(id);
        else {
            for (const auto& entry : {"ltass_universal_byrne1994", "pink", "flat", "slope_-5", "slope_-7", "slope_-9"})
                add(entry);
        }
        c[kind] = arr;
    }
    json hz = json::array();
    for (std::size_t i = 0; i < bf::kFftViewPoints; ++i) hz.push_back(bf::fftViewHz(i));
    c["fftViewHz"] = hz;
    c["thirdOctHz"] = std::vector<double>(bf::thirdOctNominalHz().begin(), bf::thirdOctNominalHz().end());
    json ids = json::array();
    for (const auto& [id, t] : g_ds->targets) ids.push_back({{"id", id}, {"displayName", t.displayName}});
    c["targetIds"] = ids;
    c["ltassUniversal"] = nullptr;
    if (auto it = g_ds->targets.find("ltass_universal"); it != g_ds->targets.end())
        c["ltassUniversal"] = {{"bandCentersHz", it->second.bandCentersHz}, {"levelsDb", it->second.levelsDb}};
    return ret(c);
}

// compose + validate + buildPlan for display (AppState::recompute equivalent).
EMSCRIPTEN_KEEPALIVE const char* bf_effective(const char* presetJson) {
    if (!g_ds) return retErr("not initialised");
    try {
        const json doc = json::parse(presetJson);
        const bf::ScenarioPlan sp = bf::buildScenarioPlan(*g_ds, doc, std::nullopt, summary(), 1);
        if (!sp.ok) return retErr(sp.error);
        const bf::ComposeResult c = bf::compose(*g_ds, sp.preset);
        json j;
        j["ok"] = true;
        j["plan"] = json::parse(sp.plan.canonicalJson());
        j["conflicts"] = sp.conflicts;
        j["layout"] = layoutJson(sp.layout);
        if (c.ok) {
            const auto& e = c.config;
            j["effective"] = {{"strengthDb", e.strengthDb}, {"character", e.character}, {"voiceAmount", e.voiceAmount},
                              {"clearVoiceReduction", e.clearVoiceReduction}, {"voiceDiversity", e.voiceDiversity},
                              {"babbleFraction", e.babbleFraction}, {"stationaryEnabled", e.stationaryEnabled},
                              {"seedMode", e.seedMode}, {"spectrumTarget", e.spectrumTarget},
                              {"correctionEnabled", e.correctionEnabled}, {"correctionSpeed", e.correctionSpeed},
                              {"spatialAlgorithm", e.spatialAlgorithm}, {"spread", e.spread}, {"motion", e.motion},
                              {"speakerVariation", e.speakerVariation}, {"limiterEnabled", e.limiterEnabled},
                              {"limiterCeilingDbtp", e.limiterCeilingDbtp}};
        }
        const auto& t = sp.plan.talkers;
        j["talkers"] = {{"pool", t.pool}, {"average", t.mean}, {"minimum", t.minActive}, {"maximum", t.maxActive},
                        {"maxInternalGapMs", t.maxGapMs}, {"segmentMinS", t.segMinS}, {"segmentMaxS", t.segMaxS},
                        {"gainVariationDb", t.gainSigmaDb}, {"fadeMs", t.fadeInMs}};
        json adj = json::array();
        for (const auto& a : sp.plan.adjustments) adj.push_back(a.fieldPath + ": " + a.reason);
        j["adjustments"] = adj;
        return ret(j);
    } catch (const std::exception& e) {
        return retErr(e.what());
    }
}

// Synthetic pseudo-speech corpus (same parameters as `bfrender --synthetic-corpus n`).
EMSCRIPTEN_KEEPALIVE const char* bf_corpus_synthetic(int speakers) {
    bf::SyntheticCorpusParams p;
    p.numSpeakers = static_cast<std::uint32_t>(std::max(1, speakers));
    p.recordingsPerSpeaker = 2;
    p.recordingSeconds = 60.0;
    g_corpus = Corpus{};
    g_corpus.synth = std::make_unique<bf::SyntheticCorpus>(p);
    g_corpus.snapshot = g_corpus.synth->snapshot();
    g_corpus.audio = g_corpus.synth.get();
    g_corpus.info = {{"kind", "synthetic"}, {"speakers", speakers}, {"version", g_corpus.snapshot->corpusVersion()}};
    return ret({{"ok", true}, {"info", g_corpus.info}});
}

// Real corpus from corpus.json (tools/webdemo exporter): metadata first, then the audio of
// every recording (bf_corpus_set_audio), then bf_corpus_finish.
EMSCRIPTEN_KEEPALIVE const char* bf_corpus_begin(const char* corpusJson) {
    try {
        const json c = json::parse(corpusJson);
        g_pendSpk.clear();
        g_pendRec.clear();
        g_pendMem = std::make_unique<MemoryAudioSource>();
        g_pendVersion = c.value("corpusVersion", std::string("web"));
        for (const auto& s : c.at("speakers")) {
            bf::SpeakerInput si;
            const auto& f = s.at("features");
            for (std::size_t i = 0; i < bf::kNumSpeakerFeatures && i < f.size(); ++i) si.features[i] = f[i].get<float>();
            si.weight = s.value("weight", 1.0f);
            si.healthy = s.value("healthy", true);
            g_pendSpk.push_back(si);
        }
        for (const auto& r : c.at("recordings")) {
            bf::RecordingInput ri;
            ri.speaker = r.at("speaker").get<bf::SpeakerId>();
            ri.length = r.at("length").get<std::int64_t>();
            for (const auto& sp : r.at("speech")) ri.speech.push_back({sp[0].get<std::int64_t>(), sp[1].get<std::int64_t>()});
            ri.aslDb = r.value("aslDb", -26.0f);
            ri.quality = r.value("quality", 1.0f);
            if (r.contains("anchorAsl"))
                for (const auto& a : r["anchorAsl"]) ri.anchorAsl.emplace_back(a[0].get<std::int64_t>(), a[1].get<float>());
            if (r.contains("excludedAnchors"))
                for (const auto& a : r["excludedAnchors"]) ri.excludedAnchors.push_back(a.get<std::int64_t>());
            g_pendRec.push_back(std::move(ri));
        }
        g_pendMem->audio.assign(g_pendRec.size(), {});
        g_pendInfo = {{"kind", "speech"}, {"speakers", g_pendSpk.size()}, {"recordings", g_pendRec.size()},
                      {"version", g_pendVersion}};
        return ret({{"ok", true}, {"recordings", g_pendRec.size()}});
    } catch (const std::exception& e) {
        return retErr(std::string("corpus.json: ") + e.what());
    }
}

// Copies n float samples (48 kHz mono) for input recording `idx` from the WASM heap.
EMSCRIPTEN_KEEPALIVE int bf_corpus_set_audio(int idx, const float* data, int n) {
    if (!g_pendMem || idx < 0 || static_cast<std::size_t>(idx) >= g_pendMem->audio.size() || n < 0) return 0;
    auto& a = g_pendMem->audio[static_cast<std::size_t>(idx)];
    a.assign(data, data + n);
    const auto want = static_cast<std::size_t>(g_pendRec[static_cast<std::size_t>(idx)].length);
    a.resize(want, 0.0f);  // decoders may add / drop a few samples at the end
    return 1;
}

EMSCRIPTEN_KEEPALIVE const char* bf_corpus_finish() {
    if (!g_pendMem) return retErr("no pending corpus");
    g_corpus = Corpus{};
    g_corpus.mem = std::move(g_pendMem);
    g_corpus.snapshot = bf::CorpusSnapshot::build(g_pendVersion, std::move(g_pendSpk), std::move(g_pendRec));
    g_corpus.mem->snap = g_corpus.snapshot;
    g_corpus.audio = g_corpus.mem.get();
    g_corpus.info = g_pendInfo;
    g_corpus.info["segments"] = g_corpus.snapshot->numSegments();
    g_corpus.info["speechSeconds"] = g_corpus.snapshot->totalSpeechSeconds();
    return ret({{"ok", true}, {"info", g_corpus.info}});
}

EMSCRIPTEN_KEEPALIVE const char* bf_corpus_info() { return ret(g_corpus.info); }

// Creates and prepares the engine for a preset at 48 kHz and applies the first plan.
EMSCRIPTEN_KEEPALIVE const char* bf_start(const char* presetJson, double seed, int maxBlock) {
    if (!g_ds) return retErr("not initialised");
    try {
        g_live.engine.reset();
        const json doc = json::parse(presetJson);
        g_live.seed = static_cast<std::uint64_t>(seed);
        const bf::CorpusSummary cs = summary();
        const bf::ScenarioPlan sp = bf::buildScenarioPlan(*g_ds, doc, std::nullopt, cs, g_live.seed);
        if (!sp.ok) return retErr(sp.error);
        bf::MaskEngineConfig mc;
        mc.seed = g_live.seed;
        if (g_corpus.snapshot) {
            mc.corpus = g_corpus.snapshot;
            mc.audio = g_corpus.audio;
        }
        mc.lRefDbfs = sp.plan.level.lRefDbfs;
        mc.channels = sp.preset.outputs.channels;
        mc.zones = sp.preset.outputs.zones;
        g_live.correctionEnabled = sp.preset.spectrum.correction.enabled.value_or(true);
        g_live.correctionSpeed = sp.preset.spectrum.correction.speed.value_or("normal");
        mc.correctionEnabled = g_live.correctionEnabled;
        mc.correctionSpeed = speedFrom(g_live.correctionSpeed);
        auto eng = std::make_unique<bf::MaskEngine>(mc);
        std::string err;
        if (!eng->prepare(48000.0, sp.layout, std::max(128, maxBlock), &err)) return retErr(err);
        if (!eng->setPlan(sp.plan, 0, &err)) return retErr(err);
        g_live.engine = std::move(eng);
        g_live.layout = sp.layout;
        g_live.presetDoc = doc;
        const int N = g_live.engine->numChannels();
        g_live.buf.assign(static_cast<std::size_t>(N), std::vector<float>(static_cast<std::size_t>(std::max(128, maxBlock))));
        g_live.ptr.resize(static_cast<std::size_t>(N));
        for (int c = 0; c < N; ++c) g_live.ptr[static_cast<std::size_t>(c)] = g_live.buf[static_cast<std::size_t>(c)].data();
        return ret({{"ok", true}, {"channels", N}, {"layout", layoutJson(sp.layout)}, {"conflicts", sp.conflicts},
                    {"latency", g_live.engine->latencySamples()}, {"babble", sp.plan.babbleEnabled}});
    } catch (const std::exception& e) {
        return retErr(e.what());
    }
}

// Re-composes the plan and schedules it at the current position (the engine crossfades).
// {"ok":true,"rebuild":true} means the change needs a new engine (layout / correction config).
EMSCRIPTEN_KEEPALIVE const char* bf_set_preset(const char* presetJson) {
    if (!g_ds || !g_live.engine) return retErr("not running");
    try {
        const json doc = json::parse(presetJson);
        const bf::ScenarioPlan sp = bf::buildScenarioPlan(*g_ds, doc, std::nullopt, summary(), g_live.seed);
        if (!sp.ok) return retErr(sp.error);
        const bool corr = sp.preset.spectrum.correction.enabled.value_or(true);
        const std::string speed = sp.preset.spectrum.correction.speed.value_or("normal");
        if (sp.layout.size() != g_live.layout.size() || sp.layout.id != g_live.layout.id ||
            corr != g_live.correctionEnabled || speed != g_live.correctionSpeed)
            return ret({{"ok", true}, {"rebuild", true}});
        std::string err;
        if (!g_live.engine->setPlan(sp.plan, g_live.engine->position(), &err)) return retErr(err);
        g_live.presetDoc = doc;
        return ret({{"ok", true}, {"rebuild", false}, {"conflicts", sp.conflicts}});
    } catch (const std::exception& e) {
        return retErr(e.what());
    }
}

// Renders n frames; returns a pointer to planar float data (channel c at ptr + c * n).
EMSCRIPTEN_KEEPALIVE float* bf_render(int n) {
    if (!g_live.engine || n <= 0) return nullptr;
    const int N = g_live.engine->numChannels();
    g_live.planar.resize(static_cast<std::size_t>(N) * static_cast<std::size_t>(n));
    const int B = static_cast<int>(g_live.buf[0].size());
    int done = 0;
    while (done < n) {
        const int m = std::min(B, n - done);
        g_live.engine->process(g_live.ptr.data(), m);
        for (int c = 0; c < N; ++c)
            std::memcpy(g_live.planar.data() + static_cast<std::size_t>(c) * n + done,
                        g_live.buf[static_cast<std::size_t>(c)].data(), static_cast<std::size_t>(m) * sizeof(float));
        done += m;
    }
    return g_live.planar.data();
}

EMSCRIPTEN_KEEPALIVE int bf_channels() { return g_live.engine ? g_live.engine->numChannels() : 0; }
EMSCRIPTEN_KEEPALIVE double bf_position() { return g_live.engine ? static_cast<double>(g_live.engine->position()) : 0.0; }

EMSCRIPTEN_KEEPALIVE const char* bf_stats() {
    if (!g_live.engine) return retErr("not running");
    const bf::MaskStatistics s = g_live.engine->statistics();
    json j = bf::toJson(s);
    j["ok"] = true;
    // Live / ANALYSIS-page view fields that toJson() leaves out (GUI.md sections 24, 39, 41-45).
    json v;
    v["rmsFastChDb"] = s.rmsFastChDb;
    v["truePeakChDb"] = s.truePeakChDb;
    v["rmsFastAllDb"] = s.rmsFastAllDb;
    v["truePeak10sDb"] = s.truePeak10sDb;
    v["rms10sDb"] = s.rms10sDb;
    v["haveSpectrum"] = s.haveSpectrumView;
    v["referenceDb"] = std::vector<double>(s.referenceDb.begin(), s.referenceDb.end());
    v["measuredDb"] = std::vector<double>(s.measuredDb.begin(), s.measuredDb.end());
    v["occupancy60s"] = s.occupancy60s;
    v["temporalDensity"] = static_cast<int>(s.temporalDensity);
    v["gapMean60s"] = s.gapMean60s;
    v["gapMax60s"] = s.gapMax60s;
    v["slotActive"] = s.slotActive;
    v["adjacentCorrelation"] = s.adjacentCorrelation;
    v["outputActiveFraction"] = s.outputActiveFraction;
    json dots = json::array();
    for (const auto& d : s.talkerDots)
        dots.push_back({d.active, d.azimuthDeg, d.pan, d.gain, d.home});
    v["talkerDots"] = dots;
    v["spatialAlgorithm"] = s.spatialAlgorithm;
    v["haveFft"] = s.haveFftView;
    if (s.haveFftView) v["fftDb"] = std::vector<float>(s.fftViewDb.begin(), s.fftViewDb.end());
    j["view"] = v;
    return ret(j);
}

EMSCRIPTEN_KEEPALIVE void bf_stop() {
    g_live.engine.reset();
}

// Parity check with the native bfrender: renders a scenario document with the current corpus
// (bf_corpus_synthetic first) and returns {"ok","sha256","rmsDb","frames","channels"}.
EMSCRIPTEN_KEEPALIVE const char* bf_render_scenario_sha(const char* scenarioJson, int blockSize) {
    if (!g_ds) return retErr("not initialised");
    try {
        bf::Scenario sc;
        std::string err;
        if (!bf::parseScenario(json::parse(scenarioJson), "/", sc, &err)) return retErr(err);
        bf::CorpusHandle h;
        h.snapshot = g_corpus.snapshot;
        h.audio = g_corpus.audio;
        bf::RenderOptions opt;
        if (blockSize > 0) opt.blockSize = blockSize;
        const bf::RenderResult r = bf::renderScenario(*g_ds, sc, h, opt);
        if (!r.ok) return retErr(r.error);
        double e = 0.0;
        std::size_t n = 0;
        for (const auto& ch : r.audio) {
            for (float v : ch) e += static_cast<double>(v) * v;
            n += ch.size();
        }
        return ret({{"ok", true}, {"sha256", bf::audioSha256(r.audio)}, {"rmsDb", n ? 10.0 * std::log10(e / n) : -200.0},
                    {"frames", r.audio.empty() ? 0 : r.audio[0].size()}, {"channels", r.numChannels}});
    } catch (const std::exception& e) {
        return retErr(e.what());
    }
}

}  // extern "C"
