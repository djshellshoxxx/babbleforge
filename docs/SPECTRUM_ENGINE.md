# BabbleForge V1 — Spectrum Engine, Stationary Masker and Temporal Analysis

Covers brief sections 18, 19, 21, 22, 23. Labels are defined in `ENGINE.md` §0.1.

---

## 1. Responsibilities

1. Hold spectrum **targets** (the curves the masker should follow).
2. Design shaping **filters** for the stationary masker and the babble bus.
3. Generate the **stationary speech-shaped masker**.
4. **Measure** the component and output spectra (octave, 1/3-octave, FFT).
5. Run the **slow spectral correction** loop for the babble component.
6. **Analyze temporal behavior**: occupancy, gaps, envelope and modulation spectrum.

---

## 2. Spectrum targets

### 2.1 Representation

The canonical representation is **1/3-octave band levels** (band *power*, not spectral density; every target and every slope in this document is expressed as band levels) at IEC 61260 nominal centres from 50 Hz to 16 kHz (26 bands), in dB, shape-only (relative). Octave levels are derived by power-summing the three constituent 1/3-octave bands.

```cpp
struct SpectrumTarget {
    std::string id;                 // "ltass_universal", "slope_-5", "speech_matched:<uuid>", "custom:<uuid>"
    std::array<float,26> thirdOctDb;// relative dB at 50,63,80,100,…,16000 Hz
    float lfLimitHz  = 80.f;        // high-pass corner below which the target rolls off (12 dB/oct)
    float hfLimitHz  = 12500.f;     // low-pass corner (clamped to 0.45·fs)
    float lfTrimDb125 = 0.f;        // area-model trim applied to the 100/125/160 Hz bands
    uint32_t revision;
};
```

**Operating range:** 100 Hz–10 kHz (21 bands). This includes all seven STI octave bands, 125 Hz–8 kHz [S: IEC 60268-16]. Bands outside the operating range define roll-off behavior only.

**Low-frequency rule [R]:** the default LF limit is 80 Hz, a 2nd-order Butterworth-equivalent high-pass. The 125 Hz octave band (88–177 Hz) is therefore attenuated by < 1 dB at 125 Hz. This follows Renz et al.'s loudspeaker-location study, in which lowering the masker high-pass from 200 Hz to 100 Hz removed a performance disadvantage, and 125 Hz octave content was identified as relevant. The LF limit is configurable (40–200 Hz) for small loudspeakers, and a UI warning appears above 100 Hz.

### 2.2 Built-in targets

All slope curves are anchored at 0 dB at 1 kHz and are flat below 200 Hz before the LF limit [E: typical masking-curve practice]. Values are band levels relative to the 1 kHz band.

| Target | Definition | Label |
|---|---|---|
| **Universal LTASS** | Byrne et al. 1994 universal LTASS (average of male and female), 1/3-octave, stored as `targets/ltass_universal_byrne1994.json` | [R] |
| **Speech Matched** | Imported profile (§3) | [R: Renz 2018] |
| **−5 dB/oct** | L(f) = −5·log2(f/1000) for f ≥ 200 Hz; L = L(200) below | [R: historical practice; not claimed optimal] |
| **−7 dB/oct** | same with −7 | [R: recent office literature cites −7…−9] |
| **−9 dB/oct** | same with −9 | same |
| **Pink (reference)** | 0 dB in every 1/3-octave band (equal power per band = −3.01 dB/oct spectral density) | [S] |
| **White (flat density)** | Band levels +10·log10(f/1000) dB (+3.01 dB/oct), testing only | [I] |
| **Custom** | 7 octave points (125…8k) ±12 dB (UI recommends ±6), PCHIP-interpolated to 1/3-octave in log-frequency; or a full 26-band table from a file | GUI §31 |

**Universal LTASS values (relative to the 1 kHz band).** Source: ANSI S3.5-1997 Table 3 standard speech spectrum level, normal vocal effort (the SII standard speech spectrum, closely related to the LTASS), as reproduced in the Apache-2.0 repository `google/speech_intelligibility_index` (`speech_intelligibility_index/sii.py`, https://raw.githubusercontent.com/google/speech_intelligibility_index/main/speech_intelligibility_index/sii.py). Spectrum levels (dB/Hz) were converted to 1/3-octave band levels by adding 10·log10(bandwidth) with IEC base-10 band edges (fc·10^±0.05), then normalised to the 1 kHz band. The Byrne et al. 1994 table could not be verified from two independent reproductions and is not used. The 100 Hz, 125 Hz and 10 kHz values are **extrapolated** (outside the 160 Hz–8 kHz range of the standard); bands outside 100 Hz–10 kHz hold the edge value. The implementation loads the data file, so values can be replaced without code changes.

| Hz | 100 | 125 | 160 | 200 | 250 | 315 | 400 | 500 | 630 | 800 | 1k | 1.25k | 1.6k | 2k | 2.5k | 3.15k | 4k | 5k | 6.3k | 8k | 10k |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| dB | −9.0 (extrap.) | −4.6 (extrap.) | −0.6 | 2.5 | 3.7 | 4.0 | 5.6 | 6.3 | 5.1 | 2.3 | 0.0 | −1.0 | −2.9 | −4.7 | −7.8 | −8.5 | −9.7 | −12.7 | −14.4 | −14.9 | −16.0 (extrap.) |

### 2.3 Effective targets per component

| Component | Filter target |
|---|---|
| Stationary | T(f) (with the area's `lfTrimDb125`) |
| Babble | T(f) − LTASS_pool(f) + C(f), where LTASS_pool is the active-speech-weighted power average of pool speakers' LTASS (from metadata; recomputed when the pool changes or rotates) and C(f) is the slow correction (§6) |

The babble static EQ is clamped to ±12 dB per band [I]. A clamp hit is logged as `spectrum.eqClamped` (it usually indicates a corpus with band-limited recordings).

---

## 3. Speech-matched spectrum profiles

V1 imports a saved profile. V2 will learn one by measurement (`V2_EXTENSION_POINTS.md`). Both produce the same file format.

```json
{
  "schema": "babbleforge.spectrumprofile/1",
  "id": "8f0c3f7e-4d7b-4a70-9d1e-1b6c2f0a9e11",
  "name": "Reception desk speech (estimated)",
  "created": "2026-09-30T10:12:00Z",
  "source": "import",                       // "import" | "corpus" | "measured" (V2) | "literature"
  "resolution": "third_octave",             // or "octave" (7 bands; expanded by PCHIP)
  "bandCentersHz": [100,125,160,200,250,315,400,500,630,800,1000,1250,1600,2000,2500,3150,4000,5000,6300,8000,10000],
  "levelsDb":      [-8.1,-4.0,-2.2,-1.0,0.0,0.3,0.1,0.0,-1.2,-3.1,-4.6,-6.0,-7.7,-8.8,-9.9,-10.8,-12.1,-13.4,-14.2,-15.5,-17.9],
  "normalization": { "type": "relative", "referenceBandHz": 500, "referenceDb": 0.0 },
  "measurement": null,                      // V2: { micClass, positions, durationS, vadRatio, calibrated }
  "confidence": "estimated",                // "estimated" | "measured" | "calibrated"
  "notes": "Imported from consultant report."
}
```

**Validation on import:**
- 7 or 21–26 bands, monotonically increasing centres matching IEC nominal values within 3 %
- values within ±40 dB
- no NaN

The loader renormalizes to the 1 kHz band = 0 dB internally. **Import from corpus:** "Use voice-library LTASS" creates a profile from the corpus-wide LTASS (`source: "corpus"`).

---

## 4. Stationary speech-shaped masker

### 4.1 Generator

```text
for each output channel c (independent):
   PRNG stream "noise.ch.<c>"  (xoshiro256**, REALTIME_ARCHITECTURE.md §8)
   u[n] = ((x >> 40) · 2^-24) · 2 − 1           // 24-bit uniform on [−1, 1)
   w[n] = √3 · u[n]                            // unit variance white noise
   s[n] = (h_stat ∗ w)[n]                      // min-phase FIR, Σh² = 1 → unit variance
   out  = g_ref · s[n]                         // g_ref = 10^(L_ref/20)
```

- **Why uniform input:** after a long shaping FIR (4096 taps), the output is Gaussian by the central limit theorem; the kurtosis error is < 0.1 %, which is verified in tests. A uniform source avoids transcendental functions in the RT path and is bit-reproducible across platforms.
- **No looping:** the PRNG period is 2^256 − 1, and there are no stored noise tables.
- **Reproducible:** the stream seed is derived from the master seed and the channel index.
- **Stable RMS:** analytic (Σh² = 1). Expected 60 s RMS standard deviation < 0.05 dB.
- **Channel independence:** separate streams give |ρ| < 0.02 in 10 s windows (A-SPA-1). The stationary component therefore needs no decorrelator.

### 4.2 FIR vs IIR

| Criterion | FIR (frequency-sampled, min-phase) | IIR (biquad cascade / graphic EQ) |
|---|---|---|
| Matches arbitrary 1/3-oct targets | Exact to design resolution | Needs a fitting step; band interaction |
| Energy normalization | Exact (Σh²) | Numerical integration of the response |
| Updating without artefacts | Kernel crossfade (simple, robust) | Coefficient interpolation risks instability and transients |
| Phase | Min-phase, no pre-ringing | Min-phase |
| CPU | Partitioned FFT convolution: ~250 flops/sample/channel for 4096 taps | ~40–100 flops/sample for 10–20 biquads |
| Determinism | Exact (FFT with fixed plan) | Exact |

**Decision:** use **minimum-phase FIR with uniformly partitioned FFT convolution** for both the stationary shaper and the babble shaper [I]. The extra CPU (about 2–4 % of one core at 16 channels) buys:
- exact spectral matching to 1/3-octave targets
- exact energy normalization
- trivial artefact-free updates

An IIR design would need a fitter and still would not match curve shapes as exactly.

### 4.3 Filter design procedure (worker thread)

**Input specification (revised during implementation):** FirDesigner input is a spectral-density-equivalent band-level target (white noise → given band levels). Callers converting per-band gains must add the bandwidth term.

```text
Input: target band levels B[k] (26 bands, dB), fs, taps L, lfLimit, hfLimit
1. N_fft = 4·L (L = 4096 → 16384 at 48 kHz; L scales ×2 at 88.2/96 kHz)
2. For each FFT bin frequency f_i (i = 1..N_fft/2):
      D_dB(f_i) = PCHIP interpolation of B over log2(f) (flat extrapolation beyond ends)
      D_dB += 20log10|H_hp(f_i)|  (2nd-order Butterworth HP at lfLimit)
      D_dB += 20log10|H_lp(f_i)|  (4th-order Butterworth LP at min(hfLimit, 0.45 fs))
   D_dB(0) = D_dB(f_1) − 60 dB  (DC suppression)
3. |D| = 10^(D_dB/20)
4. Minimum phase via real cepstrum:
      c = IFFT(log(max(|D|, 1e-6)))
      fold: c_min[0]=c[0]; c_min[n]=2c[n] (1≤n<N/2); c_min[N/2]=c[N/2]; else 0
      H_min = exp(FFT(c_min));  h = real(IFFT(H_min))
5. Truncate to L taps, apply half-Hann taper over the last 10 % of taps
6. Normalize: h ← h / √(Σ h²)                          (unit energy)
7. Verify: compute the 1/3-oct response of h against white noise (analytic |H|² band integration)
   → max deviation 100 Hz–10 kHz must be ≤ 0.5 dB, else double L (up to 16384) and retry;
     log `spectrum.designDegraded` if still failing
8. Partition kernel for the convolver (partition = 256 samples at 44.1/48 kHz, 512 at 88.2/96 kHz), publish via RCU
```

- Design time: ≤ 30 ms for L = 4096 on the Mid machine (FFT-based).
- Babble shaper taps: 2048 at 48/44.1 kHz and 4096 at 88.2/96 kHz. This is sufficient because the babble static EQ is smooth; the verification step still applies.
- Energy normalization is applied to the stationary kernel only. The babble kernel is normalized so that its gain for the *pool LTASS* input equals 1 (Σ over bands of |H|²·P_pool = Σ P_pool), which keeps the babble bus level unchanged by EQ changes.

### 4.4 Convolver

A uniformly partitioned overlap-save convolver:
- partition P = 256 at 44.1/48 kHz and 512 at 88.2/96 kHz (the same ≈ 5.3 ms in time; revised during implementation). The kernels at 88.2/96 kHz have twice the taps, so a fixed 256 partition would make the 96 kHz shaper cost 4× the 48 kHz one per second; with P scaled to the rate it costs ≈ 2×, which the ENGINE.md §7 "96 kHz ≤ 2.2 × 48 kHz" budget needs. Latency = P samples.
- FFT size 2P (in-house radix-2 FFT with a split re/im work buffer and per-stage twiddle tables, so the butterflies vectorise; bit-identical to the textbook loop)
- frequency-domain delay line of L/P partitions, spectra stored planar (re[], im[], stride P+1 rounded up to 8)
- complex multiply-accumulate in SIMD (compiler-vectorised planar loop; same per-bin operation order as the scalar form)
- one set of kernel spectra shared by all channels of a shaper (each channel's convolver holds its own small kernel object for the RCU hand-over), so N channels keep one copy in cache

It is written in-house (or uses `juce::dsp::FFT` with a fixed-size plan) with no allocation after `prepare`. `juce::dsp::Convolution` is not used on the RT path, because its internal loading behavior is not specified for this lock-free kernel-swap protocol.

**Kernel update:** the new kernel is posted via an RCU pointer. The RT thread runs old and new convolvers in parallel for 100 ms (4800 samples at 48 kHz) and crossfades equal-power, then releases the old kernel to the garbage queue. At most one pending update per shaper is allowed; newer updates replace a pending one.

---

## 5. Spectrum measurement

### 5.1 Analyzer

| Item | Specification |
|---|---|
| Taps analyzed | T1 babble (channel-power-sum), T2 stationary, T3 pre-master mix, T4 output (per channel and sum) |
| Method | FFT-based band power aggregation: Hann window, 8192 points at 48 kHz (5.9 Hz resolution; scaled with fs), 50 % overlap; power summed in IEC 61260 1/3-octave band edges (base-10, f_c·10^(±1/20)) with fractional-bin weighting at edges |
| Accuracy check | Against an IEC 61260 class 1 filterbank reference (offline test): ≤ 0.2 dB difference for pink and SSN inputs, 100 Hz–10 kHz |
| Resolutions offered | Octave (7 bands, 125–8k), 1/3-octave (21 bands, 100–10k), FFT display (log-frequency smoothed 1/24-octave for display only) |
| Averaging | Short: 1 s exponential (display); analysis block: 5 s; long-term: exponential power average, τ = 60 s |
| Shape comparison | Deviation d_b = L_meas,b − L_target,b − μ, where μ = the power-weighted mean offset across the 21 operating bands (so level differences do not appear as spectral error) |
| Published metrics | Per band d_b, RMS deviation over 125 Hz–8 kHz, max \|d_b\| and its band, spectral slope (least-squares dB/oct over 250 Hz–4 kHz), LF content (energy 100–200 Hz rel. total), HF content (energy 4–10 kHz rel. total), speech-band energy (200 Hz–5 kHz rel. total) |

### 5.2 Why FFT aggregation rather than a filterbank

Band levels are needed only at 5 s and 60 s time scales, with no real-time constraint. FFT aggregation is cheaper, deterministic and accurate enough (≤ 0.2 dB). A true IEC filterbank is used only in offline validation (`bfanalyze`).

---

## 6. Slow spectral correction (babble component)

### 6.1 Purpose and non-goals

Purpose: remove long-term spectral deviation of the babble component caused by:
- the pool's actual composition differing from its metadata LTASS
- level-variation weighting
- pause shortening

It must **not** behave like a multiband compressor or dynamic EQ: short-term speech spectral fluctuation (phonemes, individual talkers) must pass untouched.

### 6.2 Review of the brief's values

| Brief | Adopted | Reason |
|---|---|---|
| Analysis window 2–10 s | **5 s blocks** feeding a **60 s exponential long-term estimate** | A single 5 s block of a 4–8-talker babble has a 1/3-oct band-level standard deviation of about 1–2 dB at LF (few independent events). Correcting on raw 5 s blocks would chase noise. The 60 s estimate reduces this to about 0.3–0.5 dB [I, statistics] |
| Integration 30–120 s | **Closed-loop time constant τ_c: Slow 300 s, Normal 120 s, Fast 45 s** | τ_c ≥ 2 × the estimator time constant avoids loop oscillation (the estimator lag acts as a delay). 30 s is too fast relative to the 60 s estimator [I, control theory] |
| Max correction ±6 dB | **±6 dB absolute clamp; ±4 dB "healthy" range** | Beyond ±4 dB the static EQ or pool is wrong: the engine flags `spectrum.correctionLarge` and recommends "Rebuild Analysis" instead of silently correcting |
| — | **Slew limit 0.5 dB/min per band** (Fast: 1.0 dB/min; Strict-mode laboratory: 3 dB/min) (revised during implementation) | Guarantees inaudible drift [E] |
| — | **Deadband ±0.3 dB** | Stops limit cycling from estimator noise [I] |
| — | **Known limitation: 1-2-1 band smoothing cannot remove single-band errors** (revised during implementation) | Residual up to ~0.9 dB, within acceptance ±2 dB |

### 6.3 Algorithm (analysis thread, every 5 s block)

```text
inputs: P_meas[b]  (60 s exponential band power of T1 babble; b = 21 bands)
        P_tgt[b]   (babble target shape, same normalization)
        C[b]       (current correction, dB)
1. d[b] = 10log10(P_meas[b]) − 10log10(P_tgt[b]) ; d -= powerWeightedMean(d)
2. spatial smoothing across bands: d̃[b] = 0.25 d[b−1] + 0.5 d[b] + 0.25 d[b+1] (edges mirrored)
3. deadband: e[b] = sign(d̃)·max(|d̃| − 0.3, 0)
4. integrator step: ΔC[b] = −(T_block / τ_c) · e[b]      (T_block = 5 s)
5. slew limit: ΔC[b] = clamp(ΔC[b], −s·T_block/60, +s·T_block/60)   (s = 0.5 dB/min)
6. C[b] = clamp(C[b] + ΔC[b], −6, +6); re-center C so that its power-weighted mean = 0
7. if max|ΔC accumulated since last design| ≥ 0.1 dB → redesign babble kernel (≤ once per 15 s)
```

- **Stability protection:**
  - freeze the integrator (no updates) when babble RMS over the block < L_ref − 20 dB, when babble fraction b < 0.05, during strategy/plan crossfades, and for 2 × 60 s after a plan or pool change (the estimator must refill)
  - if the sign of ΔC alternates in the same band more than 4 times in 10 min, halve that band's gain for 30 min (oscillation guard) and log it
- **Filter-update method:** kernel redesign on a worker thread; RCU publish; 100 ms equal-power kernel crossfade (§4.4).
- **Reset behavior:**
  - C = 0 on Start, preset change, strategy change affecting the pool, corpus change, target change or sample-rate change
  - with the option "Remember correction" (default on), C is stored per (preset hash, corpus version, fs) and restored as the initial value, which shortens convergence
  - manual "Reset correction" is exposed in Advanced
- **Stationary component:** not corrected. Its measured deviation is monitored; > 1.0 dB in any operating band raises `spectrum.stationaryOffTarget` (indicates a design or FFT bug).
- **LaboratoryMask "Strict":** a 120 s offline pre-roll converges C with τ_c = 20 s; C is then frozen for the render.

**Convergence expectation:** from a 3 dB initial error, the error falls to < 1 dB within about 6 min at Normal (both slew limit and τ_c bound it).

---

## 7. Relationship of spectrum control to masking science

- Energetic masking is band-wise: the masker must cover the speech bands where target energy exists [R].
- Spectrally matched maskers are more efficient per dB [R: Renz 2018].
- SII weights bands by importance and depends on the band-level speech-to-masker ratio [S: ANSI S3.5]. Holding the masker's band levels on target keeps this relationship predictable for V2 SII estimation.

---

## 8. Temporal / modulation analyzer

### 8.1 Inputs

- Ground truth from the RT voice renderer, per sub-block: k_a (active slots) and k_s (speaking slots, from VAD masks), plus per-slot gains.
- Envelope data from taps T1 (babble) and T3 (mix): 10 ms frame power, computed in the RT thread as a running sum of squares per 480 samples at 48 kHz. This costs 2 flops/sample and is published through an SPSC ring.
- Optional octave-band envelopes are computed on the analysis thread from T3 samples (octave IIR bandpass, 6th order, 125–8k).

### 8.2 Metrics

| Metric | Definition | Window | Relation to intelligibility |
|---|---|---|---|
| Speech occupancy | Fraction of 10 ms frames with k_s ≥ 1 | 60 s, 10 min | Silence in the masker = unmasked target [R: glimpsing] |
| Mean active talkers | mean k_a | 60 s | Control target |
| Mean speaking talkers | mean k_s | 60 s | Effective density |
| Solo exposure | See `MASK_STRATEGIES.md` §6.1 | 60 s | Informational salience of single talkers |
| Gap | A run of consecutive 10 ms frames whose mix (T3) level is < L_eq,10s − 12 dB [E threshold]; gap duration = run length | continuous | Glimpse opportunities [R principle; threshold E] |
| Median gap, 95th-percentile gap | Over gaps ending in the last 60 s | 60 s | — |
| Maximum recent gap | Max gap duration in the last 60 s | 60 s | Worst-case glimpse |
| Gap rate | Gaps per second | 60 s | — |
| Envelope crest (modulation depth index) | L10 − L90 of 10 ms frame levels (dB) | 10 s, 60 s | Higher = more fluctuation = more glimpsing |
| Crest factor (signal) | True-peak − RMS over 10 s | 10 s | Headroom |
| Modulation depth (broadband) | m = std(env)/mean(env) of the 10 ms power envelope, low-passed at 16 Hz | 10 s | — |
| Modulation spectrum | See §8.3 | 20 s blocks, 60 s average | STI-related [S] |
| Temporal density class (GUI) | Low / Medium / High from envelope crest: > 12 dB Low, 7–12 Medium, < 7 High [E] | 60 s | Simple display |

### 8.3 Modulation spectrum (Advanced)

```text
env[n] = 10 ms-frame power of T3 (100 Hz envelope sample rate), optionally per octave band
block  = 20 s (2000 samples), Hann, 50 % overlap
E(f)   = |FFT(env)|, normalized: m(f) = 2|E(f)| / E(0)       (modulation index, as in STI MTF)
bands  = 1/3-octave modulation bands 0.5 … 16 Hz (16 bands: 0.5, 0.63, 0.8, 1, 1.25, 1.6,
         2, 2.5, 3.15, 4, 5, 6.3, 8, 10, 12.5, 16), energy-averaged within band (revised during implementation)
band value = sqrt(Σm²/ENBW) (revised during implementation)
publish: 60 s average of m(f) per band, broadband and per octave carrier (7 × 16 matrix)
```

**Interpretation:**
- Speech itself has peak modulation around 3–5 Hz (the syllabic rate).
- STI evaluates the preservation of target modulations at 0.63–12.5 Hz [S: IEC 60268-16].
- A masker with large modulation at these rates leaves dips (glimpses) and is itself speech-like (informational).
- A masker with low m(f) (stationary or dense babble) masks energetically and continuously.
- Stationary noise (the SSN masker) shows m(f) at the statistical floor of the analysis (≈ < 0.05 for bands ≤ 4 Hz; rises to ≈ 0.1–0.15 at 8–16 Hz for 20 s blocks).
- The analyzer quantifies where each strategy lies on this continuum. It is not a predictor of intelligibility in V1.

Expected behavior (verification, not a claim): m(f) at 4 Hz decreases monotonically with the talker count in laboratory fixtures, and SSN gives m(f) ≈ statistical floor (< 0.05 for 20 s blocks).

### 8.4 Cost and threading

All of §8 runs on the analysis thread except the 10 ms power accumulation and the k_a/k_s flags (RT). Analysis load is ≤ 1 % of one core at 8 channels.
