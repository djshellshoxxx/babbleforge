#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "core/corpus/ingest/Analyzer.h"
#include "core/corpus/ingest/Features.h"
#include "core/corpus/ingest/Signal.h"
#include "ingest_fixtures.h"

using namespace bf;
using namespace bf::ingest;
using namespace bftest;
using Catch::Approx;

namespace {

const std::vector<float>& goodSpeech() {
    static const auto x = speechLike(35.0, 11);
    return x;
}
const RecordingAnalysis& goodAnalysis() {
    static const auto a = analyzeAudio(toDecoded(goodSpeech()));
    return a;
}

}  // namespace

TEST_CASE("analyzer: clean speech-like recording is Good", "[ingest][pipeline]") {
    const auto& a = goodAnalysis();
    INFO("reasons: " << (a.reasons.empty() ? "" : a.reasons[0]));
    CHECK(a.qualityClass == QualityClass::Good);
    CHECK(a.vadEngine == "webrtc-fvad-m2");
    CHECK(a.length == static_cast<std::int64_t>(goodSpeech().size()));
    CHECK(a.speechRatio > 0.5);
    CHECK(a.speechRatio < 0.98);
    CHECK(a.aslDb == Approx(-26.6).margin(1.0));
    CHECK(a.snrDb > 35.0);
    CHECK(a.bandwidthHz > 11000.0);
    CHECK(a.clipRatio == 0.0);
    CHECK(a.peakDb < -3.0);
    CHECK(a.truePeakDb >= a.peakDb - 0.1);
    CHECK(a.rmsDb < a.aslDb);  // activity < 100 %
    CHECK(a.activityPct > 40.0);
    CHECK(a.activityPct < 100.0);
    CHECK(std::fabs(a.dcOffset) < 1e-3);
    CHECK(a.pcmSha256.size() == 64);
    CHECK_FALSE(a.fingerprint.empty());
    double ps = 0;
    for (double p : a.ltassPower) ps += p;
    CHECK(ps == Approx(1.0).margin(1e-9));
    CHECK(a.spectralCentroidHz > 300.0);
    CHECK(a.spectralCentroidHz < 6000.0);
}

TEST_CASE("analyzer: segmentation records are consistent", "[ingest][pipeline]") {
    const auto& a = goodAnalysis();
    REQUIRE(a.regions.size() > 3);
    REQUIRE_FALSE(a.segments.empty());
    for (std::size_t i = 1; i < a.regions.size(); ++i) {
        CHECK(a.regions[i].start - a.regions[i - 1].end >= kMinPauseSamples);  // merged < 80 ms gaps
    }
    CHECK(a.pauses.size() + 1 == a.regions.size());
    std::size_t hist = 0;
    for (auto h : a.pauseHist) hist += h;
    CHECK(hist == a.pauses.size());
    CHECK(a.pauseFracGt100 >= a.pauseFracGt250);
    CHECK(a.pauseFracGt250 >= a.pauseFracGt600);
    CHECK(a.pauseFracGt100 <= 1.0);
    for (const auto& s : a.segments) {
        // The anchor is a region start, preceded by >= 150 ms (or the file start).
        const auto it = std::find_if(a.regions.begin(), a.regions.end(), [&](const SampleSpan& r) { return r.start == s.anchor; });
        REQUIRE(it != a.regions.end());
        const auto prevEnd = it == a.regions.begin() ? 0 : (it - 1)->end;
        CHECK(s.anchor - prevEnd >= kPhraseBoundarySamples);
        CHECK(a.length - s.anchor >= 2 * kCorpusRate);
        CHECK(s.maxEnd == a.length);
        CHECK(s.speechFrac >= 0.0f);
        CHECK(s.speechFrac <= 1.0f);
        CHECK(s.longestPhraseS > 0.0f);
        CHECK(s.asl10sDb == Approx(-26.6).margin(3.0));
        CHECK(s.soloRisk == (s.speechFrac > 0.9f && s.longestPhraseS > 4.0f));
    }
}

TEST_CASE("analyzer: degraded versions are classified", "[ingest][pipeline][quality]") {
    const auto& x = goodSpeech();
    SECTION("clipped") {
        const auto a = analyzeAudio(toDecoded(clipped(x, 25.0f)));
        CHECK(a.qualityClass == QualityClass::Rejected);
        CHECK(a.hasReason("reject.clipping"));
        CHECK(a.clipRatio > 1e-4);
        CHECK(a.clipRunMs > 1.0);
    }
    SECTION("noisy (SNR ~15 dB)") {
        const auto a = analyzeAudio(toDecoded(addWhiteNoise(x, -41.0)));
        CHECK(a.qualityClass == QualityClass::Rejected);
        CHECK(a.hasReason("reject.snr"));
        CHECK(a.snrDb == Approx(15.0).margin(2.5));
    }
    SECTION("moderately noisy is Usable with a warning") {
        const auto a = analyzeAudio(toDecoded(addWhiteNoise(x, -62.0)));  // SNR ~ 36 dB -> warn threshold region
        CHECK(a.qualityClass != QualityClass::Rejected);
        CHECK(a.snrDb > 30.0);
        const auto b = analyzeAudio(toDecoded(addWhiteNoise(x, -54.0)));  // SNR ~ 28 dB
        CHECK(b.hasReason("warn.snr"));
        CHECK(b.qualityClass == QualityClass::Usable);
        CHECK(b.qualityScore < a.qualityScore);
    }
    SECTION("band-limited at 9 kHz is usable-bandwidth (warning), 5 kHz is rejected") {
        const auto a = analyzeAudio(toDecoded(lowpass(x, 9000.0)));
        CHECK(a.bandwidthHz > 7500.0);
        CHECK(a.bandwidthHz < 11000.0);
        CHECK(a.hasReason("warn.bandwidth"));
        CHECK(a.qualityClass != QualityClass::Rejected);
        const auto b = analyzeAudio(toDecoded(lowpass(x, 5000.0)));
        CHECK(b.bandwidthHz < 7500.0);
        CHECK(b.qualityClass == QualityClass::Rejected);
        CHECK(b.hasReason("reject.bandwidth"));
    }
    SECTION("low source sample rate is rejected, 32 kHz warns") {
        const auto y16 = resample(x.data(), x.size(), 48000.0, 16000.0);
        const auto a = analyzeAudio(toDecoded(y16, 16000));
        CHECK(a.hasReason("reject.sample_rate"));
        const auto y32 = resample(x.data(), x.size(), 48000.0, 32000.0);
        const auto b = analyzeAudio(toDecoded(y32, 32000));
        CHECK(b.hasReason("warn.sample_rate"));
        CHECK(b.length == static_cast<std::int64_t>(x.size()));
    }
    SECTION("DC offset is measured and removed") {
        auto y = x;
        for (auto& v : y) v += 0.005f;
        const auto a = analyzeAudio(toDecoded(y));
        CHECK(a.dcOffset == Approx(0.005).margin(5e-4));
        CHECK(a.qualityClass == QualityClass::Good);
        CHECK(a.pcmSha256.size() == 64);
    }
    SECTION("mostly silence is rejected on speech ratio") {
        std::vector<float> y(x.size(), 0.0f);
        std::copy(x.begin(), x.begin() + 96000, y.begin());  // 2 s of speech in 35 s
        const auto a = analyzeAudio(toDecoded(y));
        CHECK(a.hasReason("reject.speech_ratio"));
    }
    SECTION("too short is rejected") {
        const std::vector<float> y(x.begin(), x.begin() + 48000 * 6);
        const auto a = analyzeAudio(toDecoded(y));
        CHECK(a.hasReason("reject.duration"));
    }
}

TEST_CASE("analyzer: channel normalisation", "[ingest][pipeline][channels]") {
    const auto& x = goodSpeech();
    SECTION("correlated stereo is averaged") {
        std::vector<float> r(x.size());
        for (std::size_t i = 0; i < x.size(); ++i) r[i] = 0.9f * x[i];
        const auto a = analyzeAudio(toStereo(x, r));
        CHECK_FALSE(a.multichannelSelected);
        CHECK(a.sourceChannels == 2);
        CHECK(a.aslDb == Approx(goodAnalysis().aslDb + 20.0 * std::log10(0.95)).margin(0.3));
    }
    SECTION("uncorrelated stereo selects the better-SNR channel and flags it") {
        const auto other = addWhiteNoise(speechLike(35.0, 99), -50.0, 3);  // different talker, noisier
        const auto a = analyzeAudio(toStereo(x, other));
        CHECK(a.multichannelSelected);
        CHECK(a.hasReason("multichannelSelected"));
        const auto mono = analyzeAudio(toDecoded(x));
        CHECK(a.pcmSha256 == mono.pcmSha256);  // channel 0 selected unchanged
        CHECK(a.qualityScore == Approx(mono.qualityScore - 5.0).margin(0.5));
    }
    SECTION("the noisier channel first: channel 1 is selected") {
        const auto noisy = addWhiteNoise(x, -50.0, 5);
        const auto other = speechLike(35.0, 99);
        const auto a = analyzeAudio(toStereo(noisy, other));
        CHECK(a.multichannelSelected);
        CHECK(a.pcmSha256 == analyzeAudio(toDecoded(other)).pcmSha256);
    }
}

TEST_CASE("analyzer: energy/flatness fallback VAD", "[ingest][pipeline][vad]") {
    AnalyzerConfig cfg;
    cfg.vad = VadEngine::EnergyFlatness;
    cfg.computeFingerprint = false;
    const auto a = analyzeAudio(toDecoded(goodSpeech()), cfg);
    CHECK(a.vadEngine == "energy-flatness");
    CHECK(a.speechRatio > 0.4);
    CHECK(a.speechRatio < 1.0);
    CHECK(a.fingerprint.empty());
    // Both engines agree roughly on the amount of speech.
    CHECK(a.speechS == Approx(goodAnalysis().speechS).epsilon(0.25));
}

TEST_CASE("duplicates: exact hash and near-duplicate fingerprints", "[ingest][pipeline][duplicates]") {
    const auto& x = goodSpeech();
    const auto& orig = goodAnalysis();
    SECTION("identical decoded audio has the identical PCM hash; other audio does not") {
        CHECK(analyzeAudio(toDecoded(x)).pcmSha256 == orig.pcmSha256);
        CHECK(analyzeAudio(toDecoded(speechLike(35.0, 12))).pcmSha256 != orig.pcmSha256);
    }
    SECTION("re-encoded (gain, 16-bit requantisation, 12 kHz low-pass) and trimmed copies are near duplicates") {
        std::vector<float> re = lowpass(x, 12000.0);
        for (auto& v : re) v = std::round(v * 0.5f * 32768.0f) / 32768.0f;  // -6 dB, 16-bit
        const auto a = analyzeAudio(toDecoded(re));
        CHECK(a.pcmSha256 != orig.pcmSha256);
        FingerprintIndex idx;
        idx.add(0, orig.fingerprint);
        std::uint32_t other = 99;
        const auto m = idx.query(a.fingerprint, &other);
        REQUIRE(m.found);
        CHECK(other == 0);
        CHECK(m.ber < kNearDupBer);
        CHECK(m.offset == 0);
        CHECK(m.overlap >= kNearDupMinFrames);

        // Trim the first 6 s of the re-encoded copy.
        const std::vector<float> trimmed(re.begin() + 48000 * 6, re.end());
        const auto t = analyzeAudio(toDecoded(trimmed));
        std::uint32_t other2 = 99;
        const auto m2 = idx.query(t.fingerprint, &other2);
        REQUIRE(m2.found);
        CHECK(m2.offset == Approx(6.0 / (64.0 / 5512.5)).margin(3.0));
        CHECK(m2.frames > 0);
    }
    SECTION("different speech is not a duplicate") {
        const auto b = analyzeAudio(toDecoded(speechLike(35.0, 12)));
        FingerprintIndex idx;
        idx.add(0, orig.fingerprint);
        std::uint32_t o = 0;
        CHECK_FALSE(idx.query(b.fingerprint, &o).found);
    }
}

TEST_CASE("speaking rate is measured for syllabic modulation", "[ingest][pipeline][rate]") {
    // 4 Hz amplitude-modulated harmonic tone -> ~4 nuclei per second.
    const double fs = 16000.0;
    std::vector<float> x(static_cast<std::size_t>(fs * 10));
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        const double env = 0.5 - 0.5 * std::cos(2.0 * M_PI * 4.0 * t);
        double v = 0;
        for (int h = 1; h <= 5; ++h) v += std::sin(2.0 * M_PI * 130.0 * h * t) / h;
        x[i] = static_cast<float>(0.4 * (0.05 + env) * v);
    }
    const std::vector<std::uint8_t> mask(x.size() / 160, 1);
    const auto f0 = yinF0(x, mask);
    const double rate = speakingRate(x, mask, f0.track);
    CHECK(rate == Approx(4.0).margin(0.8));
}

namespace {
// Sustained chords (C major / A minor / F major, 2 s each), four harmonics per note.
std::vector<float> chordMusic(double seconds, double fs = 48000.0) {
    static const double chords[3][3] = {{261.63, 329.63, 392.0}, {220.0, 261.63, 329.63}, {174.61, 220.0, 261.63}};
    const auto n = static_cast<std::size_t>(seconds * fs);
    std::vector<float> x(n, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / fs;
        const auto& c = chords[static_cast<std::size_t>(t / 2.0) % 3];
        double v = 0.0;
        for (double f : c)
            for (int h = 1; h <= 4; ++h) v += std::sin(2.0 * M_PI * f * h * t) / h;
        x[i] = static_cast<float>(0.05 * v);
    }
    return x;
}
}  // namespace

TEST_CASE("non-speech detection: sustained chords are flagged for review, speech is not", "[ingest][pipeline][nonspeech]") {
    SECTION("fraction") {
        const auto m48 = chordMusic(30.0);
        const auto music16 = resample(m48.data(), m48.size(), 48000.0, 16000.0);
        CHECK(nonSpeechFraction(music16) > 0.5);
        const auto& sp = goodSpeech();
        const auto speech16 = resample(sp.data(), sp.size(), 48000.0, 16000.0);
        CHECK(nonSpeechFraction(speech16) < 0.10);
    }
    SECTION("analyzer: review.nonSpeech reason, never a rejection reason of its own") {
        const auto m = analyzeAudio(toDecoded(chordMusic(35.0)));
        CHECK(m.nonSpeechFrac > 0.10);
        CHECK(m.hasReason("review.nonSpeech"));
        CHECK_FALSE(goodAnalysis().hasReason("review.nonSpeech"));
        CHECK(goodAnalysis().nonSpeechFrac < 0.10);
    }
    SECTION("speech with a little music stays under the threshold") {
        auto x = goodSpeech();
        const auto mus = chordMusic(2.0);
        for (std::size_t i = 0; i < mus.size() && i < x.size(); ++i) x[i] += mus[i];
        const auto a = analyzeAudio(toDecoded(x));
        CHECK_FALSE(a.hasReason("review.nonSpeech"));
    }
}
