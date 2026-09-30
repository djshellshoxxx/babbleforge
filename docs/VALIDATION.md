# BabbleForge V1 — Testing and Validation

Covers brief sections 57, 58, 59, 60, 61 (data collection). Labels are defined in `ENGINE.md` §0.1. The measurable release criteria are in `ENGINE.md` §12.

---

## 1. Test strategy overview

| Layer | Tooling | Runs |
|---|---|---|
| Unit tests | Catch2 (C++), deterministic, < 2 min total | Every commit (CI: Windows MSVC, Linux clang) |
| RT-safety tests | Sanitizer builds (`RT_CHECK`, RealtimeSanitizer) | Every commit (Linux), nightly (Windows) |
| Statistical tests | `bfrender` + `bfanalyze` long renders, fixed seeds | Nightly (1 h renders), weekly (8 h) |
| Golden/regression | Hash-locked fixtures (D1 event lists, D2 WAV hashes per platform) | Every commit (short fixtures); nightly (full) |
| Stress/soak | Real-time host on benchmark machines with fault injection | Weekly (24 h), pre-release (7 days) |
| Scientific validation | Fixture set + Python analysis (`tools/validation/`) | Per release candidate |
| Human validation | Listening experiments (§6) | Before V1.0 |
| Machine validation | ASR/enhancement/separation pipeline (§7) | Before V1.0, then per major DSP change |

---

## 2. Offline tools (Test Harness)

### 2.1 `bfrender` — deterministic renderer

```text
bfrender --scenario scenario.json --corpus <root> --out out.wav [--events events.json]
         [--data-dir <dir>] [--threads-sync] [--no-limiter] [--taps T1,T2,T3]
bfrender --matrix experiment.json --out-dir <dir>
```

- Runs the same engine library as the app, with synchronous preloading and the analysis thread stepped synchronously.
- Outputs:
  - the WAV (float32, N channels)
  - optional tap WAVs
  - `*.events.json` (the D1 timeline)
  - `*.sidecar.json`: scenario, seed, corpus version, data-set hash, app version, and all metrics from `bfanalyze`
  - SHA-256 of the audio data

### 2.2 `bfanalyze` — offline analyzer

The same metric definitions as the real-time analyzers, but computed exactly over whole files:
- RMS, LUFS-S/I, true peak, crest factor
- 1/3-octave and octave levels (with an IEC 61260 class 1 filterbank reference)
- LTASS deviation
- envelope statistics, gap histogram, modulation spectrum
- channel correlation matrix
- talker statistics (from `events.json` + VAD masks)

Output is JSON + CSV. Analysis code is shared with the engine where possible, with an independent Python re-implementation in `tools/validation/` for cross-checking.

### 2.3 Experiment matrix format

Modelled after the configuration-driven synthesis of the Microsoft DNS Challenge.

```json
{
  "schema": "babbleforge.matrix/1",
  "base": "scenarios/base_lab.json",
  "axes": {
    "laboratory.talkers": [1, 2, 4, 8, 16],
    "preset.macros.mix.babbleFraction": [0.0, 0.5, 1.0],
    "preset.spectrum.target": ["ltass_universal", "slope_-5"],
    "seed": [1001, 1002, 1003]
  },
  "durationS": 300,
  "targetSpeech": { "list": "targets/sentences.csv", "snrDb": [6, 3, 0, -3, -6] }
}
```

When `targetSpeech` is given, the renderer also produces target+masker mixtures at the specified speech-to-masker ratios. Ratios are computed as the target's P.56 ASL vs the masker's RMS over the target's duration, both digital, as in the research paradigm.

---

## 3. Unit tests (minimum set)

| Area | Tests |
|---|---|
| PRNG | Golden vectors: SplitMix64 and xoshiro256** first 1000 outputs for 3 seeds; stream-derivation vectors; `detmath` functions vs high-precision references (≤ 1 ulp) and identical across platforms (vector file) |
| Distributions | Moments (mean, variance) of exponential, log-normal, truncated Gaussian within 3 standard errors for 10⁶ draws; unbiased integer ranges (χ², p > 0.001) |
| Talker scheduler | Min/max never violated in 10⁴ s planned; anti-synchrony spacing ≥ 40 ms; forced-start logic; epoch re-plan keeps frozen events; MultiVoice K constant; re-entry cooldown respected |
| Segment selector | No active speaker selected; recent-speaker rule; persistent shuffle covers all anchors before reuse; cycle-boundary constraint; cooldown skipping; relaxation order; state persistence round-trip |
| Seed determinism | Same seed → identical event JSON; different seed → different; block sizes 64/256/480/1024 → identical WAV hash |
| Energy normalization | Babble count normalization: synthetic unit-power talkers → bus power within ±0.2 dB for m = 1…32; stationary FIR unit energy (\|Σh² − 1\| < 1e-6) |
| Hybrid mixing | Power constancy for b ∈ {0, 0.25, 0.5, 0.75, 1} with independent unit noises (±0.05 dB over 60 s); ramp constancy during a 2 s ramp (every 50 ms window within ±0.1 dB); linear-law regression guard (a test fails if the gains follow a linear crossfade) |
| Spectrum filters | Designed response vs target within ±0.5 dB (100 Hz–10 kHz) for all built-in targets at all 4 sample rates; min-phase property (all zeros inside the unit circle, via cepstral check); LF limit response (125 Hz attenuation < 1 dB at an 80 Hz limit) |
| Convolver | Equals direct convolution within 1e-6 relative; kernel crossfade glitch-free (no sample step > −80 dB of RMS) |
| Correction loop | Converges from ±3 dB synthetic offsets; respects slew/clamp/deadband; freeze conditions; oscillation guard triggers on a synthetic oscillating input |
| Limiter | True-peak detector vs BS.1770 test vectors; ceiling respected for adversarial inputs (full-scale square, inter-sample-peak signals); zero GR below threshold; release timing |
| Meters | EBU Tech 3341 (M/S/I) test signals within tolerance; true-peak test signals |
| Preset parser | Schema-valid examples load; unknown fields round-trip; `x-` preserved; higher minor → warning; higher major → reject; clamping records; migration v0.9 → v1.0 fixtures |
| State transitions | All legal transitions succeed; every illegal pair rejected (exhaustive table); DEVICE_LOST never selects another device |
| Output matrix | Gain/mute/polarity/delay correctness (sample-exact delay); zone gain application; device channel mapping incl. sparse maps; ramp lengths |
| Spatial | Stereo far-channel floor; MDAP second-gain floor; power normalization Σg² = 1; neighborhood construction on grids; zone confinement |
| Decorrelator | Flat magnitude (±0.01 dB); −60 dB decay within 25 ms; distinct primes per channel/stage |
| Corpus | VAD post-processing rules; P.56 ASL vs the ITU reference implementation (±0.1 dB); YIN on synthetic harmonic signals (±1 %); clipping detector; duplicate detection on re-encoded fixtures |

---

## 4. Statistical tests (long renders)

Run each on 1 h renders (nightly) and 8 h (weekly), for 3 seeds, for these configurations: Office/Balanced, Open Office/Dense (4 ch), Common Area/Natural, MultiVoice 7, Large Room distributed (16 ch).

| Metric | Pass criterion |
|---|---|
| Mean talker count | \|mean k_a − m\| ≤ 5 % of m; min/max never violated (0 starvation offline) |
| Activity density | Occupancy (k_s ≥ 1) ≥ 97 % for Balanced/Dense with m ≥ 5; Natural reported (no pass bar, trend only) |
| Gap distribution | Median and p95 gap decrease monotonically Natural → Balanced → Dense (A-TM-2); gap histogram stored as a regression artefact; KS-test vs the previous release's histogram (flag if D > 0.1) |
| Talker reuse rate | No region reuse inside T_seg,eff; speaker reuse interval distribution: 5th percentile ≥ R_spk selections |
| Output RMS | 60 s RMS stable within ±0.5 dB (babble) / ±0.1 dB (stationary); over 1 h, no 10 s window outside ±3 dB |
| Spectral error | Octave ±1 dB, 1/3-oct ±2 dB (babble after 5 min), ±1 dB (stationary) |
| Channel correlation | Per `ENGINE.md` A-SPA-1/2/5 |
| Solo exposure | CVR High, m ≥ 4: < 1 % (verification of the hypothesized effect; a failure triggers review, not an automatic release block) |
| Level-variation distribution | Per-event gain offsets: SD within ±10 % of σ; none beyond ±2σ |

---

## 5. Scientific validation fixtures (offline)

Generated for every release candidate with the pinned QA corpus and seeds 1001–1005.

| Fixture | Configuration | Duration | Matched properties |
|---|---|---|---|
| B1, B2, B4, B8, B16 | LaboratoryMask `continuousN` N = 1, 2, 4, 8, 16; maxGap 100 ms; nested speaker sets | 300 s | Identical RMS (two-pass, ±0.01 dB), identical target LTASS (strict), identical duration |
| B3, B5, B7 | Same for 3, 5, 7 (multi-voice series) | 300 s | Same |
| SSN-L | Stationary, Universal LTASS | 300 s | RMS |
| SSN-M | Stationary, speech-matched profile of the QA target-speech corpus | 300 s | RMS |
| SL5, SL7, SL9 | Stationary −5/−7/−9 dB/oct | 300 s | RMS |
| PINK | Pink reference | 300 s | RMS |
| H25, H50, H75 | Hybrid with 8-talker babble, b = 0.25/0.5/0.75 | 300 s | RMS, LTASS |
| NAT / BAL / DEN | Office area stochastic Natural/Balanced/Dense | 600 s | RMS |

Measured and archived for each:
- LTASS (1/3-oct, speech-weighted) and deviation from target
- 1/3-octave and octave levels
- envelope modulation: m(f) 0.5–16 Hz broadband and per octave; L10 − L90
- gap distribution (histogram, median, p95, max)
- crest factor (true peak − RMS)
- for multichannel fixtures: the correlation matrix

**Expected trends (checked, since they are research-grounded):**
- envelope modulation decreases monotonically with N [R: Rosen et al.]
- SSN modulation is at the noise floor
- LTASS deviation ≤ 1 dB (octave) across all fixtures, so the spectrum is not a confound

A failure indicates an engine bug or a corpus problem, not a scientific finding.

---

## 6. Human validation (before V1.0)

### 6.1 Principles

- Pre-registered protocol (hypotheses, primary outcomes, analysis) before data collection.
- The masker conditions are the fixtures of §5, played at matched **acoustic** level. The level is set with a calibrated sound-level meter at the listening position; this is done outside BabbleForge in V1, and the SPL is noted in the study record, not in the app.
- Target speech: standardized sentence material in the participants' language (e.g. HINT/IEEE/Matrix-type sentences, used under their licenses) and serial-recall digit lists.
- Ethics approval and informed consent as required by the host institution.

### 6.2 Conditions

| # | Condition |
|---|---|
| C0 | No masker (quiet, background ≤ 30 dBA noted) |
| C1 | Pink noise |
| C2 | Speech-shaped noise (LTASS) |
| C3 | Speech-shaped noise (speech-matched) |
| C4 | 4-talker babble |
| C5 | 7-talker (multi-voice) |
| C6 | 8-talker babble |
| C7 | 16-talker babble |
| C8 | Hybrid (8-talker, b = 0.5) |
| C9 | BabbleForge default (Office/Balanced) |

### 6.3 Outcomes

| Outcome | Measure | Notes |
|---|---|---|
| Word recognition | % words correct at fixed speech-to-masker ratios (−6, −3, 0, +3 dB) | Psychometric-function fit per condition; SRT50 |
| Sentence recognition | Adaptive SRT (1-up/1-down, 50 %) | — |
| Serial recall (irrelevant-speech paradigm) | Error rate with the target speech as a distractor at fixed ratios | The office-relevant cognitive outcome [R: Renz; Keus van de Poll; Jahncke] |
| Subjective distraction | 0–10 scale | — |
| Annoyance | ISO/TS 15666 5-point verbal and 11-point numeric scales | — |
| Naturalness / acceptability | 0–10 | For Natural/Common-Area configurations |

- **Design:** within-subject, counterbalanced (Latin square), ≥ 24 participants for recognition (power analysis to be finalized in pre-registration), and ≥ 30 for recall.
- **Reporting:** effect sizes with confidence intervals. Results **update the data files** (`PRESETS.md` §8); they do not change code.
- **No outcome is encoded as "optimal"** without replication across tasks.

### 6.4 Spatial listening study (V1.x)

Stereo vs 4-channel distributed, Speaker Variation Low/Medium/High, with a loudspeaker target at a different location. It measures recognition and localization of the masker ("where does the masking come from?").

---

## 7. Machine validation

Reported **separately** from human results. It makes **no claim** that recordings are unrecoverable.

| Pipeline | Example open tools (evaluation only) | Metric |
|---|---|---|
| ASR on target+masker mixtures | Whisper-family and one conventional (e.g. Kaldi/Vosk) recognizer | WER vs speech-to-masker ratio, per masker condition |
| Speech enhancement → ASR | A DNS-Challenge-type enhancement model | ΔWER (enhanced − unenhanced), PESQ/ESTOI changes (objective, informational) |
| Source separation → ASR | Conv-TasNet/SepFormer-type separator (e.g. via SpeechBrain) | WER of the best separated stream |
| Dereverberation → ASR | WPE (nara_wpe) | ΔWER |

- **Conditions:** every masker in §6.2, separately for stationary, multi-talker and hybrid [R note: enhancement models are often trained on stationary-like noise, and multi-talker interference is a different separation problem, so results may reverse relative to human results].
- **Room simulation:** optional Pyroomacoustics rooms (4×5×2.7 m RT60 0.3 s; 8×12×3 m RT60 0.6 s; open office) with target and masker loudspeaker geometry.
- **Report template:** a table of WER by condition × ratio × pipeline, with the software versions, model versions and seeds.

---

## 8. Stress tests

| Test | Setup | Pass |
|---|---|---|
| 24 h continuous | Mid machine, 8 ch, Open Office/Balanced | A-RT-1/2, level/spectrum criteria sustained, 0 ERROR |
| 7-day soak | Low machine, stereo, Office/Balanced | A-RT-3 |
| Device restart | Scripted: disable/enable the device every 10 min for 2 h | Correct DEVICE_LOST → PREPARING → RUNNING cycles (autoReconnect on); never plays on another device (verified by recording a second device's loopback: silence) |
| Sample-rate change | Toggle 48/44.1/96 kHz every 15 min for 2 h | A-RT-5 each time |
| Missing files | Delete 20 % of cache files during a run | A-RT-6; DEGRADED correctly raised/cleared; policy behaviors (Strict/Safe/Continuous) each verified |
| Corrupt files | Flip random bytes in 10 % of FLAC files | Same |
| Slow disk | Throttle the I/O layer (test hook) to a 300 ms latency and 1 MB/s bandwidth | No audible dropout at Office defaults (starvations = 0); DEGRADED when throttled to 100 KB/s |
| High CPU load | 100 % load on all but one core (stress-ng / Prime95) at normal priority | 0 xruns at a 480-frame buffer |
| Memory pressure | Available RAM < 500 MB | No crash; pool minimum honored |
| Rapid control changes | Automated UI: random parameter changes at 10 Hz for 1 h | No RT violations, no level excursions beyond A-LV-4 limits |
| Long-uptime counters | Simulated 2^32 samples, sample counter wrap tests | 64-bit counters; no overflow in 10 years at 96 kHz |

---

## 9. Data collection for engine tuning

1. Every experiment render carries a sidecar with the complete effective parameters, the data-set hash and the seed, so results link to exact configurations.
2. A tuning workflow (`tools/tuning/`, Python) reads the experiment results (CSV) and produces updated data files (`areas/*.json`, macro anchors) as a pull request with a diff report. Recompilation is never needed.
3. Area/strategy data files carry `evidenceLabel` and `evidenceRefs`. When validated, the label changes from `engineering-default-requires-validation` to `validated:<study-id>`.
4. Validation results are stored in `docs/research/validation/<study-id>/` (protocol, anonymized data, analysis scripts, report).
