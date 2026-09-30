#include "core/engine/OfflineRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include "core/Version.h"
#include "core/analysis/OfflineAnalysis.h"
#include "core/config/Sha256.h"
#include "core/io/WavWriter.h"
#include "core/spectrum/FirDesigner.h"

namespace bf {

namespace {

class Capture final : public ITapSink {
public:
    Capture(const std::vector<int>& taps, std::map<int, std::vector<std::vector<float>>>& out, int nCh,
            std::size_t limit)
        : out_(out), limit_(limit) {
        for (int t : taps)
            if (t >= 0 && t < kNumTaps) {
                want_[t] = true;
                out_[t].assign(static_cast<std::size_t>(nCh), {});
                for (auto& ch : out_[t]) ch.reserve(limit);
            }
    }
    void onTap(int tap, std::int64_t start, const float* const* ch, int nCh, int n) override {
        if (tap < 0 || tap >= kNumTaps || !want_[tap]) return;
        const auto s = static_cast<std::size_t>(start);
        if (s >= limit_) return;
        const std::size_t m = std::min(static_cast<std::size_t>(n), limit_ - s);
        auto& dst = out_[tap];
        for (int c = 0; c < nCh; ++c) dst[static_cast<std::size_t>(c)].insert(dst[static_cast<std::size_t>(c)].end(), ch[c], ch[c] + m);
    }

private:
    std::map<int, std::vector<std::vector<float>>>& out_;
    bool want_[kNumTaps] = {false, false, false, false};
    std::size_t limit_;
};

struct PassConfig {
    double durationS = 0.0;
    bool strictPreroll = false;
    std::optional<OperatingBands> initialCorrection;
    bool freezeCorrection = false;
    double staticGainDb = 0.0;
    bool capture = true;
};

struct PassOutput {
    bool ok = false;
    int exitCode = kRenderOk;
    std::string error;
    int numChannels = 0;
    std::vector<std::vector<float>> audio;
    std::map<int, std::vector<std::vector<float>>> taps;
    MaskStatistics stats;
    nlohmann::json events, history;
    OperatingBands correction{};
    bool limiterStage = true;
    double targetRmsDb = -26.0;
    std::string targetId;
    ThirdOctArray referenceDb{};
    bool babble = false;
    std::vector<std::string> warnings;
};

CorrectionSpeed speedFrom(const std::string& s) {
    if (s == "slow") return CorrectionSpeed::Slow;
    if (s == "fast") return CorrectionSpeed::Fast;
    return CorrectionSpeed::Normal;
}

double rmsAllDb(const std::vector<std::vector<float>>& a) {
    double e = 0.0;
    std::size_t n = 0;
    for (const auto& ch : a) {
        for (float v : ch) e += static_cast<double>(v) * v;
        n += ch.size();
    }
    return n && e > 0.0 ? 10.0 * std::log10(e / static_cast<double>(n)) : -200.0;
}

PassOutput runPass(const DataSet& ds, const Scenario& sc, const CorpusHandle& corpus, const RenderOptions& opt,
                   const PassConfig& pc) {
    PassOutput po;
    auto fail = [&](int code, const std::string& m) {
        po.exitCode = code;
        po.error = m;
        return po;
    };
    const CorpusSummary cs = corpus.snapshot ? CorpusSummary::from(*corpus.snapshot) : CorpusSummary{};
    const ScenarioPlan sp0 = buildScenarioPlan(ds, sc.preset, sc.lab, cs, sc.seed);
    if (!sp0.ok) return fail(kRenderConfigError, sp0.error);
    const MaskRenderPlan& plan0 = sp0.plan;
    const bool strict = plan0.fallback == FallbackPolicy::Strict || sc.lab.has_value();
    if (plan0.babbleEnabled && !corpus.snapshot)
        return fail(kRenderConfigError, "the plan needs a voice corpus (--corpus or --synthetic-corpus)");
    if (plan0.babbleEnabled && sc.sampleRate != 48000.0)
        return fail(kRenderConfigError, "babble rendering requires sampleRate 48000 in V1");
    if (sc.lab && plan0.babbleEnabled && plan0.talkers.mode == PlanMode::ContinuousN &&
        static_cast<int>(plan0.talkers.maxActive) > cs.availableSpeakers)
        return fail(kRenderSourceFailure, "laboratory continuousN needs " + std::to_string(plan0.talkers.maxActive) +
                                              " distinct speakers, corpus has " +
                                              std::to_string(cs.availableSpeakers) + " (Strict: aborted)");
    for (const auto& c : sp0.conflicts) po.warnings.push_back(c);

    MaskEngineConfig mc;
    mc.seed = sc.seed;
    mc.corpus = corpus.snapshot;
    mc.audio = corpus.audio;
    mc.lRefDbfs = plan0.level.lRefDbfs;
    mc.channels = sp0.preset.outputs.channels;
    mc.zones = sp0.preset.outputs.zones;
    mc.limiterStage = !opt.noLimiter && (!sc.lab || sc.lab->limiter);
    mc.correctionEnabled = sp0.preset.spectrum.correction.enabled.value_or(true);
    mc.correctionSpeed = speedFrom(sp0.preset.spectrum.correction.speed.value_or("normal"));
    mc.correctionStrict = pc.strictPreroll;
    mc.initialCorrection = pc.initialCorrection;
    mc.freezeCorrection = pc.freezeCorrection;
    mc.staticGainDb = pc.staticGainDb;
    po.limiterStage = mc.limiterStage;
    po.targetRmsDb = plan0.level.outputRmsDbfs();
    po.targetId = plan0.target.id;
    FirDesignParams fp;
    fp.fs = sc.sampleRate;
    fp.lfLimitHz = plan0.target.lfLimitHz;
    fp.hfLimitHz = plan0.target.hfLimitHz;
    po.referenceDb = idealBandLevelsDb(plan0.target.effectiveThirdOctDb(), fp);
    po.babble = plan0.babbleEnabled;

    MaskEngine engine(mc);
    std::string err;
    if (!engine.prepare(sc.sampleRate, sp0.layout, opt.blockSize, &err)) return fail(kRenderConfigError, err);
    engine.setPlan(plan0, 0);
    nlohmann::json doc = sc.preset;
    for (const ScenarioEvent& ev : sc.events) {
        for (auto it = ev.set.begin(); it != ev.set.end(); ++it) setDottedPath(doc, it.key(), it.value());
        const ScenarioPlan sp = buildScenarioPlan(ds, doc, sc.lab, cs, sc.seed);
        if (!sp.ok) return fail(kRenderConfigError, "event at " + std::to_string(ev.atS) + " s: " + sp.error);
        if (sp.layout.size() != sp0.layout.size())
            return fail(kRenderConfigError, "events must not change the output layout");
        engine.setPlan(sp.plan, static_cast<std::int64_t>(std::llround(ev.atS * sc.sampleRate)));
    }

    const int N = engine.numChannels();
    po.numChannels = N;
    const auto frames = static_cast<std::size_t>(std::llround(pc.durationS * sc.sampleRate));
    const auto latency = static_cast<std::size_t>(engine.latencySamples());
    const std::size_t total = frames + latency;
    Capture cap(opt.taps, po.taps, N, frames);
    if (pc.capture && !opt.taps.empty()) engine.setTapSink(&cap);
    if (pc.capture) po.audio.assign(static_cast<std::size_t>(N), std::vector<float>(frames, 0.0f));

    const auto B = static_cast<std::size_t>(std::max(opt.blockSize, 1));
    std::vector<std::vector<float>> buf(static_cast<std::size_t>(N), std::vector<float>(B));
    std::vector<float*> ptr(static_cast<std::size_t>(N));
    for (int c = 0; c < N; ++c) ptr[static_cast<std::size_t>(c)] = buf[static_cast<std::size_t>(c)].data();
    std::size_t done = 0;
    while (done < total) {
        const std::size_t m = std::min(B, total - done);
        engine.process(ptr.data(), static_cast<int>(m));
        if (pc.capture) {
            for (std::size_t i = 0; i < m; ++i) {
                const std::size_t t = done + i;
                if (t < latency) continue;
                for (std::size_t c = 0; c < static_cast<std::size_t>(N); ++c) po.audio[c][t - latency] = buf[c][i];
            }
        }
        done += m;
        if (strict && engine.sourceFailure())
            return fail(kRenderSourceFailure, "source failure at " +
                                                  std::to_string(static_cast<double>(done) / sc.sampleRate) +
                                                  " s (fallback policy Strict: render aborted)");
    }
    po.stats = engine.statistics();
    if (plan0.babbleEnabled && po.stats.babbleUnavailable)
        return fail(kRenderConfigError, "babble engine unavailable");
    if (engine.sourceFailure()) po.warnings.push_back("source failures occurred (fallback policy Continuous)");
    po.events = engine.eventsJson(static_cast<std::int64_t>(frames));
    po.history = engine.planHistoryJson();
    po.correction = engine.correction();
    po.ok = true;
    return po;
}

}  // namespace

RenderResult renderScenario(const DataSet& ds, const Scenario& sc, const CorpusHandle& corpus,
                            const RenderOptions& opt) {
    RenderResult r;
    r.fs = sc.sampleRate;
    r.corpusVersion = corpus.snapshot ? corpus.snapshot->corpusVersion() : std::string();
    if (!sc.corpusVersion.empty() && sc.corpusVersion != r.corpusVersion)
        r.warnings.push_back("scenario corpusVersion " + sc.corpusVersion + " differs from the loaded corpus " +
                             r.corpusVersion);
    auto take = [&](PassOutput& p) {
        r.ok = p.ok;
        r.exitCode = p.exitCode;
        r.error = p.error;
        r.numChannels = p.numChannels;
        r.audio = std::move(p.audio);
        r.taps = std::move(p.taps);
        r.stats = p.stats;
        r.events = std::move(p.events);
        r.planHistory = std::move(p.history);
        r.targetRmsDb = p.targetRmsDb;
        r.targetId = p.targetId;
        r.referenceDb = p.referenceDb;
        r.warnings.insert(r.warnings.end(), p.warnings.begin(), p.warnings.end());
    };

    PassConfig main;
    main.durationS = sc.durationS;
    nlohmann::json lab = nlohmann::json::object();
    if (sc.lab) {
        lab["mode"] = std::string(toString(sc.lab->mode));
        lab["talkers"] = sc.lab->talkers;
        lab["fallbackPolicy"] = "strict";
        lab["limiter"] = sc.lab->limiter && !opt.noLimiter;
        if (sc.lab->strictSpectrum) {
            PassConfig pre;
            pre.durationS = opt.strictPrerollS;
            pre.strictPreroll = true;
            pre.capture = false;
            PassOutput p = runPass(ds, sc, corpus, opt, pre);
            if (!p.ok) {
                take(p);
                return r;
            }
            if (p.babble) {
                main.initialCorrection = p.correction;
                main.freezeCorrection = true;
                lab["strictSpectrum"] = {{"prerollS", opt.strictPrerollS},
                                         {"tauCS", 20.0},
                                         {"correctionDb", std::vector<double>(p.correction.begin(), p.correction.end())},
                                         {"prerollBabbleThirdOctMaxDevDb", p.stats.babbleThirdOctMaxDevDb}};
            }
        }
    }
    PassOutput p1 = runPass(ds, sc, corpus, opt, main);
    if (!p1.ok) {
        take(p1);
        return r;
    }
    if (sc.lab && sc.lab->twoPassRms) {
        const double meas = rmsAllDb(p1.audio);
        const double gainDb = p1.targetRmsDb - meas;
        if (!p1.limiterStage) {
            // No non-linear stage after the master gain: applying the static gain to the
            // pass-1 render is identical to re-rendering with it.
            const double g = std::pow(10.0, gainDb / 20.0);
            for (auto& ch : p1.audio)
                for (float& v : ch) v = static_cast<float>(static_cast<double>(v) * g);
            if (p1.taps.count(3))
                for (auto& ch : p1.taps[3])
                    for (float& v : ch) v = static_cast<float>(static_cast<double>(v) * g);
            take(p1);
        } else {
            PassConfig second = main;
            second.staticGainDb = gainDb;
            PassOutput p2 = runPass(ds, sc, corpus, opt, second);
            take(p2);
            if (!r.ok) return r;
        }
        lab["twoPass"] = {{"pass1RmsDb", meas}, {"staticGainDb", gainDb}, {"finalRmsDb", rmsAllDb(r.audio)},
                          {"targetRmsDb", r.targetRmsDb}};
    } else {
        take(p1);
    }
    if (sc.lab && r.events.contains("events")) {
        // Speaker of each slot (first event per slot), in slot order: for continuousN this is the
        // nested "lab.speakerset" draw.
        std::map<std::int64_t, std::int64_t> bySlot;
        for (const auto& e : r.events["events"]) bySlot.emplace(e.value("slot", std::int64_t{0}), e.value("speaker", std::int64_t{-1}));
        std::vector<std::int64_t> spk;
        for (const auto& [slot, s] : bySlot) spk.push_back(s);
        lab["speakers"] = spk;
    }
    if (sc.lab) r.laboratory = lab;
    return r;
}

std::string audioSha256(const std::vector<std::vector<float>>& planar) {
    Sha256 h;
    if (planar.empty()) return h.finishHex();
    const std::size_t n = planar[0].size();
    std::vector<unsigned char> buf;
    buf.reserve(4096 * planar.size() * 4);
    for (std::size_t i = 0; i < n; ++i) {
        for (const auto& ch : planar) {
            std::uint32_t u;
            std::memcpy(&u, &ch[i], 4);
            buf.push_back(static_cast<unsigned char>(u & 0xFF));
            buf.push_back(static_cast<unsigned char>((u >> 8) & 0xFF));
            buf.push_back(static_cast<unsigned char>((u >> 16) & 0xFF));
            buf.push_back(static_cast<unsigned char>((u >> 24) & 0xFF));
        }
        if (buf.size() >= 4096 * planar.size() * 4) {
            h.update(buf.data(), buf.size());
            buf.clear();
        }
    }
    h.update(buf.data(), buf.size());
    return h.finishHex();
}

namespace {

bool writeWav(const std::filesystem::path& p, const std::vector<std::vector<float>>& a, double fs, std::string* error) {
    WavWriter w;
    if (!w.open(p.string(), static_cast<std::uint32_t>(fs), static_cast<std::uint16_t>(a.size()))) {
        if (error) *error = w.error();
        return false;
    }
    std::vector<const float*> ptr;
    for (const auto& ch : a) ptr.push_back(ch.data());
    const std::size_t n = a.empty() ? 0 : a[0].size();
    if (!w.writePlanar(ptr.data(), n) || !w.close()) {
        if (error) *error = w.error();
        return false;
    }
    return true;
}

bool writeText(const std::filesystem::path& p, const std::string& s, std::string* error) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (error) *error = "cannot write " + p.string();
        return false;
    }
    f << s << '\n';
    return static_cast<bool>(f);
}

std::filesystem::path withSuffix(const std::filesystem::path& wav, const std::string& suffix) {
    std::filesystem::path p = wav;
    p.replace_extension();
    p += suffix;
    return p;
}

}  // namespace

bool writeRenderOutputs(const RenderResult& r, const Scenario& sc, const DataSet& ds, const std::filesystem::path& wav,
                        nlohmann::json* sidecarOut, std::string* error, const std::filesystem::path& eventsPath) {
    if (!r.ok) {
        if (error) *error = "nothing to write: " + r.error;
        return false;
    }
    if (wav.has_parent_path()) std::filesystem::create_directories(wav.parent_path());
    if (!writeWav(wav, r.audio, r.fs, error)) return false;
    const std::filesystem::path ev = eventsPath.empty() ? withSuffix(wav, ".events.json") : eventsPath;
    if (!writeText(ev, r.events.dump(1), error)) return false;
    nlohmann::json taps = nlohmann::json::object();
    for (const auto& [t, a] : r.taps) {
        const std::string name = "T" + std::to_string(t + 1);
        const auto tp = withSuffix(wav, "." + name + ".wav");
        if (!writeWav(tp, a, r.fs, error)) return false;
        taps[name] = {{"file", tp.filename().string()}, {"audioSha256", audioSha256(a)}};
    }

    std::vector<const float*> ptr;
    for (const auto& ch : r.audio) ptr.push_back(ch.data());
    OfflineAnalysisOptions ao;
    ao.targetDb = r.referenceDb;
    ao.targetId = r.targetId;
    ao.events = &r.events;
    nlohmann::json sc2 = sc.doc;
    nlohmann::json side;
    side["schema"] = "babbleforge.render-sidecar/1";
    side["appVersion"] = versionString();
    side["scenario"] = sc2;
    side["seed"] = sc.seed;
    side["corpusVersion"] = r.corpusVersion;
    side["dataSetHash"] = ds.dataSetHash;
    side["sampleRate"] = r.fs;
    side["channels"] = r.numChannels;
    side["frames"] = r.audio.empty() ? 0 : r.audio[0].size();
    side["durationS"] = sc.durationS;
    side["sampleFormat"] = "float32";
    side["wav"] = wav.filename().string();
    side["events"] = ev.filename().string();
    side["audioSha256"] = audioSha256(r.audio);
    side["planHistory"] = r.planHistory;
    side["warnings"] = r.warnings;
    if (!r.laboratory.is_null()) side["laboratory"] = r.laboratory;
    if (!taps.empty()) side["taps"] = taps;
    side["engine"] = toJson(r.stats);
    side["metrics"] = analyzeAudio(ptr.data(), ptr.size(), r.audio.empty() ? 0 : r.audio[0].size(), r.fs, ao);
    if (!writeText(withSuffix(wav, ".sidecar.json"), side.dump(1), error)) return false;
    if (sidecarOut) *sidecarOut = std::move(side);
    return true;
}

int runMatrix(const DataSet& ds, const nlohmann::json& m, const std::filesystem::path& baseDir,
              const CorpusHandle& corpus, const std::filesystem::path& outDir, const RenderOptions& opt,
              std::string* error, nlohmann::json* manifestOut) {
    auto fail = [&](const std::string& s) {
        if (error) *error = s;
        return static_cast<int>(kRenderConfigError);
    };
    if (!m.is_object() || m.value("schema", std::string()) != "babbleforge.matrix/1")
        return fail("matrix: expected schema babbleforge.matrix/1");
    nlohmann::json base;
    std::filesystem::path scenarioDir = baseDir;
    if (m.contains("base") && m.at("base").is_string()) {
        std::filesystem::path bp = m.at("base").get<std::string>();
        if (bp.is_relative()) bp = baseDir / bp;
        std::ifstream f(bp, std::ios::binary);
        if (!f) return fail("matrix: cannot open base scenario " + bp.string());
        std::stringstream ss;
        ss << f.rdbuf();
        try {
            base = nlohmann::json::parse(ss.str());
        } catch (const std::exception& e) {
            return fail(std::string("matrix: base scenario: ") + e.what());
        }
        scenarioDir = bp.parent_path();
    } else if (m.contains("base") && m.at("base").is_object()) {
        base = m.at("base");
    } else {
        return fail("matrix: missing \"base\"");
    }
    // Inline a preset given by path so that axes can address its fields.
    if (base.contains("preset") && base["preset"].is_string()) {
        Scenario tmp;
        std::string e;
        if (!parseScenario(base, scenarioDir, tmp, &e)) return fail("matrix: " + e);
        base["preset"] = tmp.preset;
    }
    if (m.contains("durationS")) base["durationS"] = m.at("durationS");

    std::vector<std::pair<std::string, std::vector<nlohmann::json>>> axes;
    if (m.contains("axes"))
        for (auto it = m.at("axes").begin(); it != m.at("axes").end(); ++it) {
            if (!it.value().is_array() || it.value().empty()) return fail("matrix: axis " + it.key() + " must be a non-empty array");
            axes.emplace_back(it.key(), std::vector<nlohmann::json>(it.value().begin(), it.value().end()));
        }
    std::size_t cells = 1;
    for (const auto& a : axes) cells *= a.second.size();
    std::filesystem::create_directories(outDir);

    nlohmann::json manifest;
    manifest["schema"] = "babbleforge.matrix-manifest/1";
    manifest["appVersion"] = versionString();
    manifest["dataSetHash"] = ds.dataSetHash;
    manifest["matrix"] = m;
    if (m.contains("targetSpeech"))
        manifest["targetSpeech"] = {{"status", "not implemented"},
                                    {"note", "TODO: target+masker mixtures at the listed speech-to-masker ratios "
                                             "(P.56 ASL of the target vs masker RMS over the target duration) are "
                                             "not rendered yet; only the masker cells are produced"}};
    nlohmann::json list = nlohmann::json::array();
    int worst = kRenderOk;
    for (std::size_t idx = 0; idx < cells; ++idx) {
        nlohmann::json doc = base;
        nlohmann::json settings = nlohmann::json::object();
        std::size_t rem = idx;
        for (std::size_t a = axes.size(); a-- > 0;) {
            const auto& [key, vals] = axes[a];
            const nlohmann::json& v = vals[rem % vals.size()];
            rem /= vals.size();
            settings[key] = v;
            if (key.rfind("preset.", 0) == 0)
                setDottedPath(doc["preset"], key.substr(7), v);
            else
                setDottedPath(doc, key, v);
        }
        char name[32];
        std::snprintf(name, sizeof name, "cell_%03zu", idx);
        nlohmann::json entry;
        entry["index"] = idx;
        entry["name"] = name;
        entry["settings"] = settings;
        Scenario sc;
        std::string e;
        int code = kRenderOk;
        if (!parseScenario(doc, scenarioDir, sc, &e)) {
            code = kRenderConfigError;
            entry["error"] = e;
        } else {
            const RenderResult r = renderScenario(ds, sc, corpus, opt);
            if (!r.ok) {
                code = r.exitCode;
                entry["error"] = r.error;
            } else {
                nlohmann::json side;
                const auto wav = outDir / (std::string(name) + ".wav");
                if (!writeRenderOutputs(r, sc, ds, wav, &side, &e)) {
                    code = kRenderIoError;
                    entry["error"] = e;
                } else {
                    entry["wav"] = wav.filename().string();
                    entry["sidecar"] = std::string(name) + ".sidecar.json";
                    entry["audioSha256"] = side["audioSha256"];
                    entry["rmsDb"] = side["metrics"]["level"]["rmsDb"];
                }
            }
        }
        entry["exitCode"] = code;
        worst = std::max(worst, code);
        list.push_back(std::move(entry));
    }
    manifest["cells"] = list;
    std::string e;
    if (!writeText(outDir / "matrix_manifest.json", manifest.dump(1), &e)) {
        if (error) *error = e;
        return kRenderIoError;
    }
    if (manifestOut) *manifestOut = std::move(manifest);
    return worst;
}

}  // namespace bf
