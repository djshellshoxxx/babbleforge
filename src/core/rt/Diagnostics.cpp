#include "core/rt/Diagnostics.h"

#include <algorithm>
#include <cmath>

#include "core/rt/Logging.h"

namespace bf::rt {

namespace {
const char* osName() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "unknown";
#endif
}
const char* cpuArch() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#else
    return "unknown";
#endif
}
double finiteOr(double v, double d) { return std::isfinite(v) ? v : d; }
}  // namespace

std::string_view toString(Health h) noexcept {
    switch (h) {
    case Health::Good: return "good";
    case Health::Warning: return "warning";
    case Health::Bad: return "bad";
    }
    return "good";
}

bool HealthReport::outputHealthy() const noexcept {
    return !error && dspLoad != Health::Bad && preload != Health::Bad && xruns != Health::Bad &&
           limiter != Health::Bad && corpus != Health::Bad;
}

HealthReport evaluateHealth(double p99, double minBuf, double xpm, LimiterHealth lim, CorpusHealth corpus, bool err) {
    HealthReport h;
    h.dspLoad = p99 < 0.5 ? Health::Good : (p99 <= 0.8 ? Health::Warning : Health::Bad);
    h.preload = minBuf > 3.0 ? Health::Good : (minBuf >= 1.0 ? Health::Warning : Health::Bad);
    h.xruns = xpm <= 0.0 ? Health::Good : (xpm <= 10.0 ? Health::Warning : Health::Bad);
    h.limiter = lim == LimiterHealth::Inactive ? Health::Good : (lim == LimiterHealth::Active ? Health::Warning : Health::Bad);
    h.corpus = corpus == CorpusHealth::Healthy ? Health::Good
                                               : (corpus == CorpusHealth::Reduced ? Health::Warning : Health::Bad);
    h.error = err;
    return h;
}

nlohmann::json toJson(const HealthReport& h) {
    return {{"dspLoad", toString(h.dspLoad)}, {"preload", toString(h.preload)}, {"xruns", toString(h.xruns)},
            {"limiter", toString(h.limiter)}, {"corpus", toString(h.corpus)},   {"outputHealthy", h.outputHealthy()}};
}

nlohmann::json buildDiagnosticsSnapshot(const DiagnosticsInput& in, bool redact) {
    const RtMetrics& m = in.metrics;
    const MaskStatistics& s = in.stats;
    nlohmann::json j;
    j["schema"] = "babbleforge.diagnostics/1";
    j["generated"] = isoUtcNow();
    j["app"] = {{"version", in.appVersion}, {"build", in.build}, {"os", osName()}, {"cpu", cpuArch()}, {"ramGB", nullptr}};
    j["engine"] = {{"state", toString(in.state)},
                   {"degradedReasons", degradedReasonCodes(in.degradedReasons)},
                   {"uptimeS", in.uptimeS},
                   {"sessionSeed", in.sessionSeed},
                   {"corpusVersion", in.corpusVersion},
                   {"dataSetHash", in.dataSetHash},
                   {"determinismBroken", m.determinismBroken},
                   {"planEpoch", in.planEpoch}};
    j["audio"] = {{"driver", in.driver},
                  {"device", in.device},
                  {"sampleRate", in.sampleRate},
                  {"bufferFrames", in.bufferFrames},
                  {"outputs", in.outputs},
                  {"latencyMs", in.latencyMs},
                  {"dspLoad", {{"p50", m.dspLoadP50}, {"p99", m.dspLoadP99}, {"max", m.dspLoadMax}}},
                  {"xruns", m.xruns},
                  {"overruns", m.overruns},
                  {"callbackGaps", m.callbackGaps}};
    j["cpu"] = {{"processPct", in.processCpuPct}};
    const double occ = s.meanActive > 0.0 ? 1.0 : 0.0;
    j["talkers"] = {{"strategy", in.strategy},
                    {"meanActiveTarget", in.meanActiveTarget},
                    {"meanActive60s", s.meanActive},
                    {"meanSpeaking60s", s.meanSpeaking},
                    {"activeNow", s.maxActive > 0 ? static_cast<int>(std::lround(s.meanActive)) : 0},
                    {"slots", in.slots},
                    {"pool", in.pool},
                    {"occupancy60s", occ},
                    {"soloExposurePct", nullptr}};
    j["preload"] = {{"minBufferedS", m.minBufferedS},
                    {"p99ReadLatencyMs", m.p99ReadLatencyMs},
                    {"lateStarts", m.lateStarts},
                    {"starvations", m.starvations},
                    {"blockPoolUsedPct", m.blockPoolUsedPct}};
    j["corpus"] = {{"speakers", in.corpusSpeakers},
                   {"usableSpeechH", in.usableSpeechH},
                   {"unhealthyRecordings", m.unhealthyRecordings},
                   {"segmentCooldownEffMin", in.segmentCooldownEffMin}};
    double corrMax = 0.0;
    for (double v : s.correctionDb) corrMax = std::max(corrMax, std::fabs(v));
    j["spectrum"] = {{"target", in.target},
                     {"babbleRmsDevDb", s.haveBabbleSpectrum ? nlohmann::json(s.babbleThirdOctMaxDevDb) : nlohmann::json()},
                     {"stationaryMaxDevDb",
                      s.haveStationarySpectrum ? nlohmann::json(s.stationaryThirdOctMaxDevDb) : nlohmann::json()},
                     {"correctionMaxDb", corrMax},
                     {"correctionState", corrMax >= 4.0 ? "saturated" : (s.haveBabbleSpectrum ? "tracking" : "idle")}};
    j["limiter"] = {{"enabled", in.limiterEnabled},
                    {"ceilingDbtp", in.limiterCeilingDbtp},
                    {"grNowDb", 0.0},
                    {"grMax60sDb", finiteOr(s.limiterGrMaxDb, 0.0)},
                    {"pctTimeActive60s", 100.0 * s.limiterActiveFraction},
                    {"sustained", in.limiterHealth == LimiterHealth::Sustained},
                    {"clipEvents", s.clipEvents}};
    nlohmann::json outs = nlohmann::json::array();
    for (const OutputInfo& o : in.outputsInfo) {
        const auto idx = static_cast<std::size_t>(o.index);
        outs.push_back({{"index", o.index},
                        {"label", o.label},
                        {"enabled", o.enabled},
                        {"rmsLeq60Dbfs", idx < s.outputRmsChDb.size() ? nlohmann::json(s.outputRmsChDb[idx]) : nlohmann::json()},
                        {"truePeakMaxDbtp", in.haveStats ? nlohmann::json(s.truePeakMaxDb) : nlohmann::json()},
                        {"gainDb", o.gainDb},
                        {"mute", o.mute},
                        {"zone", o.zone}});
    }
    j["outputs"] = outs;
    j["correlation"] = {{"maxAdjacent", nullptr}, {"median", nullptr}};
    j["counters"] = {{"substitutions", m.substitutions}, {"logDropped", in.logDropped}, {"tapOverflow", m.tapOverflowFrames}};
    nlohmann::json recent = nlohmann::json::array();
    for (nlohmann::json e : in.recentLog) {
        if (redact) Logger::redactJson(e);
        recent.push_back(std::move(e));
    }
    j["recentLog"] = recent;
    const HealthReport h = evaluateHealth(m.dspLoadP99, m.babble ? m.minBufferedS : 8.0, in.xrunsPerMinute,
                                          in.limiterHealth, in.corpusHealth, in.state == EngineState::Error);
    j["health"] = toJson(h);
    return j;
}

const std::vector<std::string>& diagnosticsRequiredPaths() {
    static const std::vector<std::string> p = {
        "/schema", "/generated", "/app/version", "/app/build", "/app/os", "/app/cpu", "/app/ramGB",
        "/engine/state", "/engine/degradedReasons", "/engine/uptimeS", "/engine/sessionSeed", "/engine/corpusVersion",
        "/engine/dataSetHash", "/engine/determinismBroken", "/engine/planEpoch",
        "/audio/driver", "/audio/device", "/audio/sampleRate", "/audio/bufferFrames", "/audio/outputs",
        "/audio/latencyMs", "/audio/dspLoad/p50", "/audio/dspLoad/p99", "/audio/dspLoad/max", "/audio/xruns",
        "/audio/overruns", "/audio/callbackGaps", "/cpu/processPct",
        "/talkers/strategy", "/talkers/meanActiveTarget", "/talkers/meanActive60s", "/talkers/meanSpeaking60s",
        "/talkers/activeNow", "/talkers/slots", "/talkers/pool", "/talkers/occupancy60s", "/talkers/soloExposurePct",
        "/preload/minBufferedS", "/preload/p99ReadLatencyMs", "/preload/lateStarts", "/preload/starvations",
        "/preload/blockPoolUsedPct", "/corpus/speakers", "/corpus/usableSpeechH", "/corpus/unhealthyRecordings",
        "/corpus/segmentCooldownEffMin", "/spectrum/target", "/spectrum/babbleRmsDevDb", "/spectrum/stationaryMaxDevDb",
        "/spectrum/correctionMaxDb", "/spectrum/correctionState", "/limiter/enabled", "/limiter/ceilingDbtp",
        "/limiter/grNowDb", "/limiter/grMax60sDb", "/limiter/pctTimeActive60s", "/limiter/sustained",
        "/limiter/clipEvents", "/outputs", "/correlation/maxAdjacent", "/correlation/median",
        "/counters/substitutions", "/counters/logDropped", "/counters/tapOverflow", "/recentLog", "/health/outputHealthy"};
    return p;
}

}  // namespace bf::rt
