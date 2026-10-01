#include "core/analysis/OfflineAnalysis.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

#include "core/analysis/Meters.h"
#include "core/analysis/ModulationAnalyzer.h"
#include "core/analysis/SpectrumAnalyzer.h"

namespace bf {

namespace {

double powDb(double p) { return p > 1e-30 ? 10.0 * std::log10(p) : -200.0; }

double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double pos = q * static_cast<double>(v.size() - 1);
    const auto i = static_cast<std::size_t>(pos);
    const double f = pos - static_cast<double>(i);
    return i + 1 < v.size() ? v[i] * (1.0 - f) + v[i + 1] * f : v[i];
}

double pearson(const float* a, const float* b, std::size_t n) {
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = a[i], y = b[i];
        sa += x;
        sb += y;
        saa += x * x;
        sbb += y * y;
        sab += x * y;
    }
    const double dn = static_cast<double>(n);
    const double cov = sab - sa * sb / dn;
    const double va = saa - sa * sa / dn, vb = sbb - sb * sb / dn;
    return va > 0.0 && vb > 0.0 ? cov / std::sqrt(va * vb) : 0.0;
}

nlohmann::json talkerStats(const nlohmann::json& ev, std::size_t numFrames) {
    nlohmann::json t;
    if (!ev.contains("events") || !ev.at("events").is_array()) return t;
    const auto frames = static_cast<std::int64_t>(numFrames);
    std::map<std::int64_t, int> delta;
    std::set<std::int64_t> speakers;
    std::size_t count = 0;
    double activeSamples = 0.0;
    for (const auto& e : ev.at("events")) {
        const std::int64_t a = std::max<std::int64_t>(0, e.at("start").get<std::int64_t>());
        const std::int64_t b = std::min<std::int64_t>(frames, e.at("end").get<std::int64_t>());
        if (b <= a) continue;
        ++count;
        speakers.insert(e.value("speaker", std::int64_t{0}));
        delta[a] += 1;
        delta[b] -= 1;
        activeSamples += static_cast<double>(b - a);
    }
    int k = 0, kMin = 1 << 30, kMax = 0;
    std::int64_t prev = 0;
    for (const auto& [s, d] : delta) {
        if (s > prev) {
            kMin = std::min(kMin, k);
            kMax = std::max(kMax, k);
        }
        k += d;
        prev = s;
    }
    if (prev < frames) {
        kMin = std::min(kMin, k);
        kMax = std::max(kMax, k);
    }
    t["events"] = count;
    t["speakers"] = speakers.size();
    t["meanActive"] = frames > 0 ? activeSamples / static_cast<double>(frames) : 0.0;
    t["minActive"] = delta.empty() ? 0 : kMin;
    t["maxActive"] = kMax;
    return t;
}

}  // namespace

nlohmann::json analyzeAudio(const float* const* x, std::size_t nCh, std::size_t n, double fs,
                            const OfflineAnalysisOptions& opt) {
    nlohmann::json j;
    j["sampleRate"] = fs;
    j["channels"] = nCh;
    j["frames"] = n;
    j["durationS"] = static_cast<double>(n) / fs;
    if (nCh == 0 || n == 0) return j;

    // ---- level ------------------------------------------------------------------------------
    std::vector<double> rmsCh;
    double eAll = 0.0;
    float spk = 0.0f;
    for (std::size_t c = 0; c < nCh; ++c) {
        double e = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            e += static_cast<double>(x[c][i]) * x[c][i];
            spk = std::max(spk, std::fabs(x[c][i]));
        }
        rmsCh.push_back(powDb(e / static_cast<double>(n)));
        eAll += e;
    }
    const double rmsAll = powDb(eAll / static_cast<double>(n * nCh));
    Meters meters;
    meters.prepare(fs, static_cast<int>(nCh));
    const std::size_t step = static_cast<std::size_t>(std::llround(fs * 0.1));
    std::vector<const float*> p(nCh);
    double lufsSMax = -200.0;
    for (std::size_t off = 0, k = 0; off < n; off += step, ++k) {
        const std::size_t m = std::min(step, n - off);
        for (std::size_t c = 0; c < nCh; ++c) p[c] = x[c] + off;
        meters.process(p.data(), static_cast<int>(m));
        if (k % 10 == 9 && off + m >= static_cast<std::size_t>(3.0 * fs))
            lufsSMax = std::max(lufsSMax, meters.snapshot().lufsS);
    }
    const MeterSnapshot ms = meters.snapshot();
    double tp = -200.0;
    for (double v : ms.truePeakMaxDb) tp = std::max(tp, v);
    j["level"] = {{"rmsDb", rmsAll},
                  {"rmsChDb", rmsCh},
                  {"samplePeakDb", powDb(static_cast<double>(spk) * spk)},
                  {"truePeakDbtp", tp},
                  {"crestDb", tp - rmsAll},
                  {"lufsShortTermMax", lufsSMax},
                  {"lufsShortTermFinal", ms.lufsS},
                  {"lufsIntegrated", ms.lufsI},
                  {"lufsNote", nCh > 2 ? "LUFS (unweighted multichannel)" : "LUFS"}};

    // ---- spectrum ---------------------------------------------------------------------------
    const SpectrumAnalysis sa = analyzeSpectrum(x, nCh, n, fs, opt.targetDb ? &*opt.targetDb : nullptr);
    nlohmann::json sp;
    sp["thirdOctCentresHz"] = std::vector<double>(thirdOctNominalHz().begin(), thirdOctNominalHz().end());
    sp["thirdOctDb"] = std::vector<double>(sa.overallDb.begin(), sa.overallDb.end());
    sp["octaveCentresHz"] = std::vector<double>(octaveNominalHz().begin(), octaveNominalHz().end());
    sp["octaveDb"] = std::vector<double>(sa.octaveDb.begin(), sa.octaveDb.end());
    if (opt.targetDb) {
        const ShapeMetrics& s = sa.shape;
        double max125 = 0.0;
        for (std::size_t i = 1; i + 1 < kNumOperatingBands; ++i) max125 = std::max(max125, std::fabs(s.d[i]));
        const OctaveArray ro = octaveFromThirdOct(*opt.targetDb);
        std::vector<double> od(kNumOctaveBands);
        double wsum = 0.0, mu = 0.0;
        for (std::size_t k = 0; k < kNumOctaveBands; ++k) {
            const double w = std::pow(10.0, ro[k] / 10.0);
            mu += w * (sa.octaveDb[k] - ro[k]);
            wsum += w;
        }
        mu /= std::max(wsum, 1e-30);
        double omax = 0.0;
        for (std::size_t k = 0; k < kNumOctaveBands; ++k) {
            od[k] = sa.octaveDb[k] - ro[k] - mu;
            omax = std::max(omax, std::fabs(od[k]));
        }
        sp["target"] = opt.targetId;
        sp["deviation"] = {{"thirdOctDb100to10k", std::vector<double>(s.d.begin(), s.d.end())},
                           {"maxAbsThirdOct125to8kDb", max125},
                           {"maxAbsThirdOct100to10kDb", s.maxAbsDev},
                           {"rms125to8kDb", s.rmsDev125to8k},
                           {"octaveDb", od},
                           {"maxAbsOctaveDb", omax}};
    }
    j["spectrum"] = sp;

    // ---- temporal ---------------------------------------------------------------------------
    const std::size_t fl = static_cast<std::size_t>(std::llround(fs / 100.0));
    ModulationAnalyzer mod(false, 100.0);
    ModulationSpectrumTracker mst(100.0);
    std::vector<double> frameDb;
    ModBandArray modAcc{};
    std::size_t modN = 0;
    for (std::size_t off = 0, f = 0; off + fl <= n; off += fl, ++f) {
        double e = 0.0;
        for (std::size_t c = 0; c < nCh; ++c)
            for (std::size_t i = 0; i < fl; ++i) e += static_cast<double>(x[c][off + i]) * x[c][off + i];
        const double pw = e / static_cast<double>(fl * nCh);
        mod.pushFrame(pw);
        mst.push(pw);
        frameDb.push_back(powDb(pw));
        if (f % 6000 == 5999 && mst.has()) {
            const ModBandArray a = mst.average();
            for (std::size_t k = 0; k < kNumModBands; ++k) modAcc[k] += a[k];
            ++modN;
        }
    }
    if (modN == 0 && mst.has()) {
        modAcc = mst.average();
        modN = 1;
    }
    std::vector<double> gaps;
    for (const auto& g : mod.allGaps()) gaps.push_back(g.seconds);
    const double durS = static_cast<double>(n) / fs;
    const double l10 = percentile(frameDb, 0.9), l90 = percentile(frameDb, 0.1);
    const ModulationSnapshot msn = mod.snapshot();
    nlohmann::json tm;
    tm["envelope"] = {{"L10Db", l10}, {"L90Db", l90}, {"L10minusL90Db", l10 - l90},
                      {"modulationDepth10s", msn.modulationDepth10s}};
    const double edges[] = {0.0, 0.05, 0.1, 0.2, 0.5, 1.0, 1e9};
    std::vector<std::size_t> hist(6, 0);
    for (double g : gaps)
        for (std::size_t b = 0; b < 6; ++b)
            if (g >= edges[b] && g < edges[b + 1]) ++hist[b];
    tm["gaps"] = {{"count", gaps.size()},
                  {"medianS", percentile(gaps, 0.5)},
                  {"p95S", percentile(gaps, 0.95)},
                  {"maxS", gaps.empty() ? 0.0 : *std::max_element(gaps.begin(), gaps.end())},
                  {"ratePerS", durS > 0 ? static_cast<double>(gaps.size()) / durS : 0.0},
                  {"histogramEdgesS", {0.0, 0.05, 0.1, 0.2, 0.5, 1.0}},
                  {"histogram", hist}};
    if (modN > 0) {
        nlohmann::json m = nlohmann::json::object();
        const auto& fc = modulationBandCentresHz();
        for (std::size_t k = 0; k < kNumModBands; ++k) m[std::to_string(fc[k])] = modAcc[k] / static_cast<double>(modN);
        tm["modulationSpectrum"] = m;
    }
    j["temporal"] = tm;

    // ---- correlation ------------------------------------------------------------------------
    if (nCh > 1) {
        const std::size_t w = static_cast<std::size_t>(std::llround(10.0 * fs));
        nlohmann::json mat = nlohmann::json::array();
        nlohmann::json win = nlohmann::json::array();
        double maxAdj = 0.0;
        for (std::size_t a = 0; a < nCh; ++a) {
            nlohmann::json row = nlohmann::json::array(), wrow = nlohmann::json::array();
            for (std::size_t b = 0; b < nCh; ++b) {
                row.push_back(a == b ? 1.0 : pearson(x[a], x[b], n));
                double mw = 0.0;
                if (a != b && n >= w)
                    for (std::size_t off = 0; off + w <= n; off += w)
                        mw = std::max(mw, std::fabs(pearson(x[a] + off, x[b] + off, w)));
                wrow.push_back(a == b ? 1.0 : mw);
                if (b == (a + 1) % nCh && a != b) maxAdj = std::max(maxAdj, mw);
            }
            mat.push_back(row);
            win.push_back(wrow);
        }
        j["correlation"] = {{"matrix", mat}, {"max10sAbs", win}, {"maxAdjacent10sAbs", maxAdj}};
    }

    if (opt.events) j["talkers"] = talkerStats(*opt.events, n);
    return j;
}

}  // namespace bf
