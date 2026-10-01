# BabbleForge V1 — Engine Specification (Master Document)

Document set: V1 Engine Specification, revision 1.0-draft
Status: implementation-ready design, pre-code
Scope: audio engine and supporting subsystems. The GUI is specified separately in `GUI.md`.
Audience: C++/JUCE developers, DSP engineers, QA.

---

## 0. How to read this document set

| Document | Contents |
|---|---|
| `ENGINE.md` (this file) | Conventions, evidence labels, top-level architecture, signal flow, gain staging, output level management, limiter, meters, CPU budget, default configuration, open research questions, implementation priorities, acceptance criteria |
| `MASK_STRATEGIES.md` | Strategy architecture, the six strategy classes, hybrid mixer, activity density model, Natural↔Dense macro, Clear Voice Reduction, Voice Amount macro, laboratory mode |
| `TALKER_ENGINE.md` | Virtual talker model, state machine, planner/scheduler algorithm and distributions, talker-count modes, segment selection, voice diversity, source preloading |
| `SPECTRUM_ENGINE.md` | Spectrum targets, filter design, stationary speech-shaped masker, speech-matched profiles, measurement, slow spectral correction, modulation/temporal analysis |
| `SPATIAL_ENGINE.md` | Layouts, stereo/small/large algorithms, decorrelation, zones, output matrix |
| `CORPUS.md` | Corpus requirements, ingestion pipeline, VAD, quality checks, metadata schema, cache format |
| `PRESETS.md` | Area × Strategy model, area defaults, preset file format, data-driven tuning, first-launch default |
| `REALTIME_ARCHITECTURE.md` | Real-time rules, threads, messaging, buffers, memory, determinism and PRNG |
| `RELIABILITY.md` | Engine state machine, failure handling, fallback policies, logging, diagnostics, telemetry, privacy |
| `VALIDATION.md` | Unit, statistical, stress tests; scientific, human and machine validation |
| `V2_EXTENSION_POINTS.md` | Reserved interfaces, calibration signal bus, V2 integration plan |

Section numbers of the original engine brief are mapped in each document's header.

### 0.1 Evidence labels

Every non-trivial numeric value or design decision in this document set carries one of these labels (inline or in a table column):

| Label | Meaning |
|---|---|
| **[R]** Research-grounded | Behavior directly follows from a cited, established result. The principle is named. |
| **[S]** Standard-derived | Taken from a published standard (ITU-R BS.1770, IEC 60268-16, ANSI S3.5, ITU-T P.56, IEC 61260). |
| **[E] Engineering default — requires validation** | A reasoned starting value. It is not an experimentally established optimum and must be tuned from data (`VALIDATION.md`). |
| **[I]** Implementation choice | A value set for software reasons (latency, CPU, memory). It has no perceptual claim. |

No preset, strategy or parameter value in this document is described as "optimal". Where a value is labelled [E], implementers must load it from data files (`PRESETS.md` §8) and must not hard-code it.

### 0.2 Units and conventions

| Quantity | Convention |
|---|---|
| Digital RMS level | dBFS RMS = 20·log10(x_rms). A full-scale square wave is 0 dBFS RMS; a full-scale sine is −3.01 dBFS RMS. |
| Sample peak | dBFS = 20·log10(max\|x\|) |
| True peak | dBTP per ITU-R BS.1770-5 Annex 2 (oversampled peak). |
| Loudness | LUFS per ITU-R BS.1770-5; LUFS-M (400 ms), LUFS-S (3 s), LUFS-I (gated, integrated). |
| Energy fraction | Fraction of mean-square power (not amplitude). A 25 % fraction means a quarter of the long-term power. |
| Band levels | dB re an arbitrary digital reference, relative (shape) unless stated otherwise. |
| Octave bands | IEC 61260 base-10 nominal centres: 125, 250, 500, 1000, 2000, 4000, 8000 Hz ("the seven STI bands"). |
| 1/3-octave bands | IEC 61260 nominal centres 50 Hz … 16 kHz (26 bands). Operating range 100 Hz … 10 kHz (21 bands). |
| Time | Engine time is a 64-bit sample counter at the engine rate. Wall-clock is used only for logs. |
| Gain | Linear gains are `float`. dB values in configuration are converted once on a non-RT thread. |

**Digital level is not SPL.** No V1 value, label, log entry or export may state or imply an acoustic sound-pressure level (dB SPL, dBA). V1 reports digital quantities only. The mapping from digital level to SPL is the job of the V2 `ILevelCalibration` interface (`V2_EXTENSION_POINTS.md`).

---

## 1. Primary objective and design principles

The V1 engine generates, switches and controls these maskers at a predictable aggregate digital output level:

1. stationary speech-shaped noise (SSN)
2. multi-talker babble
3. low-count multi-voice masking
4. high-density babble
5. hybrid babble + stationary
6. natural/common-area babble
7. controlled laboratory/reference maskers

Three generator families are first-class and equal in importance: **stationary speech-shaped masking**, **multi-talker babble**, and **hybrid combinations** [R: Renz, Leistner & Liebl 2018 found spectrally matched stationary noise gave similar recall error rates at a speech-to-noise ratio 3 dB higher than a −5 dB/oct masker; Rosen et al. 2013 and Keus van de Poll et al. 2015 show talker count changes masking in non-monotonic, task-dependent ways].

### 1.1 Design rules (binding on all documents)

1. Prefer measured behavior over assumptions: every generator reports statistics it can be tested against.
2. Separate facts [R]/[S] from defaults [E].
3. Keep V1 independent of microphones and acoustic calibration.
4. Multichannel is the base case: the engine is N-channel internally, and mono and stereo are layouts of N = 1 and N = 2.
5. Stationary SSN has equal status with babble: it has its own engine, tests and presets.
6. Output is digitally safe: a true-peak limiter is always present and is not the loudness control.
7. Digital gain is never labelled as SPL.
8. No preset is described as universally optimal.
9. DSP stays simple: per-talker processing is gain, fade and pan only, and all spectral shaping is per output channel.
10. Speech is never manipulated for novelty: no pitch shifting, reversal, vocoding or time-stretching in production strategies.
11. Every stochastic process is reproducible from a seed.
12. The engine must run unattended for weeks.
13. V2 calibration must plug in without rewriting the masker (reserved stages exist from V1).

---

## 2. Top-level architecture

### 2.1 Subsystems

| # | Subsystem | Thread(s) | Responsibility | Spec |
|---|---|---|---|---|
| 1 | Audio Device Layer | driver/RT | JUCE `AudioDeviceManager` wrapper; device open/close, channel map, xrun detection | `REALTIME_ARCHITECTURE.md` §3 |
| 2 | Master Engine Controller | control | Engine state machine, command handling, graph (re)build, parameter distribution | `RELIABILITY.md` §1 |
| 3 | Mask Strategy Layer | control | Converts user/preset parameters into a `MaskRenderPlan` (see §2.3) | `MASK_STRATEGIES.md` |
| 4 | Talker Planner (Scheduler) | planner | Plans talker events ahead of real time using the seeded PRNG | `TALKER_ENGINE.md` §4 |
| 5 | Segment Selector | planner | Chooses corpus segments, enforces repetition cooldowns | `TALKER_ENGINE.md` §6 |
| 6 | Source Preloader | decode pool | Decodes, resamples, gap-shortens and fills talker ring buffers | `TALKER_ENGINE.md` §8 |
| 7 | Talker Engine (voice renderer) | RT | Plays planned events: gain, fades, pan into the N-channel babble bus | `TALKER_ENGINE.md` §3 |
| 8 | Stationary Mask Engine | RT | N independent seeded noise sources → speech-shaped FIR | `SPECTRUM_ENGINE.md` §4 |
| 9 | Spectrum Engine | RT (filters) + analysis | Target curves, FIR design, measurement, slow correction | `SPECTRUM_ENGINE.md` |
| 10 | Temporal/Modulation Analyzer | analysis | Occupancy, gaps, envelope statistics, modulation spectrum | `SPECTRUM_ENGINE.md` §8 |
| 11 | Spatial Renderer | RT + control | Per-talker gain vectors, decorrelators, motion | `SPATIAL_ENGINE.md` |
| 12 | Hybrid Mixer | RT | Energy-normalized babble/stationary mix per zone | `MASK_STRATEGIES.md` §4 |
| 13 | Output Matrix | RT | Zone gains, per-output gain, mute, delay, polarity, calibration EQ slot | `SPATIAL_ENGINE.md` §7 |
| 14 | Limiter | RT | True-peak safety limiter plus final safety clip | §6 |
| 15 | Meters | RT (accumulate) + analysis | RMS, peak, true peak, LUFS, crest factor, GR, correlation | §5 |
| 16 | Corpus Manager | corpus I/O | Metadata database, corpus versions, runtime segment index | `CORPUS.md` |
| 17 | Corpus Analyzer (ingestion) | ingestion pool | Offline import pipeline | `CORPUS.md` §3 |
| 18 | Preset Manager | control | Area × Strategy composition, factory/user presets, migration | `PRESETS.md` |
| 19 | Configuration Manager | control | App settings, last-known-good, autosave | `PRESETS.md` §9, `RELIABILITY.md` |
| 20 | Diagnostics | control | Snapshots, watchdog, health model | `RELIABILITY.md` §6 |
| 21 | Logging | logger | Asynchronous structured logging | `RELIABILITY.md` §5 |
| 22 | Test Harness / Offline Renderer | CLI | Headless deterministic rendering and analysis (`bfrender`, `bfanalyze`) | `VALIDATION.md` §2 |
| 23 | Calibration Signal Bus (reserved) | RT | Test and identification signals, V2 measurement stimuli | `V2_EXTENSION_POINTS.md` §3 |

### 2.2 Complete signal flow

The engine processes N output channels (1 ≤ N ≤ 32; 16 guaranteed in V1). All buses below the Talker Engine are N-channel.

```text
                       ┌───────────────────── NON-REAL-TIME ─────────────────────┐
  Corpus DB ──► Corpus Manager ──► Segment Selector ◄── Talker Planner (seeded)
  (SQLite +     (segment index)          │                  │ event timeline
   FLAC cache)                           ▼                  ▼ (lookahead 4 s)
                                   Source Preloader ──► per-slot ring buffers
                                   (decode, resample,        │ (block pool)
                                    gap-shorten, fade-safe)  │
                       └─────────────────────────────────────┼────────────────────┘
  ══════════════════════════ REAL-TIME AUDIO CALLBACK ═══════▼════════════════════
                                                             │
      Event consumer ──► Talker voices v=1..V (gain · fade · level-var)
                                     │
                                     ▼  per-talker gain vector (N) from Spatial Renderer
                        ┌── Babble Bus [N ch] (Σ talkers)
                        │        │  × g_bnorm (feed-forward count norm) × g_btrim (slow trim)
                        │        ▼
                        │  Babble Spectral Shaper  (min-phase FIR per ch;
                        │        │                  = Target/PoolLTASS × Correction)
                        │        ▼
                        │  Decorrelator (all-pass cascade per ch, Speaker Variation)
                        │        │                                      ┌─ tap T1 (babble)
  PRNG noise [N indep.] │        │
        │               │        │
        ▼               │        │
  Stationary Shaper ────┼────────┤     (min-phase FIR per ch = Target shape, unit energy)
  (FIR per ch)          │        │                                      ┌─ tap T2 (stationary)
                        │        ▼
                        │  Hybrid Mixer (per zone: √b · babble + √(1−b) · stationary)
                        │        │                                      ┌─ tap T3 (pre-master, spectrum/modulation)
                        │        ▼
                        │  Strategy crossfader (A/B, strategy change; equal-power)
                        │        ▼
                        │  Master Gain (Strength; smoothed)
                        │        ▼
   Calibration Bus ─────┴──► Source Select (MASKER | CALIBRATION | MUTE)  [reserved V2 path]
                                 ▼
                            Output Matrix: zone gain → output gain → mute → polarity
                                 │         → delay → Calibration-EQ slot (identity in V1)
                                 ▼
                            True-Peak Limiter (per zone-linked group)
                                 ▼
                            Safety clip ±1.0 (counts events)          ┌─ tap T4 (output meters)
                                 ▼
                            Device channel map ──► Audio Device (float32)
```

Analysis threads read taps T1–T4 through lock-free SPSC rings (`REALTIME_ARCHITECTURE.md` §5). They never write audio.

**Improvements over the concept flow in the brief:**
1. Spectral shaping moves from per-talker to per output channel. This is O(N) instead of O(V) and keeps per-talker DSP trivial.
2. Spectral correction is folded into the babble shaper. It corrects only the babble component, so the stationary masker stays exactly on target (`SPECTRUM_ENGINE.md` §6).
3. Decorrelation is applied after shaping and before mixing. The stationary component needs none, because its channels are generated independently.
4. Talker planning happens ahead of real time (a timeline of events) instead of on-the-fly in the callback. This makes preloading and determinism straightforward.
5. The calibration bus and calibration-EQ slot exist in V1 as identity or unused stages, so V2 inserts no new graph nodes.

### 2.3 Strategy = policy, not a DSP graph

All strategies drive **one fixed DSP graph**. A `MaskStrategy` is a non-RT policy object that turns (Area model, Strategy parameters, macros, corpus state) into an immutable `MaskRenderPlan`:

```cpp
struct MaskRenderPlan {                    // immutable, built on control thread
    uint64_t        planId;                // excludes the talker seed (revised during implementation)
    TalkerPlanParams talkers;              // pool, min/mean/max, durations, gaps, gain var, fades …
    StationaryParams stationary;           // enabled, spectrum id, seed stream
    SpectrumTarget   target;               // 1/3-oct target (shared by both components)
    MixParams        mix;                  // babble energy fraction per zone
    SpatialParams    spatial;              // layout, spread, motion, decorrelation level
    LevelParams      level;                // reference level, strength
    FallbackPolicy   fallback;
    StrategyId       strategyId;           // for reporting only
};
```

Consequences:
- Switching strategy never rebuilds the audio graph. It swaps plans and crossfades (`MASK_STRATEGIES.md` §7).
- Aggregate-level normalization is implemented once and applies to every strategy.
- New strategies are added as data plus policy, with no DSP changes.

---

## 3. Gain staging and level architecture

### 3.1 Reference level

| Symbol | Value | Label | Meaning |
|---|---|---|---|
| L_ref | −26.0 dBFS RMS | [I] (aligned with ITU-T P.56 nominal −26 dBov) | Long-term RMS of every generator component, per output channel, at Strength 0 dB |
| Headroom at L_ref | 25 dB to 0 dBFS; 25 dB to the −1 dBTP ceiling at Strength +0 dB is 24 dB | [I] | Covers babble envelope crest (≤ 20 dB for 1 talker) |
| Strength range (Simple) | −18 … +9 dB | [E] | GUI labels: Gentle −12, Low −6, Normal 0, Strong +5, Very Strong +9 |
| Strength range (Advanced) | −40 … +12 dB | [I] | Above +6 dB the UI shows "limiter may engage" |
| Strength smoothing | 1st-order, τ = 200 ms, applied per sample | [I] | No zipper noise |

Each component (babble bus after trim, stationary bus) is normalized independently to L_ref per channel. The hybrid mixer is energy-preserving (§4 of `MASK_STRATEGIES.md`), so the pre-master mix is at L_ref for every strategy and every mix ratio. Output RMS per channel = L_ref + Strength + zone gain + output gain.

### 3.2 Babble level normalization (two-stage)

1. **Per-segment normalization [R/S]** — every corpus segment carries its active speech level (ITU-T P.56 method B) from ingestion. Talker gain g_seg = 10^((L_ref − ASL_seg)/20). Every talker, while speaking, is at the same active level, following Rosen et al.'s practice of RMS-normalizing individual talkers before mixing [R].
2. **Feed-forward count normalization** — the babble bus is scaled by g_bnorm = 1/√(E[k_speech]), where E[k_speech] = planned mean simultaneous *speaking* talkers measured from an 1800 s probe run of the planner (includes level-variation and fade losses). Independent talkers add in power, so this puts the expected bus power at L_ref (revised during implementation).
3. **Slow feedback trim g_btrim** — the analysis thread measures babble tap T1 power over a 10 s window. It updates a trim integrator with τ = 30 s, clamped to ±6 dB and slew-limited to 0.5 dB/s. This removes residual bias (for example, segment speech-fraction estimation error). It is **not** an AGC: its time constant exceeds speech modulation (0.5–16 Hz) by more than 2 orders of magnitude, so it cannot flatten the envelope [R: preserving the envelope preserves the temporal structure studied in the literature]. Slow trim is required (feed-forward alone gave +0.5 dB windows) (revised during implementation).
   - The trim freezes while fewer than 1 talker is planned, during strategy crossfades, and for 10 s after any plan change.
   - At a plan change, the trim resets to the stored per-plan value (0 dB if none).

The target accuracy is a long-term (60 s) babble RMS within ±0.5 dB of L_ref, and short-term (3 s) within ±3 dB for mean-active ≥ 4 (see acceptance criteria §12).

### 3.3 Stationary level normalization

The stationary FIR is designed with unit energy (Σh² = 1) and the source noise has unit variance. The output variance is therefore exactly 1 before gain, and the gain to L_ref is analytic: g_s = 10^(L_ref/20). No feedback is needed. The 60 s RMS error must be within ±0.1 dB (a statistical property of the noise).

---

## 4. Audio engine requirements

| Item | Requirement | Label |
|---|---|---|
| Sample rates | 44 100, 48 000, 88 200, 96 000 Hz | brief |
| Default rate | 48 000 Hz | [I] |
| Internal format | 32-bit IEEE float for audio; 64-bit double for integrators, level accumulators, filter design and analysis | [I] |
| Channel counts | 1, 2, 4, 6, 8, custom 1–16 guaranteed; up to 32 supported if device and CPU allow | brief |
| Device APIs (Windows) | WASAPI shared and exclusive, ASIO (Steinberg SDK licensing is a build-time option), DirectSound excluded | [I] |
| Future | CoreAudio, ALSA/PipeWire via JUCE | [I] |
| Buffer sizes | 64–4096 frames accepted; default 480 frames at 48 kHz (10 ms) for WASAPI shared, 256 for ASIO | [I] (latency is irrelevant for a masker; larger buffers improve robustness) |
| Internal block | The engine processes in sub-blocks of at most 256 frames regardless of device buffer; events are sample-accurate within a sub-block | [I] |
| Block-size independence | Output must be bit-identical for any device buffer size (required for determinism) | [I] |
| Latency | Not a requirement. Whole-chain latency is reported for diagnostics only | — |
| Continuous operation | ≥ 30 days without restart, no unbounded growth in memory or handles | [I] |
| Denormals | FTZ/DAZ enabled on the RT thread (`juce::ScopedNoDenormals`), and noise-dither at −200 dBFS not required | [I] |
| Headroom | See §3.1. The whole pre-limiter chain is float and cannot clip internally | [I] |

Real-time rules, thread model, lock-free messaging, file decoding and underrun recovery are in `REALTIME_ARCHITECTURE.md`.

---

## 5. Output level management and meters

V1 does not know acoustic level. It exposes digital measurements only, computed on tap T4 (post-limiter), and T3 where noted.

| Meter | Definition | Update | Usefulness for a continuous masker | Shown in |
|---|---|---|---|---|
| RMS-fast | 300 ms rectangular, per channel | 20 Hz | Visual activity only | Simple bar (LOW/GOOD/HIGH), Advanced |
| RMS-Leq60 | 60 s rectangular, per channel and energy-mean over channels | 1 Hz | **Primary level metric**: the masker's stable operating level | Advanced |
| LUFS-M / LUFS-S | BS.1770-5, all channel weights 1.0 (see note) | 10 Hz | LUFS-S is useful as a perceptual loudness proxy and stable for maskers | Advanced |
| LUFS-I | BS.1770-5 gated integrated since Start | 1 Hz | Limited, because a continuous masker has no programme gating structure. Kept for session comparisons | Advanced (details) |
| Sample peak | max\|x\| per channel, 1 s hold | 20 Hz | Low | Diagnostics |
| True peak | BS.1770-5 4× oversampled (2× at 88.2/96 kHz), max over 1 s and session max | 10 Hz | Headroom verification | Advanced |
| Crest factor | True peak (dBTP, 10 s window) − RMS (dBFS, same 10 s) | 1 Hz | Headroom planning; indicates masker type (SSN ≈ 11–13 dB, 2-talker ≈ 18–22 dB) | Advanced |
| Limiter GR | Instantaneous and 60 s maximum and 60 s time-above-0.5 dB (%) | 20 Hz / 1 Hz | Detects misuse of the limiter as loudness control | Advanced, status bar |
| Clip count | Safety-clip events since start | event | Must stay 0 | Diagnostics |
| Channel correlation | Pearson ρ between channel pairs, 10 s windows (`SPATIAL_ENGINE.md` §5) | 0.2 Hz | Spatial diffuseness | Analysis |

Note on LUFS: BS.1770 channel weights are defined for standard loudspeaker layouts. BabbleForge applies weight 1.0 to every output and labels the value "LUFS (unweighted multichannel)" in Advanced and Diagnostics when N > 2.

**Simple-mode LOW/GOOD/HIGH mapping [E]:** computed on RMS-Leq60 relative to L_ref:
- LOW < −12 dB
- GOOD −12 … +6 dB
- HIGH > +6 dB, or the limiter "sustained" flag is set

This is a digital-headroom indicator, not a loudness or SPL judgement.

---

## 6. Limiter

A transparent, always-present true-peak safety limiter. It is not part of normal loudness control: at Strength ≤ +6 dB with any factory preset, gain reduction must be 0 dB for ≥ 99.9 % of the time (acceptance test).

| Parameter | Value | Label |
|---|---|---|
| Ceiling (threshold) | −1.0 dBTP default; adjustable −6.0 … −0.1 dBTP (Advanced) | [S] (EBU R128 practice) / brief |
| Detector | True-peak: 4× polyphase oversampling (48 taps per phase, Kaiser β = 8 half-band design) at 44.1/48 kHz; 2× at 88.2/96 kHz. Detector only: the gain is applied at the base rate | [S] BS.1770 Annex 2 |
| Lookahead | 5.0 ms (240 samples at 48 kHz) | [I] |
| Attack | Gain curve reaches the required reduction exactly at the peak using a lookahead minimum-hold (sliding-window min over 5 ms) followed by a 5 ms moving-average smoother (Hann-shaped via cascaded box filters), giving a click-free ramp | [I] |
| Detector | Adds 24 samples of intrinsic delay; smoother = lookahead − detector delay = 240 − 24 = 216 samples = 4.5 ms (revised during implementation) | [I] |
| Release | Two-stage program-dependent: fast τ = 80 ms for isolated peaks (GR < 2 dB, duration < 50 ms); slow τ = 600 ms otherwise. Release is from a peak-hold of 10 ms | [I] |
| Linking | Linked within each zone (identical gain on all channels of a zone) so that the spatial balance in a zone does not shift. Zones are independent | [I] |
| Latency | 5 ms total, applied equally to all channels (revised during implementation) | [I] |
| Safety clip | Hard clip at ±1.0 after the limiter. Each clipped sample increments `clipEvents` (a diagnostic; must remain 0) | [I] |
| Bypass | Advanced only. When bypassed, the safety clip remains, and the status bar shows "Limiter disabled" | GUI §40 |
| CPU | ≤ 0.3 % of one core per channel at 48 kHz (benchmark) | [I] |

**Alarm criteria (computed on the analysis thread from RT-published GR statistics):**

| Condition | Severity | Action |
|---|---|---|
| GR > 0.5 dB for > 1 % of time in a 60 s window | INFO `limiter.active` | Advanced meter highlight |
| GR > 0.5 dB for > 5 % of time in 60 s, or any GR > 3 dB | WARN `limiter.sustained` | Status bar "Output limiting — reduce Strength", Simple meter shows HIGH, log |
| GR > 6 dB at any time, or `limiter.sustained` for > 10 min | ERROR `limiter.overload` | Engine enters DEGRADED (`RELIABILITY.md`), log. Audio continues |
| Any safety-clip event | ERROR `output.clip` | DEGRADED, log with count |

The limiter never reduces Strength automatically. Automatic level control belongs to V2 adaptive masking.

---

## 7. CPU budget

Measured as **DSP load** = callback processing time ÷ buffer duration on one core, plus total process CPU on the whole machine.

| Configuration | DSP load target | Total process CPU target | Label |
|---|---|---|---|
| Stereo, Balanced, 10 talkers slots, 48 kHz, 480-frame buffer | ≤ 5 % of one core | ≤ 3 % of machine | [E] target, not guaranteed until benchmarked |
| 8 channels, 24 talker slots | ≤ 10 % of one core | ≤ 6 % | [E] |
| 16 channels, 48 talker slots (distributed) | ≤ 20 % of one core | ≤ 10 % | [E] |
| 96 kHz, 8 channels | ≤ 2.2 × the 48 kHz value | — | [E] |
| Analysis thread | ≤ 5 % of one core at 8 ch | — | [E] |

**Cost model (for guidance):**
- Per talker: ≈ 6 flops/sample/channel-with-nonzero-gain.
- Per channel: two uniformly-partitioned FFT convolutions (babble 2048 taps, stationary 4096 taps, 256-frame partitions; at 88.2/96 kHz twice the taps with 512-frame partitions), about 250 flops/sample; decorrelator about 20 flops/sample; limiter about 60 flops/sample (its true-peak oversampler only runs on blocks whose peak bound can reach the ceiling).

**Benchmark reference machines [I]:**

| Class | Example | Purpose |
|---|---|---|
| Low | Intel Core i5-8250U laptop, 8 GB RAM, Windows 11, on battery "balanced" | Worst supported |
| Mid | Intel Core i5-12400 or AMD Ryzen 5 5600, 16 GB | Typical desktop |
| Future | Apple M1 8 GB (when macOS is built) | Portability |

Benchmarks run for 10 minutes per configuration. Report p50, p99 and max callback time plus xruns. A configuration passes when p99 DSP load ≤ target and there are 0 xruns.

---

## 8. Default first-launch configuration

The brief's suggestion is reviewed and adjusted. The first launch must be safe, sound deliberate, and not over-claim.

| Setting | Default | Rationale |
|---|---|---|
| Area | Office | Most common single-room deployment (GUI §68) |
| Strategy | Balanced (= HybridMask) | Combines energetic coverage (stationary) with informational/natural character (babble). Not claimed optimal [E] |
| Babble energy fraction | 0.70 (stationary 0.30) | [E] Moderate stationary component fills envelope minima. Validation must compare 0.5/0.7/1.0 |
| Mean active talkers | 6.5 (pool 14, min 4, max 9) | [E] Inside the 4–8 "balanced babble" band. 7 voices had the best writing-task result in Keus van de Poll et al. [R for the finding; E for applying it here] |
| Character | 0.55 (between Balanced and Dense) | GUI §68 specifies 60 %. 0.55 is used so that Character "Balanced" anchor + small density bias; either is acceptable [E] |
| Spectrum | Universal LTASS (both components) | Speech-matched is preferred when a profile exists [R: Renz 2018], but no profile exists at first launch |
| Max internal gap | 250 ms (from Character 0.55) | [E] |
| Output | Stereo, Distributed stereo algorithm, spread 0.6 | [E] |
| Speaker Variation | Medium | [E] |
| Strength | Normal (0 dB → −26 dBFS RMS per channel) | [I] |
| Limiter | On, −1 dBTP | [S] |
| Seed | Random (fresh seed per start, logged) | Reproducible on demand |
| Fallback policy | Continuous | `RELIABILITY.md` §4 |
| Spectral correction | On, Normal speed | `SPECTRUM_ENGINE.md` §6 |

**If no corpus is installed at first launch:** the engine offers Speech-Shaped Stationary (Universal LTASS) as the only runnable strategy. Babble strategies show "Voice library required". This is not an error state.

---

## 9. Open-source references and licensing

| Project | License | Use in BabbleForge | Production dependency? |
|---|---|---|---|
| JUCE | AGPLv3 / commercial | Audio I/O, threading, file I/O, DSP primitives (`dsp::Convolution` not used on RT path, see `SPECTRUM_ENGINE.md` §4.4). Device layer and GUI. Chosen for implementation (revised during implementation); licence decision pending with the project owner | Yes (license choice is a project decision) |
| Spatial Audio Framework (SAF) | ISC core; some optional modules GPLv2 | Reference for VBAP/MDAP, lattice all-pass decorrelator design, arbitrary layouts. Core ISC modules may be linked; GPL modules must not be | Optional (ISC modules only) |
| Pyroomacoustics | MIT | Offline room simulation in the validation toolchain | No |
| Google speech_intelligibility_index | Apache-2.0 (archived Apr 2026) | SII reference oracle for V2 tests | No |
| zawi01/STIPA, Cieslar-Simon/STI | GPL-3.0 | Reference/oracle only, run as external tools | No |
| Open Sound Meter | GPL-3.0 | Architecture study for V2 measurement | No |
| IR Measurement Toolbox | MIT | Swept-sine reference (Farina ESS) for V2 | No |
| WebRTC VAD | BSD-3-Clause | Built-in corpus VAD (ingestion only) | Yes (ingestion) |
| Silero VAD (+ ONNX Runtime, MIT) | MIT | Optional higher-accuracy ingestion VAD | Optional plug-in |
| libFLAC | BSD-3-Clause | Corpus cache codec | Yes |
| SQLite | Public domain | Corpus metadata DB | Yes |
| r8brain-free-src | MIT | Offline high-quality resampling (ingestion + preload) | Yes |
| nlohmann/json | MIT | Preset/config parsing | Yes |
| Microsoft DNS Challenge | MIT code, mixed data | Pattern for configuration-driven deterministic synthesis | No |
| MUSAN | CC BY 4.0 | Development test corpus | No (test data) |
| Mozilla Common Voice | CC0, with distribution terms | Research corpus via the corpus-builder only; not bundled | No |

Rule: GPL code is never copied into the production tree. Algorithms are implemented from standards and papers.

---

## 10. Open Research Questions

Every item below is an assumption encoded as [E] that requires experimental verification (`VALIDATION.md` §6–7).

1. **Stationary vs babble vs hybrid**: which masker (and which babble energy fraction: 0, 0.25, 0.5, 0.75, 1.0) minimizes intelligibility and cognitive disruption per area type, at equal A-weighted level? The Renz 2018 advantage for speech-matched SSN is established for serial recall in one lab setting only.
2. **Mean active talker count** per area: are 4.5 / 6.5 / 8.5 / 12 appropriate? Does the 7-voice result (Keus van de Poll 2015, writing task) generalize to serial recall, reading and speech recognition?
3. **Min/max active bounds and variance**: does variability in the active count (binomial-like) help naturalness without opening glimpses?
4. **Maximum internal gap** mapping 600/250/100 ms for Natural/Balanced/Dense: effect on glimpsing and on perceived naturalness.
5. **Talker-level variation** σ = 3.0/2.0/1.0 dB: effect on informational masking (dominant-talker salience).
6. **Clear Voice Reduction** dominance cap and minimum-overlap rules: do they reduce "intelligible fragment" reports?
7. **Spectrum curve**: LTASS vs speech-matched vs −5/−7/−9 dB/oct per area. Does the −3 dB 125 Hz trim in Small Room help or hurt? Renz et al. found 125 Hz content relevant.
8. **Spectral correction** loop parameters (τ_c 120 s, 60 s estimator, ±6 dB, 0.5 dB/min) — perceptibility of correction changes.
9. **Decorrelation**: does all-pass decorrelation of babble add benefit beyond independent-content rendering? Target correlation thresholds (0.3 / 0.15) are engineering values.
10. **Spatial spread** values per area and the stereo pan limit (far channel ≥ −12 dB): trade-off between diffuse coverage and spatial release from masking.
11. **Voice diversity vs target similarity**: does "Matched" diversity outperform "High" when target speakers are known?
12. **Natural motion** of talkers in Common Area: any measurable benefit or distraction?
13. **Language mix**: does a different-language masker change informational masking for the target population?
14. **Minimum corpus size**: are 24 speakers and 15 min/speaker sufficient to avoid perceived repetition over 8 h and multi-day runs?
15. **Crest factor and headroom**: is L_ref = −26 dBFS appropriate for typical amplifier gain structures in the field?
16. **Machine robustness**: relative robustness of SSN vs babble against enhancement, separation and ASR.
17. **Outdoor/free-field defaults**: entirely engineering-derived; requires field trials.
18. **Universal LTASS table**: the table now uses the ANSI S3.5-1997 standard speech spectrum (160 Hz–8 kHz, normal effort); 100/125 Hz and 10 kHz are extrapolated. Open: verify against the Byrne et al. 1994 universal LTASS table and replace the extrapolated bands with measured values.

---

## 11. Implementation Priorities

Build order; each step ends with its tests green.

| # | Milestone | Content | Exit criterion |
|---|---|---|---|
| 1 | Foundations | Repo, CMake, CI, PRNG (`xoshiro256**`/SplitMix64/stream derivation), detmath, config/JSON parser with schema validation, logging skeleton | PRNG golden vectors, parser tests |
| 2 | Offline renderer skeleton | `bfrender` CLI, float WAV writer, N-channel graph, block-size-independent processing | Bit-identical output across block sizes |
| 3 | Spectrum targets + FIR design | Target curves, min-phase FIR design, partitioned convolver | Filter response within ±0.5 dB of target 100 Hz–10 kHz |
| 4 | Stationary Mask Engine | Seeded noise, shaper, level normalization | RMS ±0.1 dB, spectrum ±1 dB (1/3-oct, 60 s) |
| 5 | Corpus ingestion (CLI first) | Decode, resample, VAD, P.56, LTASS, F0, quality, SQLite, FLAC cache | Ingest MUSAN speech subset; metadata tests |
| 6 | Talker planner + selector (offline) | Planner, distributions, cooldowns, selection | Statistical tests on 1 h simulated plans |
| 7 | Talker Engine + babble normalization | Voices, fades, count normalization, slow trim | Laboratory 1/2/4/8/16 fixtures: equal RMS ±0.5 dB |
| 8 | Hybrid mixer + strategies + macros | Plans, crossfades, Character, Clear Voice Reduction, Voice Amount | Mix-level tests, macro monotonicity tests |
| 9 | Analysis | 1/3-oct analyzer, modulation/gap analyzer, slow correction loop | Babble spectrum ±1.5 dB after convergence |
| 10 | Real-time host | Device layer, RT callback, lock-free messaging, preloader threads, engine state machine | 1 h real-time run, 0 xruns, no RT allocations (instrumented) |
| 11 | Limiter + meters | True-peak limiter, BS.1770 meters | EBU Tech 3341/3342 meter conformance vectors, limiter tests |
| 12 | Spatial | Stereo, small multichannel, distributed, decorrelators, zones, output matrix | Correlation and energy tests |
| 13 | Presets | Area × Strategy data, migration, last-known-good | Round-trip and migration tests |
| 14 | Reliability | Fallback policies, device loss, diagnostics, watchdog | Fault-injection suite |
| 15 | Calibration bus (speaker test) | Channel-ID signal, pink noise | Routing test passes |
| 16 | Long-run and validation | 24 h and 7-day soak, scientific fixture generation, reports | Acceptance criteria §12 |

---

## 12. V1 Engine Acceptance Criteria

All criteria are measurable. "Factory preset" means every Area × Strategy combination shipped. Unless stated otherwise, the reference corpus is the pinned QA corpus (≥ 24 speakers).

### 12.1 Real-time and reliability

| ID | Criterion | Pass condition |
|---|---|---|
| A-RT-1 | No allocation or lock in callback | Instrumented build (allocation and mutex hooks on RT thread) reports 0 events over a 24 h run |
| A-RT-2 | Xruns | 0 engine-caused xruns in 24 h at stereo/480 frames and 8 ch/256 frames on the Mid machine |
| A-RT-3 | Soak | 7-day continuous run: memory growth < 10 MB after the first hour; handle count stable (±5); no state change except RUNNING |
| A-RT-4 | Device loss | Unplugging the device yields DEVICE_LOST within 2 s, no crash; no audio on any other device; reconnect restores RUNNING in ≤ 5 s after user action |
| A-RT-5 | Sample-rate change | Device rate change 48→44.1 kHz: engine returns to RUNNING within 3 s; spectrum error criterion still met at the new rate |
| A-RT-6 | Missing/corrupt files | 20 % of cache files deleted or corrupted mid-run: audio continues without dropout; DEGRADED reported; per-policy behavior verified |
| A-RT-7 | CPU | Targets of §7 met on the Mid machine (p99) |

### 12.2 Level

| ID | Criterion | Pass condition |
|---|---|---|
| A-LV-1 | Stationary RMS | 60 s RMS within ±0.1 dB of L_ref + Strength |
| A-LV-2 | Babble RMS (mean-active ≥ 4) | 60 s RMS within ±0.5 dB; 95 % of 3 s windows within ±3 dB |
| A-LV-3 | Hybrid mix constancy | For b ∈ {0, .25, .5, .75, 1}, 60 s output RMS within ±0.5 dB of each other |
| A-LV-4 | Strategy switch | Switching between any two factory strategies: 10 s RMS before vs after within ±1.0 dB; no 50 ms window during the crossfade deviating more than +2 dB/−3 dB |
| A-LV-5 | Character sweep | Sweeping Character 0→1 over 60 s: every 10 s window RMS within ±1.0 dB of L_ref |
| A-LV-6 | Limiter transparency | Factory presets at Strength ≤ +6 dB: GR = 0 for ≥ 99.9 % of samples over 1 h |
| A-LV-7 | True peak | Output true peak never exceeds ceiling + 0.2 dB (limiter on) with any input |
| A-LV-8 | Meters | BS.1770 meters pass EBU Tech 3341 (M/S/I) and 3342-independent true-peak vectors within stated tolerances |

### 12.3 Spectrum and temporal

| ID | Criterion | Pass condition |
|---|---|---|
| A-SP-1 | Stationary spectrum | 1/3-oct levels 100 Hz–10 kHz within ±1.0 dB of target (60 s average, shape-normalized) |
| A-SP-2 | Babble spectrum | After 5 min convergence: 1/3-oct 125 Hz–8 kHz within ±2.0 dB, octave bands within ±1.0 dB |
| A-SP-3 | Octave coverage | All seven STI octave bands 125 Hz–8 kHz present within ±1 dB of target in the stationary component; 125 Hz band not attenuated by any default high-pass (LF limit ≤ 80 Hz) |
| A-SP-4 | Correction stability | Correction gains move ≤ 0.5 dB/min; no oscillation (sign changes of the update in one band ≤ 4 per 10 min after convergence) |
| A-TM-1 | Mean active talkers | Over 1 h: measured mean within ±5 % of configured mean; min/max bounds never violated (except during starvation events, which are counted) |
| A-TM-2 | Macro monotonicity | Across Character 0, 0.5, 1: median gap and 95th-percentile gap strictly decrease; envelope L10−L90 strictly decreases |
| A-TM-3 | Laboratory talker series | 1/2/4/8/16-talker fixtures: equal RMS (±0.2 dB), equal LTASS (±1 dB 1/3-oct), envelope modulation depth monotonically decreasing with N |
| A-TM-4 | Repetition | 8 h run: no material region reused inside its effective cooldown (`TALKER_ENGINE.md` §6.4); effective cooldown ≥ 30 min with the reference corpus (≥ 24 speakers × 15 min) at the Office default; no speaker selected while already active; no two consecutive selections of the same speaker |

### 12.4 Spatial

| ID | Criterion | Pass condition |
|---|---|---|
| A-SPA-1 | Stationary decorrelation | \|ρ\| over 60 s < 0.02; median of 10 s windows < 0.02; 95th percentile < 0.03 (independent noise has ±0.01 SD per 10 s window) (revised during implementation) |
| A-SPA-2 | Babble correlation | Adjacent-channel ρ: Medium ≤ 0.30, High ≤ 0.15 (95th percentile of 10 s windows) in small-multichannel and distributed modes |
| A-SPA-3 | Channel energy balance | Long-term (10 min) per-channel RMS within ±1.0 dB of each other (before user trims) |
| A-SPA-4 | No hard panning | Stereo: no talker's far-channel gain below −12.4 dB relative to its near channel (revised during implementation). Small multichannel: every talker feeds ≥ 2 outputs, with the second-largest gain ≥ −9 dB relative to the largest (`SPATIAL_ENGINE.md` §3) |
| A-SPA-5 | Stereo babble correlation | 95th-percentile broadband ρ: Medium ≤ 0.5, High ≤ 0.3 |

### 12.5 Determinism

| ID | Criterion | Pass condition |
|---|---|---|
| A-DT-1 | Same platform | Same preset + corpus version + seed + duration → bit-identical WAV (SHA-256) across 3 runs and across block sizes 64/256/480/1024 |
| A-DT-2 | Cross-platform event list | Event timelines (JSON) identical across Windows/Linux builds |
| A-DT-3 | Cross-platform audio | Difference signal ≤ −100 dB relative to output RMS |

### 12.6 Scientific release gate (V1.0)

| ID | Criterion |
|---|---|
| A-SCI-1 | Validation fixture set (`VALIDATION.md` §5) generated and archived with metrics |
| A-SCI-2 | Listening study (`VALIDATION.md` §6) shows at least one factory preset significantly reduces word/sentence recognition relative to no masker (p < 0.05, pre-registered analysis) at the tested speech-to-masker ratios |
| A-SCI-3 | No user-visible text describes any preset as optimal, and no digital value is labelled SPL/dBA (text audit) |
