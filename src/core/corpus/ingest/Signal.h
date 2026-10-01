#pragma once
// Signal-conditioning stages: resample (§3.2), DC removal (§3.4), VAD + post-processing (§3.5).
#include <cstdint>
#include <string>
#include <vector>

#include "core/corpus/CorpusSnapshot.h"
#include "core/corpus/ingest/IngestTypes.h"

namespace bf::ingest {

// r8brain-free-src, 24-bit quality (180 dB stopband), transition band 10 % of Nyquist
// (starts at 0.45 fs of the lower rate). Same rate: plain copy. Output length is
// round(n * dst / src); r8brain compensates its own latency.
std::vector<float> resample(const float* x, std::size_t n, double srcRate, double dstRate);

// 2nd-order Butterworth high-pass, forward-backward (zero phase). In place.
void removeDcZeroPhase(std::vector<float>& x, double fs, double cutoffHz = 20.0);

std::vector<std::int16_t> toPcm16(const std::vector<float>& x);

// VAD post-processing rules (§3.5, common) on a 10 ms flag sequence. Returns speech regions
// in 10 ms frame units [start, end):
//   hangover +150 ms after / +50 ms before each run, merge gaps < 80 ms,
//   drop regions < 200 ms isolated by >= 500 ms pauses on both sides.
std::vector<SampleSpan> postProcessVadFlags(const std::vector<std::uint8_t>& flags10ms);

// Raw 10 ms speech flags from a 16 kHz signal (WebRTC VAD, 30 ms frames) or from a 48 kHz
// signal (energy + spectral-flatness fallback).
std::vector<std::uint8_t> vadFlagsWebRtc(const std::vector<std::int16_t>& pcm16k, int mode, bool* ok = nullptr);
std::vector<std::uint8_t> vadFlagsEnergy(const std::vector<float>& audio48k);

// Speech regions in 48 kHz samples for the whole pipeline; `pcm16k` is the 16 kHz version.
std::vector<SampleSpan> detectSpeech(const std::vector<float>& audio48k, const std::vector<std::int16_t>& pcm16k,
                                     std::int64_t length48k, const AnalyzerConfig& cfg, std::string* engineName);

}  // namespace bf::ingest
