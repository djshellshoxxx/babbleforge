# BabbleForge V1 — Implementation Notes

Record of implementation decisions made during V1 development that revise the specification. Each item lists the spec file/section and the decision.

---

## 1. ENGINE.md §12 A-SPA-1: Stationary channel independence criterion

**File/Area:** `ENGINE.md` §12 A-SPA-1

**Revised specification:** Stationary decorrelation acceptance criterion updated to:
- |ρ| over 60 s < 0.02
- median of 10 s windows < 0.02
- 95th percentile < 0.03

Independent noise has ±0.01 SD per 10 s window.

---

## 2. SPATIAL_ENGINE.md §5.2 and §3.2: All-pass gains and far-channel floor

**File/Area:** `SPATIAL_ENGINE.md` §5.2 (decorrelator constraints) and §3.2 (stereo pan law)

**Revised specifications:**
- All-pass gains are reduced automatically below 0.45–0.60 where needed to meet the −60 dB-within-25 ms decay rule
- High level decorrelation ends up |g| ≈ 0.1–0.3 at 48 kHz
- Adjacent channels use disjoint prime-delay sets
- IR correlation with previous channel ≤ 0.1
- Far-channel floor at p = 0.70 is −12.4 dB (exact constant-power value), not −12.6 dB

---

## 3. ENGINE.md §6: Limiter total latency and detector delay

**File/Area:** `ENGINE.md` §6 (Limiter)

**Revised specification:** Total latency exactly 5 ms (240 samples @48 kHz)
- Detector adds 24 samples of intrinsic delay
- Smoother = lookahead − detector delay = 240 − 24 = 216 samples = 4.5 ms

---

## 4. SPECTRUM_ENGINE.md §8.3 and §6: Modulation bands and slew rate

**File/Area:** `SPECTRUM_ENGINE.md` §8.3 (modulation spectrum) and §6 (spectral correction)

**Revised specifications:**
- 16 modulation bands (0.5…16 Hz): 0.5, 0.63, 0.8, 1, 1.25, 1.6, 2, 2.5, 3.15, 4, 5, 6.3, 8, 10, 12.5, 16
- Band value = sqrt(Σm²/ENBW)
- Strict-mode slew rate: 3 dB/min
- Known limitation: 1-2-1 band smoothing cannot remove single-band errors (residual up to ~0.9 dB, within acceptance ±2 dB)

---

## 5. SPECTRUM_ENGINE.md §4.3: FirDesigner input specification

**File/Area:** `SPECTRUM_ENGINE.md` §4.3 (filter design procedure)

**Revised specification:** FirDesigner input is a spectral-density-equivalent band-level target. White noise input yields the given band levels. Callers converting per-band gains must add the bandwidth term.

---

## 6. TALKER_ENGINE.md §4.5 and §3.2/ENGINE §3.2: End-cluster guard and g_bnorm

**File/Area:** `TALKER_ENGINE.md` §4.5 (enforcing min/max) and §3.2; `ENGINE.md` §3.2

**Revised specifications:**
- End-cluster guard: when ≤ V − min ends occur in any cooldown+40 ms window, prevent gap
- Forced-start overlap floor is 120 ms
- g_bnorm uses E[k_speech] measured from an 1800 s probe run of the planner (includes level-variation and fade losses)
- Slow trim is required (feed-forward alone gave +0.5 dB windows)

---

## 7. MASK_STRATEGIES.md: Area default Character and planId

**File/Area:** `MASK_STRATEGIES.md` §5.1 (area anchoring) and `ENGINE.md` (MaskRenderPlan)

**Revised specifications:**
- Area values apply at the Area's default Character (verify consistency)
- planId excludes the talker seed

---

## 8. CORPUS.md §1.3 and §3.12: Clipping detection and cache handling

**File/Area:** `CORPUS.md` §1.3 (audio quality requirements) and §3.12 (quality analysis)

**Revised specifications:**
- Clipping detection uses flat-top rule: consecutive samples equal within 2^-17
  - Note: low-frequency (<~20 Hz) sines may still false-trigger
- Previous cache kept as cache.old-<version>, purged next import
- Effective bandwidth measured on 1/3-oct band levels
- Not yet implemented: music/non-speech flag, overlap-based anchor exclusion, ECAPA check

---

## 9. CORPUS.md / BUILDING: SQLite options

**File/Area:** `CORPUS.md` §7 (new section on build options)

**Revised specification:** SQLite is the corpus database backend
- Fetched from sqlite.org as single-file amalgamation
- Option `BF_SQLITE_USE_SYSTEM=ON` uses system library instead
- Compiled with `BF_WITH_CORPUS_DB` toggle

---

## 10. ENGINE.md §9: JUCE device layer and license decision

**File/Area:** `ENGINE.md` §9 (open-source references)

**Revised specification:** JUCE is chosen for device layer and GUI (AGPLv3 or commercial licence)
- Licence decision pending with the project owner

---

## 11. SPECTRUM_ENGINE.md §8.3 / VALIDATION.md §5: stationary-noise modulation floor

**File/Area:** `SPECTRUM_ENGINE.md` §8.3 (Interpretation), `VALIDATION.md` §5 (expected trends)

**Revised specification:** "SSN modulation is at the noise floor" is quantified: stationary noise shows m(f) at the statistical floor of the analysis (≈ < 0.05 for bands ≤ 4 Hz; rises to ≈ 0.1–0.15 at 8–16 Hz for 20 s blocks).
- The floor is statistical (finite 20 s blocks of a random envelope), not a defect of the analyzer; tests therefore bound m(f) per band rather than expecting zero.

---
