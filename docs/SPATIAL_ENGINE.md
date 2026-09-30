# BabbleForge V1 — Spatial Engine and Output Matrix

Covers brief sections 25, 26, 27, 28. Labels are defined in `ENGINE.md` §0.1.

---

## 1. Design principle

> **Distributed acoustic coverage without highly separable masker locations.**

| Evidence | Consequence | Label |
|---|---|---|
| Spatial release from masking: target–masker spatial separation improves target recognition | Do not create a few conspicuous, localized masker sources; avoid hard-panned voices | [R] |
| Renz et al. loudspeaker-location study: masker presented from a different direction than the distracting speech reduced masking efficiency | Prefer a masker that is diffuse and present "everywhere", including near talkers' locations | [R] |
| Zarei et al.: more masking loudspeakers improve spatial uniformity; 1/3-oct uniformity is harder than broadband | Multichannel is fundamental; many low-level sources are better than few loud ones | [R] |
| Coherent signals on multiple loudspeakers create position-dependent comb filtering and a phantom image | Keep inter-channel correlation low; prefer *independent content* over delay tricks | [R, acoustics] |

The spatial engine therefore optimizes for:
1. equal long-term energy per output
2. low inter-output correlation
3. soft (never hard) talker placement
4. slow motion only

It does not attempt immersive or binaural rendering. HRTF processing is out of scope for V1.

---

## 2. Layout model

```cpp
struct OutputDef {
    uint8_t  index;            // logical output 0..N-1
    uint16_t deviceChannel;    // physical device channel (0-based); mapping may be sparse
    std::string label;         // "Front Left", "Ceiling 3" …
    float    azimuthDeg;       // for ring layouts (0 = front, + = left), −180..180
    float    elevationDeg;     // informational in V1
    std::optional<Vec2> posM;  // optional x,y metres (grid/distributed layouts)
    uint8_t  zone;             // 0..7
    bool     enabled;
};
struct OutputLayout { std::string id; LayoutKind kind; std::vector<OutputDef> outputs; };
enum class LayoutKind { Mono, Stereo, Ring, Grid };
```

| Preset layout | N | Kind | Positions |
|---|---|---|---|
| Mono | 1 | Mono | — |
| Stereo | 2 | Stereo | ±30° |
| 4 Channel | 4 | Ring | ±45°, ±135° |
| 6 Channel | 6 | Ring | ±30°, ±90°, ±150° |
| 8 Channel | 8 | Ring | ±22.5°, ±67.5°, ±112.5°, ±157.5° |
| Custom Ring | 3–16 | Ring | User azimuths, or equally spaced |
| Custom Grid | 3–32 | Grid | User x/y (ceiling or distributed arrays) |

The algorithm is chosen from the layout ("Automatic") or forced in Advanced:

| N and kind | Algorithm |
|---|---|
| 1 | §3.1 Mono |
| 2 | §3.2 Distributed Stereo |
| 3–8, Ring | §3.3 Small Multichannel (VBAP + MDAP spread) |
| > 8, or Grid of any N ≥ 3 | §3.4 Large Distributed (neighborhood rendering) |

---

## 3. Algorithms

### 3.1 Mono

All talkers sum with gain 1. The stationary component is one noise stream. No decorrelation. Diagnostics note: "Mono output: masker source is localized — consider ≥ 2 speakers" [R, spatial release].

### 3.2 Distributed Stereo

- **Pan law:** constant power. For pan position p ∈ [−1, 1]: θ = (p + 1)·π/4, g_L = cos θ, g_R = sin θ.
- **Width limit (no hard panning):** |p| ≤ p_max = 0.70 · spread (spread ∈ [0, 1], default 0.6 → p_max 0.42) [E]. At the absolute limit p = 0.70, the far channel is −12.6 dB relative to the near one. Hard left/right never occurs.
- **Placement at segment start:** p is drawn uniformly in [−p_max, p_max]. With probability 0.7 [E], the draw is reflected to the side whose babble energy over the last 10 s is lower (energy balancing).
- **Motion:** within a segment, p follows a slow bounded random walk. The target is re-drawn every 4–8 s (uniform) and approached with a maximum rate of 0.02·motion per second (motion ∈ [0, 1] from Area and Character). This is ≤ 0.02 pan units/s, which is inaudible as motion but avoids static images. No ping-pong: reversals are limited to one per target interval.
- **Decorrelation:** all-pass cascade per channel (§5) at the selected Speaker Variation level.
- **Stationary:** 2 independent noise streams.
- **Correlation targets (babble, broadband Pearson over 10 s, 95th percentile) [E]:** Low ≤ 0.8, Medium ≤ 0.5, High ≤ 0.3.

### 3.3 Small Multichannel (3–8 ring)

- **Virtual source:** each talker has an azimuth φ.
- **Gains:** 2D VBAP (Pulkki 1997) over adjacent speaker pairs [R], with **MDAP spread**. Five virtual directions φ + δ·{−1, −0.5, 0, 0.5, 1}, δ = spread·45°. The VBAP gains for each direction are summed, then power-normalized (Σ_c g_c² = 1).
- **Minimum spread rule:** every talker must feed ≥ 2 outputs, and the second-largest gain must be ≥ −9 dB relative to the largest [E]. If the MDAP result violates this (small spread), δ is increased until satisfied. This prevents point-source talkers.
- **Placement:** at segment start, φ is chosen as follows:
  - with probability 0.7, near the azimuth of the least-loaded output (lowest babble energy in the last 10 s), with ±15° uniform jitter
  - otherwise uniform over 360°
  This balances energy across outputs.
- **Motion:** φ drifts at ≤ 3°·motion per second toward re-drawn targets (every 5–10 s) [E].
- **Content independence:** because talkers are spread over subsets of speakers, non-adjacent outputs share little content. Adjacent outputs share the talkers between them.
- **Decorrelation:** all-pass per channel (§5).
- **Correlation targets:** as `ENGINE.md` A-SPA-2 (adjacent Medium ≤ 0.30, High ≤ 0.15).
- **Implementation:** SAF `saf_vbap` (ISC) may be used for gain-table generation on the control thread; alternatively, an in-house 2D VBAP (a closed-form pair inverse). Gain tables are precomputed at 1° resolution; RT reads a table and interpolates.

### 3.4 Large Distributed (neighborhood rendering)

For ceiling grids, large rooms, open offices and outdoor arrays, the goal is that **every listener position hears a locally independent babble of about the configured density**.

- **Neighborhood:** each output o has a neighborhood 𝒩(o) = {o} ∪ its k_n − 1 nearest outputs (k_n = 3 default, 2–5) [E]. Grid distance uses posM. For rings, the angular distance is used.
- **Talker assignment:** each talker has a home output h (the least-loaded output with probability 0.7, else uniform) and feeds 𝒩(h):
  - g_h = 0 dB
  - neighbors: g = −3 dB − 3 dB·(d/d_max,𝒩), i.e. −3…−6 dB
  - power-normalized
- **Density scaling:** the talker count parameters (m, min, max) are defined **per neighborhood**. The total simultaneous slots are V_total = ⌈max · N_active_outputs / k_n⌉, and the total mean is m_total = m · N / k_n. For example, 16 outputs, m = 8, k_n = 3 → m_total ≈ 43, V_total = 54. V_total is capped at 96 [I]. If capped, m is reduced proportionally and `spatial.densityCapped` is reported.
- **Pool:** P_total = max(P, V_total + 8), limited by the corpus (see `RELIABILITY.md` §3).
- **Motion:** "migration": a talker may move its home to a neighbor by a 3 s equal-power gain crossfade, at most once per segment, with probability 0.3·motion per segment [E].
- **Correlation:** outputs with disjoint neighborhoods share no talkers (ρ ≈ 0). Adjacent outputs share about 1/k_n of their talkers. Target: adjacent ≤ 0.30 (Medium) before all-pass, ≤ 0.15 (High) with all-pass.
- **Zones:** assignment never crosses zone boundaries. A talker's neighborhood is truncated to its home zone.

---

## 4. Per-channel babble balance

Different placements produce unequal per-channel babble power. Balancing happens in two ways:
1. **Feed-forward:** the expected power per channel is E_c = Σ_v a_v·g_vc², where a_v is the slot's planned activity probability. Per-channel normalization is g_bnorm,c = 1/√E_c. It is computed per plan and recomputed every 1 s from the actual planned events (smoothed, τ = 10 s).
2. **Feedback:** a per-channel trim toward equal T1 power per channel (τ = 60 s, ±3 dB, slew 0.25 dB/s). It is frozen under the same conditions as the babble level trim (`ENGINE.md` §3.2).

Target: long-term per-channel babble RMS within ±1.0 dB (A-SPA-3). The stationary component is inherently balanced.

---

## 5. Channel decorrelation ("Speaker Variation")

### 5.1 Method evaluation

| Method | Decorrelation | Artefacts | CPU | Decision |
|---|---|---|---|---|
| **Independent content** (talker subsets, independent source offsets, independent noise) | Complete where content differs | None | None | **Primary method** in all modes |
| Random micro-delays (0.5–20 ms) | Only above ~1/delay; LF stays correlated | Strong, stable comb filtering at positions where two speakers are equally loud; a fixed notch pattern is audible | Negligible | **Rejected** as a decorrelator (delay is allowed only for alignment in the output matrix) |
| All-pass cascade (Schroeder / lattice) | Good above ~300 Hz; poor at LF | Mild transient smearing if the group delay is long; slight "phasiness" | Low | **Secondary method** for shared (panned) talker content |
| Velvet-noise decorrelators (Alary, Politis, Välimäki 2017) | Good | Slight coloration, transient smearing ~20–30 ms | Low (sparse FIR) | Alternative; to be evaluated in validation |
| Diffuse-field / reverb-based (SAF reverb, FDN) | Very good | Adds audible reverberance, which changes the masker's temporal structure | Medium | Rejected for V1 (alters the envelope under study) |
| SAF lattice all-pass decorrelator (frequency-dependent orders) | Good, band-controlled | Low | Low–medium | Reference for tuning the all-pass design; may be used directly (ISC) |

### 5.2 Specified all-pass decorrelator

Applied per output channel to the **babble** bus only, after the babble shaper:

```text
y = A_S(…A_2(A_1(x)))    S stages of Schroeder all-pass:
A_i(z) = (−g_i + z^{−D_i}) / (1 − g_i z^{−D_i})
```

| Level (GUI) | Stages S | D_i range | g_i | Content policy |
|---|---|---|---|---|
| Low | 0 (bypass) | — | — | Independent noise, normal talker spread |
| Medium (default) | 3 | 0.4–3.0 ms | 0.45–0.60, alternating sign per channel | + talker placement balancing |
| High | 5 | 0.4–4.5 ms | 0.45–0.60 | + stereo p_max × 1.1 (≤ 0.7), MDAP δ × 1.2, k_n − 1 shared neighbors reduced by one where N allows |

- **Delay selection [I]:** delays in samples are distinct primes, unique per (channel, stage). They are drawn deterministically from stream `decorrelator.ch.<c>` without replacement from the primes in range. They scale with fs (the time values are fixed).
- **Constraints:**
  - the energy decay of each cascade must reach −60 dB within 25 ms (checked at design; otherwise g is reduced)
  - magnitude response flat by construction
- **Why this is acceptable for speech:** the impulse response is short (< 25 ms to −60 dB), below the echo threshold and comparable to early reflections in small rooms. Validation must confirm there is no audible "phasiness" (Open Research Question 9).

### 5.3 Correlation measurement

- Pairwise Pearson ρ on T4 (and T1 for babble-only) is computed on the analysis thread for adjacent pairs (all pairs if N ≤ 8) over 10 s windows, broadband and in 3 bands (< 500 Hz, 500–2000 Hz, > 2 kHz).
- Published: max adjacent ρ, median ρ, and the per-pair matrix (Advanced/Diagnostics).
- GUI summary: "Low / Good" if max adjacent broadband ρ ≤ the level's target.

---

## 6. Zones

| Property | Range / default | V1 behavior |
|---|---|---|
| Zone count | 1–8; default 1 | — |
| Membership | Each output belongs to exactly one zone | — |
| Enable | on/off | Off = zone outputs muted (ramped 200 ms) |
| Relative level | −24 … +6 dB; 0 | Applied in the output matrix |
| Babble-fraction offset | −0.5 … +0.5; 0 | The per-zone hybrid mix (cheap, because both buses are already per channel). This is how the GUI's per-zone "Masking Style" is realized in V1 |
| Limiter link group | Zone | `ENGINE.md` §6 |
| Talker neighborhood boundary | Zone | Talkers never span zones |
| Separate strategy/engine per zone | **Not in V1** | Reserved: `ZoneEngineBinding` (V2) |

---

## 7. Output matrix

### 7.1 Bus model

```text
Source buses (N ch each):   BABBLE   STATIONARY   CALIBRATION(reserved)
          │ hybrid mixer per zone │               │
          ▼                                        │
       MASKER bus [N] ──► Master gain ──► Source select (MASKER|CALIBRATION|MUTE)
                                                   ▼
Zone stage:    zone gain · zone enable (per zone, applied to member outputs)
                                                   ▼
Output stage (per logical output o):
    gain_o (−40 … +6 dB, 0) · mute_o · polarity_o (±1)
    → delay_o (0 … 100 ms, integer samples; preallocated 100 ms line)
    → CalEQ_o slot (FIR/biquad slot; identity in V1; V2 per-speaker EQ)
    → limiter (zone-linked) → safety clip
Physical map:   logical output o → deviceChannel(o); unmapped device channels receive silence
```

### 7.2 Matrix data

```json
"outputs": {
  "layout": "ring8",
  "algorithm": "auto",
  "channels": [
    { "index": 0, "deviceChannel": 0, "label": "Ceiling 1", "azimuthDeg": 22.5,
      "zone": 0, "enabled": true, "gainDb": 0.0, "mute": false,
      "polarityInvert": false, "delayMs": 0.0, "calEqProfile": null }
  ],
  "zones": [ { "id": 0, "name": "Zone A", "enabled": true, "levelDb": 0.0, "babbleFractionOffset": 0.0 } ]
}
```

- **Parameter changes:** gains are smoothed (20 ms); mute uses a 20 ms ramp; delay changes cause a 20 ms fade-out, jump, then fade-in; polarity changes use the same fade.
- **Rule:** the matrix is applied **after** all masker generation. It never influences talker planning, except that zone membership constrains talker neighborhoods.

### 7.3 Integration with V2 room calibration

V2 writes only to the output stage and the zone stage:
- `gainDb`, `delayMs`, `polarityInvert`
- `calEqProfile` (per-speaker EQ FIR, ≤ 2048 taps, designed by the V2 optimizer)
- the zone-level target via `ILevelCalibration`

Because these stages already exist and are identity in V1, V2 needs no change to the masker, spatial renderer or strategies. Calibration EQ is applied per physical output and is **not** seen by the spectral correction loop, which measures T1/T3 (pre-matrix). V2's acoustic loop measures the room. The two loops are decoupled by design (`V2_EXTENSION_POINTS.md` §4).

---

## 8. Spatial parameters summary

| Parameter | Range | Default source | Label |
|---|---|---|---|
| Spread | 0–1 | Area model | [E] |
| Motion | 0–1 | Area model × Character | [E] |
| Speaker Variation | Low / Medium / High | Area model | [E] |
| Neighborhood size k_n | 2–5 | 3 | [E] |
| Placement balancing probability | 0–1 | 0.7 | [E] |
| Stereo far-channel floor | ≥ −12.6 dB (derived) | — | [E] |
| Small-MC second-gain floor | −9 dB | — | [E] |
| Max talker slots (distributed) | ≤ 96 | — | [I] |
