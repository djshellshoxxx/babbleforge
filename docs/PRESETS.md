# BabbleForge V1 — Presets, Area Models and Configuration Format

Covers brief sections 31–39, 42, 61, 62. Labels are defined in `ENGINE.md` §0.1.

---

## 1. Two-dimensional model: Area × Strategy

| Dimension | Answers | Examples |
|---|---|---|
| **Area Model** | *Where* is the masker played? It governs physical/perceptual context: output count, spatial algorithm, spread, decorrelation, level headroom, LF handling, baseline talker density | Small Room, Office, Conference, Open Office, Large Room, Common Area, Reception, Free Field, Custom |
| **Mask Strategy** | *What kind* of masker? It governs the generator mix and temporal character | Balanced, Natural, Dense, Speech-Shaped Stationary, Multi-Voice, Hybrid, Research |

**Why two dimensions beat one fixed preset list:**
1. **Science:** the evidence varies along the strategy axis (SSN vs babble vs multi-voice) independently of the room. A one-dimensional list would force an untested pairing ("Open Office = babble") and block the comparisons validation requires.
2. **Combinatorics:** 9 areas × 7 strategies = 63 valid configurations from 16 data objects, instead of 63 hand-maintained presets that drift apart.
3. **Tuning:** experimental results usually update one axis (for example, "stationary works better in open offices" changes one Area field). Data tuning stays local.
4. **UI:** the GUI already separates Area and Mask Type (GUI §5–6).
5. **V2:** calibration refines the Area side (measured room, outputs, level) without touching strategies.

---

## 2. Composition and precedence

```text
EffectiveParams = apply in order (later overrides earlier):
  1. Engine built-in fallbacks          (data/engine_defaults.json)
  2. Area Model                         (data/areas/<area>.json)
  3. Strategy definition                (data/strategies/<strategy>.json) — overrides + forced ranges
  4. Macros: Voice Amount → Character → Clear Voice Reduction   (MASK_STRATEGIES.md §5, §6, §9)
  5. User advanced overrides            (preset "overrides" block)
  6. Validation/clamping                (MaskStrategy::validate) — records adjustments
  7. Degrade transformation             (fallback policy, runtime only)
```

- Each field in steps 2–5 may be absent, which means inherit.
- "Modified" state (GUI §46) = the preset contains any step-5 override, or macro values differ from the Area defaults.
- "Reset to Recommended" deletes step-5 overrides and resets macros to their Area defaults.

---

## 3. Area model defaults

**All values in this section are: Engineering starting defaults requiring validation [E].** They are informed by the research summarized in `docs/research/` (talker-count effects, SSN efficiency, LF content, spatial uniformity) but are not experimentally established optima for these areas.

### 3.1 Summary table

| Area | Talker range (recommended) | Default mean active m (Character 0.5) | Pool P | Min / Max | Stationary energy (1 − b) in Balanced | Spectrum | LF trim 125 Hz | Spread | Speaker Variation | Min outputs (recommended) | Activity density | Character default | Spatial algorithm |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Small Room | 3–6 | 4.5 | 10 | 3 / 6 | 0.30 | Universal LTASS | −3 dB | 0.5 | Medium | 1 (2) | Balanced | 0.45 | Auto |
| Office | 4–8 | 6.5 | 14 | 4 / 9 | 0.30 | Universal LTASS | −1.5 dB | 0.6 | Medium | 1 (2) | Balanced | 0.55 | Auto |
| Conference | 5–8 | 6.5 | 14 | 4 / 9 | 0.35 | Universal LTASS | −1.5 dB | 0.7 | High | 2 (4) | Balanced | 0.55 | Auto |
| Open Office | 7–12 | 8.5 per neighborhood | 20 | 6 / 11 | 0.45 | Universal LTASS | 0 dB | 0.85 | High | 2 (4+) | Dense-leaning | 0.65 | Distributed if ≥ 4 outputs |
| Large Room | 10–16 | 12 per neighborhood | 28 | 9 / 15 | 0.40 | Universal LTASS | 0 dB | 0.9 | High | 4 (8+) | Dense-leaning | 0.65 | Distributed; zones enabled |
| Common Area | 6–12 | 8 | 24 | 4 / 12 | 0.15 | Universal LTASS | 0 dB | 0.8 | Medium | 2 (4) | Natural | 0.25 | Auto; motion 0.8 |
| Reception | 5–10 | 7 | 20 | 3 / 10 | 0.20 | Universal LTASS | 0 dB | 0.7 | Medium | 2 (2–4) | Natural-leaning | 0.35 | Auto; motion 0.6 |
| Free Field | 10–20 | 12 per neighborhood | 32 | 9 / 15 | 0.40 | Universal LTASS | 0 dB | 1.0 | High | 4 (6+) | Dense | 0.70 | Distributed (Grid) |
| Custom | copy of Office | — | — | — | — | — | — | — | — | — | — | — | — |

Notes:
- "Min outputs" is the minimum at which the preset runs **without an advisory**. The bracketed value is the recommendation shown in the GUI advisory (GUI §56).
- The Speech-Matched spectrum replaces Universal LTASS automatically only if the user has set "Use speech-matched profile when available" and a profile is attached to the Area. Evidence favors a speech-matched spectrum [R: Renz 2018], but a mismatched profile would be worse, so this is opt-in.
- −5/−7/−9 dB/oct curves are never area defaults. They are available as explicit choices. The −5 dB/oct curve is historical practice and is not supported as uniquely optimal.

### 3.2 Common per-area fields

```json
{
  "schema": "babbleforge.area/1",
  "id": "office",
  "displayName": "Office",
  "description": "1–4 person offices",
  "evidenceLabel": "engineering-default-requires-validation",
  "talkers":   { "meanActive": 6.5, "pool": 14, "minActive": 4, "maxActive": 9,
                 "recommendedRange": [4, 8] },
  "mix":       { "balancedStationaryFraction": 0.30 },
  "macros":    { "character": 0.55, "voiceAmount": 0.5, "clearVoiceReduction": 1.0,
                 "voiceDiversity": "high" },
  "spectrum":  { "target": "ltass_universal", "lfLimitHz": 80, "lfTrimDb125": -1.5,
                 "useSpeechMatchedIfAvailable": false },
  "spatial":   { "algorithm": "auto", "spread": 0.6, "motion": 0.3,
                 "speakerVariation": "medium", "neighborhoodSize": 3 },
  "outputs":   { "minRecommended": 2, "minWithoutAdvisory": 1 },
  "level":     { "defaultStrengthDb": 0.0, "maxSimpleStrengthDb": 9.0 },
  "advisories":[ { "when": "outputs < 2", "text": "A single speaker makes the masker easy to localize. Two or more are recommended." } ]
}
```

---

## 4. Area rationales

### 4.1 Small Room (private office, small meeting room, 2–6 occupants)

| Concern | Default | Reasoning |
|---|---|---|
| Strong early reflections → an already diffuse, dense field | m = 4.5 (3–6), Character 0.45 | Reflections add effective density, so an excess of talkers is avoided [E]. |
| Few speakers needed | min outputs 1, recommended 2 | Small volume; reflections aid coverage |
| Avoid excessive density | max 6 | — |
| Avoid LF build-up (room modes, small-room bass gain) | LF trim −3 dB on the 100–160 Hz bands; LF limit stays 80 Hz | Keeps the 125 Hz band present (Renz: 125 Hz content matters) but reduces modal boom [E; conflicting considerations, see Open Research Question 7] |
| Stationary component | 0.30 | Fills dips that reflections do not fill [E] |
| Level | Strength default 0 dB | V1 cannot know SPL. The small-room V2 calibrated starting range is 39–42 dBA (`SPEC.md` §35) |

### 4.2 Office (1–4 occupants)

Moderate density (m = 6.5). The 7-voice multi-voice result [R: Keus van de Poll 2015, writing task] motivates staying near 6–7 simultaneous talkers, and Balanced hybrid provides energetic floor coverage. Stereo is the expected installation. LF trim −1.5 dB [E].

### 4.3 Conference Room

| Concern | Default |
|---|---|
| Table-centred speech; listeners around the table | Spread 0.7, Speaker Variation High. The masker should come from around the table, not from one wall (spatial coincidence with the talkers matters [R: Renz loudspeaker location]) |
| 2–4+ speakers | Recommended ≥ 4 outputs at room corners/ceiling; with 2, the stereo algorithm places talkers across the full ±p_max |
| Diffuse masking | Stationary 0.35; High decorrelation |
| Density | m = 6.5 (5–8) |

The masker is intended to mask speech leaking *out* (or between tables). In-room privacy against participants is not a V1 goal.

### 4.4 Open Office

| Priority | Default |
|---|---|
| Uniformity | Distributed rendering with ≥ 4 outputs; per-channel balance loop; High decorrelation [R: Zarei — more loudspeakers improve uniformity] |
| Multiple independent outputs | Neighborhood k_n = 3 → locally independent babble |
| Strong temporal coverage | Character 0.65 (shorter gaps); stationary 0.45 |
| Moderate stable masking | CVR High; level σ reduced by Character |

With only 2 outputs, it runs the stereo algorithm and shows the advisory "Open offices usually need 4+ speakers for even coverage."

### 4.5 Large Room (hall, large boardroom, event space)

Output count and zones dominate. Zones are enabled by default (one zone per 4 outputs when N ≥ 8, auto-grouped by position). Density is per neighborhood, m = 12. Stationary 0.40 for robust coverage. Advisory if N < 4: "Coverage will be uneven with fewer than 4 speakers."

### 4.6 Common Area

Naturalness is the priority:
- Character 0.25: longer gaps (≈ 450 ms max internal gap), level σ ≈ 2.5 dB, segment median ≈ 6.3 s
- stationary 0.15
- motion 0.8 (slow talker drift)
- m = 8 with a wide min/max span (4/12) so that activity varies noticeably

**This area prioritizes acceptability over maximum masking.** Validation should measure annoyance as well as intelligibility.

### 4.7 Reception

Like Common Area but smaller and closer to the listener: Character 0.35, stationary 0.20, m = 7, motion 0.6. CVR stays High because receptions are close to the public.

### 4.8 Free Field / Outdoor

A different acoustic model: there are no reflections to diffuse the masker, and the source localizes strongly.

| Priority | Default |
|---|---|
| Physical coverage | Grid layout, Distributed rendering, recommended ≥ 6 outputs; advisory under 4 |
| Multiple speakers | Neighborhood k_n = 3, High decorrelation |
| Broad distribution | Spread 1.0 |
| Source-localization reduction | Independent content per neighborhood; no single-speaker operation (warning at N = 1: "Outdoor masking with one speaker is not recommended") |
| Stable speech-band content | Stationary 0.40; Character 0.70; correction Normal |

- Room-compensation assumptions: none in V1. The V2 Free-Field model disables room EQ (`V2_EXTENSION_POINTS.md`).
- V1 cannot calibrate distance-based SPL. The defaults are digital only.

---

## 5. Strategy definitions (data)

```json
{
  "schema": "babbleforge.strategy/1",
  "id": "balanced",
  "class": "HybridMask",
  "displayName": "Balanced",
  "descriptionSimple": "General-purpose speech masking with a mix of voices and stable background masking.",
  "overrides": { },                                   // Balanced uses Area values unchanged
  "forced":    { "characterRange": [0.0, 1.0] },
  "evidenceLabel": "engineering-default-requires-validation"
}
```

| id | class | Overrides / forced |
|---|---|---|
| balanced | HybridMask | b = 1 − area.balancedStationaryFraction; Character from area |
| natural | NaturalBabbleMask | Character ≤ 0.35 (default 0.2); b = min(0.95, area b + 0.10); motion × 1.5 (≤ 1) |
| dense | BabbleMask | Character ≥ 0.75 (default 0.9); b = area b − 0.10 (≥ 0.4) |
| speech_noise | StationarySpeechMask | b = 0; spectrum from area (or user) |
| multi_voice | MultiVoiceMask | K = 7 default (options 3, 5, 7, 9 "Dense Multi-Voice"); b = 1.0 |
| hybrid | HybridMask | b = user (default 0.5); Character from area |
| research | LaboratoryMask | Explicit scenario; Strict fallback; seed required |

---

## 6. Preset file format

Format: **JSON (UTF-8)**, extension `.bfpreset`. Schema id `babbleforge.preset`, semantic version `major.minor`.

### 6.1 Rules

| Rule | Detail |
|---|---|
| Versioning | `schemaVersion` "1.0". Minor bumps add optional fields only. A major bump requires a migration function `migrate_vN_to_vN+1` |
| Forward compatibility | Unknown fields are **preserved** on load and re-save (round-trip), and ignored by the engine. A file with a higher **minor** version loads with a warning. A higher **major** version is rejected with "Created by a newer BabbleForge" |
| Extensions | Keys prefixed `x-` are reserved for experiments and tools, and are never interpreted by V1 |
| Validation | JSON Schema (draft 2020-12) shipped as `schemas/preset-1.0.schema.json`. Values out of range are clamped and recorded; type errors reject the file |
| Numbers | Units in key names (`Db`, `Ms`, `Hz`, `S`) |
| Device identity | Stored only if `rememberDevice: true` (GUI §47) |
| Integrity | `contentHash` = SHA-256 of the canonical JSON without the hash field; used for "Modified" detection and correction-memory keys |

### 6.2 Complete example

```json
{
  "schema": "babbleforge.preset",
  "schemaVersion": "1.0",
  "name": "Server Room Privacy",
  "basedOn": { "area": "open_office", "strategy": "hybrid", "factoryPresetVersion": "1.0.0" },
  "created": "2026-09-30T14:02:11Z",
  "appVersion": "1.0.0",
  "notes": "Hybrid 50/50 for comparison test B",

  "area": "open_office",
  "strategy": "hybrid",

  "macros": {
    "strengthDb": 0.0,
    "character": 0.65,
    "voiceAmount": 0.5,
    "clearVoiceReduction": 1.0,
    "voiceDiversity": "high",
    "mix": { "babbleFraction": 0.50 }
  },

  "talkers": {
    "pool": 20,
    "meanActive": 8.5,
    "minActive": 6,
    "maxActive": 11,
    "segmentMinS": 2.0,
    "segmentMaxS": 15.0,
    "segmentMedianS": null,
    "maxInternalGapMs": null,
    "gainVariationDb": null,
    "fadeInMs": null,
    "fadeOutMs": null,
    "reEntryCooldownS": null,
    "segmentCooldownMin": 45,
    "poolRotationMin": 20,
    "languageAware": false,
    "targetVoiceProfile": null,
    "multiVoiceK": null
  },

  "stationary": {
    "enabled": true,
    "spectrum": "ltass_universal",
    "seedMode": "session"
  },

  "spectrum": {
    "target": "ltass_universal",
    "speechMatchedProfile": null,
    "custom": null,
    "lfLimitHz": 80,
    "hfLimitHz": 12500,
    "lfTrimDb125": 0.0,
    "correction": { "enabled": true, "speed": "normal", "maxDb": 6.0, "rememberCorrection": true }
  },

  "spatial": {
    "algorithm": "auto",
    "spread": 0.85,
    "motion": 0.3,
    "speakerVariation": "high",
    "neighborhoodSize": 3
  },

  "outputs": {
    "rememberDevice": false,
    "device": null,
    "layout": "ring4",
    "channels": [
      { "index": 0, "deviceChannel": 0, "label": "Ceiling NW", "azimuthDeg": 45,   "zone": 0, "enabled": true, "gainDb": 0.0, "mute": false, "polarityInvert": false, "delayMs": 0.0, "calEqProfile": null },
      { "index": 1, "deviceChannel": 1, "label": "Ceiling NE", "azimuthDeg": -45,  "zone": 0, "enabled": true, "gainDb": 0.0, "mute": false, "polarityInvert": false, "delayMs": 0.0, "calEqProfile": null },
      { "index": 2, "deviceChannel": 2, "label": "Ceiling SW", "azimuthDeg": 135,  "zone": 0, "enabled": true, "gainDb": -1.5, "mute": false, "polarityInvert": false, "delayMs": 0.0, "calEqProfile": null },
      { "index": 3, "deviceChannel": 3, "label": "Ceiling SE", "azimuthDeg": -135, "zone": 0, "enabled": true, "gainDb": 0.0, "mute": false, "polarityInvert": false, "delayMs": 0.0, "calEqProfile": null }
    ],
    "zones": [ { "id": 0, "name": "Main", "enabled": true, "levelDb": 0.0, "babbleFractionOffset": 0.0 } ],
    "limiter": { "enabled": true, "ceilingDbtp": -1.0 }
  },

  "random": {
    "seed": null,
    "deterministic": false
  },

  "reliability": { "fallbackPolicy": "continuous" },

  "overrides": { },

  "contentHash": "sha256:…",
  "x-experiment": { "condition": "B" }
}
```

`null` means "inherit from Area/Strategy/macros".

### 6.3 Research scenario (deterministic render) extension

```json
{
  "schema": "babbleforge.scenario",
  "schemaVersion": "1.0",
  "preset": { "...": "inline preset or path" },
  "corpusVersion": "3f9a1c0d77e24b1a",
  "seed": 182731,
  "durationS": 300,
  "sampleRate": 48000,
  "outputFormat": { "container": "wav", "sampleFormat": "float32" },
  "events": [ { "atS": 120.0, "set": { "macros.character": 0.9 } } ],
  "laboratory": { "mode": "continuousN", "talkers": 8, "maxGapMs": 100,
                  "rmsNormalization": "two-pass", "spectrumMatch": "strict" }
}
```

---

## 7. Factory preset storage

```text
resources/data/
├── engine_defaults.json
├── areas/{small_room,office,conference,open_office,large_room,common_area,reception,free_field}.json
├── strategies/{balanced,natural,dense,speech_noise,multi_voice,hybrid,research}.json
├── macros/{character_anchors,cvr_mapping,voice_amount}.json
├── targets/{ltass_universal_byrne1994,slope_-5,slope_-7,slope_-9,pink,flat}.json
└── schemas/*.schema.json
```

---

## 8. Data-driven tuning (no recompilation)

These values must come from data files, never code constants:

| Group | Values |
|---|---|
| Talker counts | pool, mean, min, max per area; count-mode table (`TALKER_ENGINE.md` §5); Voice Amount anchors |
| Temporal | Character anchor table (all rows), cooldowns, rotation interval, overlap values, anti-synchrony spacing |
| Levels | Gain variation σ, dominance caps, L_ref, Strength labels/range |
| Spectrum | Target curves, LF limit/trim, correction speeds/limits/deadband |
| Mix | Stationary fractions per area, strategy offsets, Character Δs |
| Spatial | Spread, motion, p_max factor, MDAP δ, second-gain floor, k_n, decorrelator stage tables |
| CVR | Entire mapping table |
| Analysis thresholds | Gap threshold (−12 dB), density class bounds |
| Alarms | Limiter alarm thresholds, starvation rates |

- Only the physical/structural constants marked [I] (block sizes, FFT sizes, buffer sizes) may be compiled in.
- A developer override directory (`--data-dir`) allows experiments to load alternative data sets. The active data-set hash is logged at every start and stored in render sidecars.

---

## 9. Configuration persistence

| File | Content | Write policy |
|---|---|---|
| `settings.json` | App settings (Simple/Advanced, startup, device, corpus root, logging, redaction) | On change (debounced 2 s), atomic write (temp + rename) |
| `session.json` | Current effective preset (+ Modified overrides), macros | Every 30 s if changed, and on stop/exit |
| `last_known_good.json` | The last configuration that reached RUNNING and stayed there ≥ 60 s with no ERROR | On promotion |
| `user_presets/*.bfpreset` | User presets | Explicit save |
| `correction_memory.json` | Spectral correction per (preset hash, corpus version, fs) | Every 10 min while running and on stop |
| `selector_state.json` | Persistent shuffle | Every 60 s and on stop |

All writes are atomic. Every file has a `.bak` of its previous version.

---

## 10. First-launch default configuration

See `ENGINE.md` §8. In preset terms: Area `office`, Strategy `balanced`, Strength 0 dB, Character 0.55, Voice Amount 0.5, CVR 1.0, Diversity High, Universal LTASS, stereo, Speaker Variation Medium, limiter −1 dBTP, fallback Continuous, random session seed.
