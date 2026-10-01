# BabbleForge V1 — V2 Extension Points

Covers brief sections 55 and 56. Labels are defined in `ENGINE.md` §0.1.

**Principle:** V2 adds measurement, calibration and adaptation **around** the V1 masker. It never replaces the talker engine, stationary engine, hybrid mixer or strategies. Every V2 feature connects through an interface or graph slot that already exists in V1 (null or identity implementation).

---

## 1. Reserved interfaces

All interfaces live in `Source/Engine/Extensions/` and are compiled into V1 with null implementations. They are called only on non-RT threads, unless marked RT.

| Interface | V1 implementation | V2 purpose | Consumed by |
|---|---|---|---|
| `IMeasurementInput` | `NullMeasurementInput` (no device) | Measurement microphone capture: device, channel, calibration file, mic class A/B/C | Measurement subsystem |
| `IMicCalibration` | none | Sensitivity (dB SPL per dBFS), frequency-response correction (1/3-oct or FIR), confidence class | Level + spectrum measurement |
| `IRoomResponse` | none | Per-output impulse responses and transfer functions per measurement location (ESS, Farina 2000) | Optimizer, STI indirect |
| `ISpeakerCalibration` | `IdentitySpeakerCalibration` | Per-output gain, delay, polarity, EQ | Output matrix (`calEqProfile`, gain, delay) |
| `ILevelCalibration` | `DigitalOnlyLevel` (returns "uncalibrated") | Mapping digital level → estimated SPL per output/zone/location; enables calibrated targets | Meters/UI labels, level controller |
| `IAmbientMonitor` | none | Continuous ambient level and spectrum at the microphone(s) | Adaptive controller |
| `ISpeechSpectrumSource` | `FileSpectrumProfileSource` (V1 import) | "Learn Speech": VAD-gated spectral averages from the microphone, producing the same `spectrumprofile/1` file | Spectrum target (`SPECTRUM_ENGINE.md` §3) |
| `ITargetVoiceProfileSource` | file import | Learned F0 distribution of room speech | Voice diversity "Matched" |
| `IAdaptiveLevelController` | `ConstantLevel` (Strength only) | Slow ambient-adaptive masking level (attack/release in minutes, max dB/hour) | Master gain stage |
| `IIntelligibilityEstimator` | none | Estimated SII (ANSI S3.5) and STI/STIPA (IEC 60268-16), labelled "Estimated" | Analysis/report |
| `ICoverageModel` | none | Spatial map of level/spectrum per location, uniformity statistics | Optimizer, UI |
| `IMeasurementLocationStore` | none | Measurement positions (x, y, z, label, zone) | Coverage, reports |
| `IOptimizer` | none | Cost-function optimization of per-output gain/EQ/delay and masking spectrum/level | Output matrix, targets |
| `IZoneEngineBinding` | single shared engine | Per-zone independent strategy/engine instances (if needed) | Zone stage |

### 1.1 Interface sketches

```cpp
struct CalibrationConfidence { enum Class { None, C_Generic, B_Uncalibrated, A_Calibrated } cls; std::string note; };

class ILevelCalibration {
public:
    virtual ~ILevelCalibration() = default;
    virtual CalibrationConfidence confidence() const = 0;                 // V1: None
    // Returns nullopt in V1 → UI must show digital units only.
    virtual std::optional<float> estimatedSplDbA(OutputOrZoneId, float digitalRmsDbfs) const = 0;
    virtual std::optional<float> digitalForTargetSplDbA(OutputOrZoneId, float targetDbA) const = 0;
};

class ISpeakerCalibration {                                               // applied via output matrix
public:
    virtual ~ISpeakerCalibration() = default;
    virtual OutputCalibration forOutput(uint8_t outputIndex) const = 0;   // {gainDb, delayMs, polarity, eqKernel?}
    virtual uint64_t revision() const = 0;                                // bump → Control re-publishes via RCU
};

class IAdaptiveLevelController {                                          // control thread, ≤ 1 Hz
public:
    virtual ~IAdaptiveLevelController() = default;
    // Returns an additional gain offset (dB) added to Strength; V1 returns 0.
    virtual float updateGainOffsetDb(const AmbientEstimate*, const MaskStatistics&, Seconds dt) = 0;
    virtual Limits limits() const = 0;       // maxOffsetDb, maxSlewDbPerHour …
};

class IIntelligibilityEstimator {
public:
    virtual ~IIntelligibilityEstimator() = default;
    virtual std::optional<SiiResult> estimateSii(const BandLevels& speech, const BandLevels& masker,
                                                 const BandLevels* hearingThreshold) const = 0;
    virtual std::optional<StiResult> estimateSti(const ImpulseResponse&, const BandLevels& noise) const = 0;
};
```

---

## 2. Engine hooks already present in V1

| Hook | Location | V1 state | V2 use |
|---|---|---|---|
| Output-source selector (MASKER / CALIBRATION / MUTE) | After master gain | MASKER; CALIBRATION used by "Test Speakers" | Measurement stimuli |
| Calibration-EQ slot per output | Output matrix | Identity | Per-speaker EQ from the optimizer |
| Per-output gain, delay, polarity | Output matrix | User-set | Written by the optimizer |
| Zone stage | Output matrix | Level + mix offset | Zone targets, zone-specific calibration |
| Master gain offset input | Master gain | 0 dB | Adaptive level controller |
| Spectrum target source | Spectrum engine | Built-in/imported | Learned/measured speech spectrum; room-compensated target |
| Tap T4 per output + engine sample clock | Meters | Digital meters | Timestamp alignment of stimulus and capture (latency measurement) |
| Deterministic stimulus generation | PRNG + FIR designer | Used by the stationary masker | Reproducible calibration noise |
| Analysis framework (1/3-oct, modulation) | Analysis thread | Output analysis | Reused on microphone input |
| Scenario/sidecar format | `bfrender` | Validation | Calibration reports (JSON) |

---

## 3. Calibration signal bus (`CALIBRATION` output mode)

A reserved engine mode that **bypasses the masker** and routes a generated test signal through the output matrix and limiter. The masker generators keep running (silently) so that returning to MASKER is seamless: 500 ms crossfade.

| Signal | Parameters | Generation | V1 exposure |
|---|---|---|---|
| Sine | 20 Hz–20 kHz, level −40…−12 dBFS RMS (default 1 kHz, −20 dBFS RMS) | Phase accumulator (double), 10 ms raised-cosine ramps | Advanced "Test tone" |
| Pink noise | −40…−12 dBFS RMS (default −26) | Pink target (`SPECTRUM_ENGINE.md` §2.2) via the FIR designer, seeded | **Test Speakers** (GUI §23) |
| Speech-shaped noise | Target selectable, level as above | Stationary engine reused | Advanced |
| Log sweep (ESS) | f1–f2 (default 50 Hz–20 kHz, primary analysis 100 Hz–12 kHz), duration 1–30 s (default 10 s), −20 dBFS peak, fade 50 ms, silence tail 2 s | Farina exponential sweep (closed form), with the inverse filter precomputed | Reserved (V2) |
| Channel-identification signal | Sequential per output: 1.0 s pink burst + 0.5 s gap; optional coded tone pair (a unique 2-tone combination per channel) for automatic identification by microphone in V2 | Pink + sines | **Test Speakers** sequence (Left/Right or Speaker 1…N) |
| Band tests | Octave-band noise 125 Hz–8 kHz | FIR bandpass (designed) | Reserved |

**Rules:**
- The CALIBRATION mode obeys the limiter and the output matrix, so it is safe by construction.
- The default calibration level is −26 dBFS RMS, and the GUI requires confirmation for > −12 dBFS.
- "STOP TEST" returns to the previous source within 100 ms (with a 50 ms fade).
- The test sequence has a hard timeout of 5 minutes.

---

## 4. Calibration loop integration (V2 design sketch)

```text
IMeasurementInput ─► analysis (reused) ─► IRoomResponse / ICoverageModel
                                               │
                                          IOptimizer  (J = w1·spatial variance + w2·spectral error
                                               │        + w3·predicted intelligibility + w4·level penalty
                                               │        + w5·EQ penalty; SPEC.md §33)
             ┌─────────────────────────────────┼──────────────────────────────┐
             ▼                                 ▼                              ▼
   ISpeakerCalibration              Spectrum target (room-aware)      ILevelCalibration / IAdaptiveLevelController
   (output matrix: gain,            (SPECTRUM_ENGINE target source)   (master gain offset, zone targets)
    delay, CalEQ slot)
```

**Decoupling of the V1 and V2 control loops:**

| Loop | Measures | Controls | Time constant |
|---|---|---|---|
| V1 spectral correction | T1 (digital babble) | Babble shaper | 120 s |
| V2 speaker calibration | Microphone (room) | Output CalEQ, gain, delay | One-shot + verification |
| V2 adaptive level | Microphone ambient | Master gain offset | Minutes to hours |

Because V1 correction operates on the pre-matrix digital signal and V2 operates post-matrix acoustically, the two cannot fight. V2 must hold the V1 correction frozen during measurement sweeps.

**Free-field model (V2):** `ICoverageModel` gets a `FreeField` implementation. Room-EQ derivation is disabled, and the optimizer uses distance/coverage terms and wind-contamination rejection (`SPEC.md` §38).

---

## 5. Data model reservations

| File/Entity | V1 | V2 |
|---|---|---|
| `spectrumprofile/1` `measurement` field | null | Mic class, positions, duration, VAD ratio, calibrated flag |
| Preset `outputs.channels[].calEqProfile` | null | Reference to a calibration profile |
| `calibration/<id>.bfcal.json` | not created | Per-installation calibration (speakers, positions, IRs as references, results) |
| Area `v2` block | ignored | Calibrated targets (e.g. starting ranges from `SPEC.md` §35–36), level limits |
| Diagnostics `calibration` section | absent | Confidence, last calibration date, verification results |

The V1 preset parser preserves these fields (unknown-field round-trip), so V1 files open in V2 and vice versa (minor-version compatibility).

---

## 6. What V1 must not do (to keep V2 clean)

- It must not compute or display SPL/dBA estimates, even "approximate" ones.
- It must not open input devices.
- It must not hard-wire the master gain to Strength only (the offset input must exist).
- It must not bake speaker EQ into the masker spectrum target (speaker EQ belongs to the output matrix).
- It must not allow the spectral correction loop to observe post-matrix signals.
