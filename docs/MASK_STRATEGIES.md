# BabbleForge V1 — Mask Strategies

Covers brief sections 1, 7, 10, 16, 20, 24, 40 (strategy side). Labels [R]/[S]/[E]/[I] are defined in `ENGINE.md` §0.1.

---

## 1. Scientific basis for the strategy set

| Principle | Consequence for strategies | Label |
|---|---|---|
| **Energetic masking**: masker energy overlapping target speech in time-frequency regions reduces audibility | A stationary speech-shaped masker gives continuous spectral coverage | [R] |
| **Informational masking**: competing intelligible speech causes confusion and distraction beyond energy overlap. It is strongest with 1–4 talkers and declines as babble becomes noise-like | Low-count strategies (Multi-Voice) behave qualitatively differently from dense babble; talker count is a primary control | [R] Brungart 2001; Freyman et al. 2004; Rosen et al. 2013 |
| **Temporal glimpsing**: listeners exploit masker envelope dips | Gap duration and envelope depth must be controlled and measured. Stationary noise has no dips | [R] Rosen et al. 2013; masking-release literature |
| **Speech-matched SSN efficiency**: stationary noise matched to the disturbing speech spectrum gave similar recall errors at a 3 dB higher speech-to-noise ratio than −5 dB/oct noise | SSN is first-class, not a "stabilizer" | [R] Renz, Leistner & Liebl 2018 |
| **Multi-voice office masking**: in a writing task, performance improved from 1 to 3, 5 and 7 voices | Multi-Voice 3/5/7 presets; 7 labelled "research-derived starting point" | [R] for the finding; [E] for generalization |
| **Large N → SSN-like**: the babble envelope flattens and the spectrum converges to LTASS as N grows | Dense babble and hybrid form a continuum with SSN; comparisons must be level- and spectrum-matched | [R] |
| **Spatial release from masking**: separated target/masker locations aid segregation | Strategies do not create localized masker sources (`SPATIAL_ENGINE.md`) | [R] |

**No strategy is described as optimal.** The engine exposes all of them at identical long-term level and spectrum so that they can be compared.

---

## 2. Strategy architecture

### 2.1 Interface

Strategies are **non-RT policy objects**. They never process audio. The fixed DSP graph (`ENGINE.md` §2.2) is driven by the `MaskRenderPlan` a strategy produces. This is simpler and more robust than polymorphic `processBlock()` strategies:
- a single level-normalization path serves every strategy
- strategy switches are glitch-free crossfades
- the RT thread has no virtual dispatch into strategy code

```cpp
class MaskStrategy {                                  // control thread only
public:
    virtual ~MaskStrategy() = default;
    virtual StrategyId        id() const = 0;
    virtual StrategyCaps      capabilities() const = 0;   // needsCorpus, supportsMix, minTalkers…
    // Validate + clamp user parameters; returns list of adjustments (for UI/log).
    virtual ValidationResult  validate(StrategyParams&, const CorpusSummary&) const = 0;
    // Pure function: same inputs -> same plan. No I/O, no randomness.
    virtual MaskRenderPlan    buildPlan(const AreaModel&, const StrategyParams&,
                                        const MacroState&, const CorpusSummary&,
                                        const OutputLayout&) const = 0;
    // Degraded-mode transformation (fallback policy); pure.
    virtual MaskRenderPlan    degrade(const MaskRenderPlan&, const DegradeReason&) const = 0;
};

// Runtime statistics are produced by the engine and analyzers, not by the strategy:
struct MaskStatistics {           // published at 1 Hz (analysis thread)
    double meanActiveTalkers60s, meanSpeakingTalkers60s, occupancy60s;
    double medianGapMs, p95GapMs, maxRecentGapMs;
    double envelopeL10minusL90dB, crestFactorDb;
    double babbleRms60sDb, stationaryRms60sDb, outputRms60sDb;
    double babbleFractionMeasured;       // measured power fraction of babble in T3
    std::array<float,21> thirdOctDeviationDb;
    double soloExposurePct;              // % time exactly 1 speaking talker dominant (see §6)
    uint32_t starvationEvents, substitutions;
};
```

The brief's `prepare/reset/processBlock/getStatistics/setTargetLevel/setParameters` concepts map as follows:
- `prepare` → the engine graph `prepare(sampleRate, maxBlock, layout)`
- `reset` → plan re-issue with a new epoch
- `processBlock` → the fixed graph
- `getStatistics` → `MaskStatistics`
- `setTargetLevel` → `LevelParams` (Strength)
- `setParameters` → `validate` + `buildPlan`

### 2.2 Strategy classes

| Class | GUI name(s) | Generators | Babble fraction b (default) | Talker model | Notes |
|---|---|---|---|---|---|
| `StationarySpeechMask` | Speech Noise / Speech-Shaped Stationary | Stationary only | 0.0 (fixed) | none | Runs with no corpus. Spectrum: Speech-Matched / LTASS / −5 / −7 / −9 / Custom |
| `MultiVoiceMask` | Multi-Voice (3, 5, 7, Dense Multi-Voice) | Babble (+ optional stationary) | 1.0 (user may set 0.8–1.0) | **Fixed-count continuous**: exactly K voices speaking; K ∈ {3, 5, 7} or 9 for "Dense Multi-Voice" | Constant simultaneity; each voice plays long segments back-to-back with 150 ms crossfades |
| `BabbleMask` | Maximum Density / Dense | Babble + stationary | area value + 0 (typically 0.60) | Stochastic, Character forced ≥ 0.75 | High overlap, short gaps |
| `HybridMask` | Balanced; Hybrid (explicit mix slider) | Both | Balanced: area default (0.55–0.85); Hybrid: user 0–1 (default 0.5) | Stochastic, Character from UI | The general-purpose engine |
| `NaturalBabbleMask` | Natural | Babble + small stationary | area value + 0.10, clamped ≤ 0.95 | Stochastic, Character forced ≤ 0.35, motion on | Resembles distant conversation |
| `LaboratoryMask` | Research | Babble / stationary / pink / hybrid | explicit | Continuous-N (Rosen-style) or stochastic, all explicit | Strict fallback, fixed seed, locked parameters (§8) |

All classes share the same Area model defaults (`PRESETS.md`). A strategy overrides only the fields listed in its row and in §8.

### 2.3 Capability flags

```cpp
struct StrategyCaps {
    bool needsCorpus;          // false only for StationarySpeechMask and Laboratory(stationary/pink)
    bool exposesMixSlider;     // Hybrid, Laboratory
    bool exposesCharacter;     // Hybrid(Balanced), Babble, Natural
    bool exposesVoiceAmount;   // all babble-based
    bool lockedForResearch;    // Laboratory
    int  minCorpusSpeakers;    // MultiVoice: K+3 ; Babble/Hybrid/Natural: max(ceil(mean)+4, 8)
};
```

---

## 3. Stationary vs babble ownership of shared parameters

| Parameter | Stationary component | Babble component |
|---|---|---|
| Spectrum target | Exact (FIR designed from target) | Static EQ (target ÷ pool LTASS) + slow correction |
| Level | Analytic (unit-energy FIR) | Per-segment ASL + count normalization + slow trim |
| Spatial | N independent noise channels (ρ ≈ 0) | Talker panning + independent content + optional all-pass |
| Seed streams | `noise.ch.<k>` | `planner.*`, `selector`, `gainvar`, `spatial.*` |

---

## 4. Hybrid Mixer

### 4.1 Energy-correct law

Both inputs are normalized to L_ref per channel and are mutually uncorrelated (independent sources). For babble energy fraction b ∈ [0, 1]:

```
g_b = sqrt(b)          g_s = sqrt(1 − b)
y[n] = g_b · babble[n] + g_s · stationary[n]
E[y²] = b·P_ref + (1−b)·P_ref = P_ref            (cross term E[babble·stationary] = 0)
```

This is an equal-power (constant-power) law on energy fractions. A linear amplitude crossfade (g_b = b, g_s = 1 − b) is **forbidden**: at b = 0.5 it gives a power of 0.5 (a −3.01 dB dip).

| Babble fraction b | g_b (lin) | g_b (dB) | g_s (lin) | g_s (dB) | Output power |
|---|---|---|---|---|---|
| 0 % | 0.000 | −∞ | 1.000 | 0.00 | P_ref |
| 25 % | 0.500 | −6.02 | 0.866 | −1.25 | P_ref |
| 50 % | 0.707 | −3.01 | 0.707 | −3.01 | P_ref |
| 75 % | 0.866 | −1.25 | 0.500 | −6.02 | P_ref |
| 100 % | 1.000 | 0.00 | 0.000 | −∞ | P_ref |

### 4.2 Implementation rules

- b is set per zone (`SPATIAL_ENGINE.md` §6): zone b = strategy b + zone offset, clamped to [0, 1].
- Changes to b are smoothed on b itself (not on the gains) with a linear ramp of 2.0 s [I], and the gains are recomputed per 32-sample sub-block. Linear interpolation of the gains inside a sub-block is permitted: the deviation from constant power is < 0.01 dB for 32-sample steps of a 2 s ramp.
- When b reaches exactly 0 or 1 and stays there for > 5 s, the unused generator is **paused** (CPU saving) — except for:
  - the stationary generator in LaboratoryMask, which must retain its seed position
  - any component the fallback policy may need, which is kept "hot" (running, gain 0) when policy = Continuous or Safe, so that fallback is instantaneous
- The paused babble generator keeps planning (the planner continues on its own time base), so unpausing is seamless.
- Measured babble fraction (from T1/T2/T3 power) is published in `MaskStatistics`. It must be within ±0.03 of the configured b over 60 s (acceptance test A-LV-3 complement).

### 4.3 Why constant power is valid here

The two components are statistically independent (different sources, different PRNG streams). The correlation coefficient between the babble and stationary buses in 10 s windows must be |ρ| < 0.02 (tested). If a future generator were correlated with babble (for example, vocoded babble), the mixer would need correlation-aware normalization; this is not permitted in V1.

---

## 5. Activity density model (Natural / Balanced / Dense)

"Activity density" is the scheduler's temporal filling of the babble layer. It is set by the **Character** macro c ∈ [0, 1] (GUI "Natural ↔ Dense"), with named anchors Natural c = 0.0, Balanced c = 0.5, Dense c = 1.0. Intermediate values interpolate piecewise-linearly between anchors, in the dB domain for levels and the log domain for durations. The anchor values below are **Area-independent offsets and scalings**. Area models supply the base values (`PRESETS.md` §3).

### 5.1 Anchor table [E — all values require validation]

| Parameter | Natural (c = 0) | Balanced (c = 0.5) | Dense (c = 1) | Interpolation | Evidence basis |
|---|---|---|---|---|---|
| Mean active talkers m | 0.70 × m_area | 1.00 × m_area | 1.45 × m_area | linear in scale | more talkers fill gaps [R]; magnitudes [E] |
| Min active | max(1, round(0.4·m)) | max(2, round(0.6·m)) | max(3, round(0.75·m)) | step (recomputed) | [E] |
| Max active | round(1.5·m) | round(1.35·m) | round(1.2·m) | step | narrower spread when dense [E] |
| Max internal gap (pause shortening) | 600 ms | 250 ms | 100 ms | log | 100 ms from Rosen et al. dense construction [R]; others [E] |
| Segment duration median | 7.0 s | 5.5 s | 4.5 s | log | [E] |
| Segment duration σ_ln | 0.55 | 0.45 | 0.35 | linear | [E] |
| Per-segment level σ (dB) | 3.0 | 2.0 | 1.0 | linear | brief ±1.5…±4 dB [E] |
| Level-var truncation | ±2σ | ±2σ | ±2σ | — | [I] |
| Re-entry cooldown (slot) | 1.5 s | 0.8 s | 0.3 s | log | [E] |
| Overlap on handover (min) | 0 ms | 150 ms | 400 ms | linear | [E] |
| Stationary fraction offset Δs (added to 1−b) | −0.10 | 0.00 | +0.15 | linear | "Dense = higher stationary component" (brief) [E] |
| Spatial motion rate | 1.0 × area | 0.6 × area | 0.3 × area | linear | [E] |
| Fade in / fade out | 250 / 400 ms | 150 / 250 ms | 80 / 150 ms | log | [E] |

**Area anchoring:** Area values apply at the Area's default Character c_a (`macros.character`); anchors scale relative to c_a (revised during implementation — verify consistency). The mean is m_area · f_mean(c)/f_mean(c_a), and the stationary offset applied is Δs(c) − Δs(c_a), so an Area at its default macros reproduces its own mean/min/max/pool/b exactly.

**Area-specified bounds:** when an Area model gives explicit min/max (`PRESETS.md` §3), those values apply at the Area's default Character c_a. At other c, they are scaled by the ratio of the anchor formulas: min(c) = area.min · f_min(c)/f_min(c_a) and max(c) = area.max · f_max(c)/f_max(c_a), both rounded and then clamped so that min ≤ m ≤ max. The formula rows above are used only when the Area gives no explicit bounds.

The stationary offset applies only to strategies that expose Character, and only when the strategy's mix is not user-locked (Hybrid with an explicit mix slider ignores Δs). The final value is clamped to [0, 0.9] for NaturalBabbleMask and [0, 1] otherwise.

### 5.2 Level stability

Changes in m, gain variance and stationary fraction all alter the babble bus statistics. Stability is guaranteed by:
1. recomputing feed-forward g_bnorm = 1/√E[k_speech] with each new plan and ramping it over 2 s
2. the energy-preserving mixer
3. the slow trim, which freezes for 10 s and then resumes

The requirement is A-LV-5 (±1 dB in 10 s windows during a 60 s sweep).

### 5.3 Macro-to-plan application latency

Macro changes are debounced for 150 ms (UI drag), then a new plan is issued. The planner re-plans events that start later than `now + 0.5 s` (`TALKER_ENGINE.md` §4.6). The perceived response is therefore 0.5–2.5 s, which is intentional: no abrupt texture jumps.

---

## 6. Clear Voice Reduction (CVR)

**Goal:** reduce the probability that a single phrase becomes individually prominent and intelligible. There is no destructive processing: no pitch shift, reversal, spectral smearing or vocoding [R: such manipulations change the masker's temporal and linguistic structure away from the studied paradigm, and are unnecessary].

**Mechanism:** the conditions that make a single masker talker salient are:
1. being the only speaking talker (a solo)
2. being much louder than the others (dominance)
3. starting in a silence gap (onset salience)
4. long uninterrupted clean phrases

CVR r ∈ [0, 1] (GUI Low = 0.0, Medium = 0.5, High = 1.0; default High) acts on exactly these conditions.

### 6.1 Metric

`soloExposurePct` = percentage of 10 ms frames in which exactly one talker is speaking (per VAD metadata) **or** one talker's instantaneous level exceeds the power sum of all others by ≥ 6 dB. This is computed from ground-truth planner/VAD data, not estimated.

### 6.2 Mapping [E]

| Engine parameter | r = 0 (Low) | r = 0.5 (Medium) | r = 1 (High) | Curve |
|---|---|---|---|---|
| Min active floor (applied as max(minFromCharacter, floor)) | 1 | 2 | 3 | step at r ≥ 0.33 → 2, r ≥ 0.66 → 3 |
| Dominance cap: per-talker gain limited so that no talker exceeds the mean active level by more than | +6 dB | +4 dB | +2.5 dB | linear |
| Level σ multiplier | 1.0 | 0.8 | 0.6 | linear |
| Forced overlap at handover (new talker starts before the old one ends) | 0 ms | max(150 ms, Character value) | max(400 ms, Character value) | linear |
| Onset masking: a new talker may not start when the current speaking count is 0 unless another start is scheduled within | no rule | 300 ms | 100 ms (paired starts) | step |
| Segment selection weight for "solo-risk" segments (segments whose speech fraction > 0.9 and duration of longest continuous phrase > 4 s) | 1.0 | 0.6 | 0.3 | linear |
| Minimum stationary fraction (only if strategy allows a stationary component and mix is not user-locked) | 0.00 | 0.05 | 0.10 | linear |
| Max phrase continuity (forced fade-out point if a talker's continuous speech > X s with fewer than 2 others speaking) | off | 10 s | 6 s | log |

**Constraints:**
- CVR never raises mean active talkers. The talker count belongs to Voice Amount / Character, so that CVR stays orthogonal.
- CVR raises the min floor only if min ≤ mean − 1 remains satisfiable; otherwise the floor is lowered and the conflict is reported in `ValidationResult`.
- In `MultiVoiceMask`, only the dominance cap, level σ and selection weighting apply, because the count is fixed.
- In `LaboratoryMask`, CVR is disabled (all values as r = 0) unless explicitly configured.

**Expected effect (to be verified):** soloExposurePct at High < 1 % for m ≥ 4; at Low it is unconstrained.

---

## 7. Strategy switching and A/B comparison

| Step | Action |
|---|---|
| 1 | Control thread builds plan B, validates it, and builds new FIR kernels (asynchronously; ≤ 200 ms typical) |
| 2 | Plan B is posted to the RT thread via an RCU pointer swap |
| 3 | If the talker models differ, the planner starts a new epoch: talkers of plan A finish with their own fade-outs within 1.5 s after the 0.5 s freeze window (per-event cut points spread uniformly over the span), and plan B's slots start from its stationary on/off state, the on slots fading in over the same span (mid-segment, taking over from the slot's finishing plan-A talker where there is one). Each talker keeps the count normalization of the plan that scheduled it, so the expected babble power stays at L_ref through the hand-over (revised during implementation, `IMPLEMENTATION_NOTES.md` §12) |
| 4 | Mix fractions ramp linearly in b over 2 s (constant-power law, §4) |
| 5 | FIR kernels are crossfaded (two convolvers per channel active during 100 ms, equal-power) |
| 6 | Babble trim freezes for 10 s |

A/B compare (GUI `SPEC.md` §14) holds two plans. Toggling performs the same switch with a 1.0 s mix ramp. Because every plan is normalized to L_ref, **no loudness matching step is required** (A-LV-4).

---

## 8. Laboratory / Research mode (`LaboratoryMask`)

Purpose: reproduce controlled masker configurations for validation and research.

### 8.1 Sub-modes

| Sub-mode | Description |
|---|---|
| `continuousN` | Exactly N talkers continuously speaking, N ∈ {1, 2, 3, 4, 5, 7, 8, 12, 16, 24, custom 1–64}. Each talker is a distinct speaker, and within-talker pauses > `maxGapMs` (default 100 ms) are shortened. Each talker is normalized to equal active speech level, and consecutive segments of the same talker are concatenated with 20 ms equal-power crossfades [R: Rosen et al. 2013 construction] |
| `stochastic` | The normal planner with explicit, locked parameters |
| `ssn` | Speech-shaped stationary reference (target selectable) |
| `pink` | Pink noise reference: target curve = −3.01 dB/oct from 100 Hz to 10 kHz via the same FIR designer (not a Voss–McCartney generator), for spectral accuracy |
| `hybrid` | Explicit b ∈ {0, 0.25, 0.5, 0.75, 1} or custom |
| `ltassMatchedBabble` | `continuousN` with babble EQ strictly matched to the target (correction loop "Strict": converged offline in a pre-roll pass, then frozen) |

### 8.2 Guarantees

| Property | Mechanism |
|---|---|
| Same aggregate RMS across N | Offline: two-pass render. Pass 1 renders and measures the whole duration; pass 2 applies a single static gain to hit L_ref exactly (±0.01 dB). Real time: count normalization + trim |
| Same target spectrum | Babble static EQ from the actual N-talker pool's LTASS, plus an offline pre-roll correction pass (120 s) whose converged correction is frozen for the render |
| Same duration | Sample-exact from the scenario |
| Seed | Mandatory. The seed and corpus version are stored in the sidecar |
| Speaker sets | For a series (1, 2, 4, 8, 16), the speaker set for N is a prefix of the set for the next larger N (nested sets), drawn from the `selector` stream. This minimizes speaker-identity confounds [E, recommended practice] |
| No fallback | Fallback policy is forced to Strict: any source failure aborts the render with a non-zero exit code |
| Limiter | Disabled in offline research renders (float WAV, no ceiling) unless requested. A true-peak value is reported instead |

### 8.3 Research experiment matrix

`bfrender --matrix experiment.json` renders the Cartesian product of the listed parameter axes (talkers × mask type × spectrum × SNR when target speech is supplied × seed). It writes one WAV and one JSON sidecar per cell plus `matrix_manifest.json`. The format is in `VALIDATION.md` §2.

---

## 9. Voice Amount macro (GUI "Few ↔ Many")

v ∈ [0, 1] maps to the area's mean active talkers via log-interpolation between three anchors [E]:

| v | Label | Mean active (m) |
|---|---|---|
| 0.0 | Few | 3.5 |
| 0.5 | Medium | m_area (e.g. 6.5 Office) |
| 1.0 | Many | 16 |

```
m(v) = exp( lerp(ln 3.5, ln m_area, 2v) )          for v ≤ 0.5
m(v) = exp( lerp(ln m_area, ln 16,  2v − 1) )      for v > 0.5
```

- Pool = max(ceil(1.8·m) + 2, area pool).
- Min and max are then derived from Character (§5.1).
- The Voice Amount result replaces m_area as the base for the Character scaling. The final m is clamped to [1, 32], and to the number of speakers available in the corpus minus 2.

---

## 10. Strategy parameter ranges (Advanced)

| Parameter | Range | Default | Applies |
|---|---|---|---|
| Talker pool | 2–64 | area | babble |
| Mean active | 1.0–32.0 (step 0.1) | area | babble |
| Min active | 0–mean | derived | babble |
| Max active | ceil(mean)–48 | derived | babble |
| Multi-Voice K | 1–16 | 7 | MultiVoice |
| Babble fraction b | 0–1 (step 0.01) | per strategy | Hybrid, Lab |
| Max internal gap | 50–1000 ms | from Character | babble |
| Segment length min / max | 1–20 s / 3–60 s (min < max) | 2 s / 15 s | babble |
| Gain variation σ | 0–6 dB | from Character | babble |
| Fade time | 20–1000 ms | from Character | babble |
| Character c | 0–1 | area | Hybrid(Balanced), Babble, Natural |
| CVR r | 0–1 | 1.0 | babble |
| Voice Amount v | 0–1 | 0.5 | babble |
| Voice Diversity | Low / Balanced / High / Matched | High | babble |

Invariants enforced by `validate()` (silently clamped with an adjustment record, never rejected):
- min ≤ mean ≤ max ≤ pool
- pool ≤ available speakers
- segMin < segMax
- fadeIn + fadeOut < segMin
