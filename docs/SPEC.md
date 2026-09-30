# BabbleForge

## Software Babble Generator

Specification version: 1.0  
Planned calibration release: 2.0  
Primary platforms: Windows initially, architecture portable to macOS/Linux  
Primary purpose: controlled generation of multi-talker babble for speech-privacy and acoustic-masking applications.

---

# 1. Product goal

BabbleForge generates continuously varying multi-talker speech babble whose important acoustic properties are explicitly controlled rather than relying on a conventional crowd-noise recording.

The software shall control:

- number of simultaneous talkers
- talker diversity
- speech activity density
- spectral distribution
- long-term average speech spectrum
- temporal modulation
- individual-talker level
- aggregate RMS level
- peak level
- speech-shaped-noise contribution
- talker spatial distribution
- inter-output correlation
- stereo/multichannel routing
- repetition
- audible linguistic fragments
- output limiting

Version 1.0 creates the masker from scientifically derived presets.

Version 2.0 adds microphone-assisted acoustic measurement and automatic room calibration.

The program is not to claim that a particular recording becomes impossible to recover. Its measurable objective is reduction of speech intelligibility and improvement of speech privacy.

---

# 2. Scientific model

The engine shall model babble as two interacting masking mechanisms.

## 2.1 Energetic masking

Energetic masking occurs when masker energy overlaps target-speech energy within auditory time-frequency regions.

The engine therefore needs controlled broadband speech-spectrum coverage rather than arbitrary noise.

Speech energy important to intelligibility extends through multiple octave bands, with particularly significant information in the middle speech frequencies. Speech-privacy calculations should ultimately use frequency-dependent measurements rather than broadband SPL alone.

ANSI/ASA S3.5 defines Speech Intelligibility Index calculations from speech and noise spectral information.

## 2.2 Informational masking

Competing speech also interferes because a listener must determine which speech stream belongs to the desired speaker.

Experiments using multiple simultaneous talkers show especially strong changes when going from one masker voice to approximately two to four voices. Increasing the count further increasingly produces a dense, noise-like masker.

BabbleForge must consequently expose talker count rather than assuming that the largest possible number of voices is universally optimal.

## 2.3 Temporal glimpsing

Speech naturally contains gaps.

Listeners exploit low-energy moments in the masker to obtain short "glimpses" of target speech. Increasing the number of independent talkers tends to fill these gaps.

Amplitude-modulated masker research confirms that modulation characteristics substantially affect masking effectiveness.

BabbleForge therefore needs explicit control and measurement of:

- envelope modulation
- gap duration
- aggregate speech occupancy
- overlap between speakers

## 2.4 Long-term spectrum

All preset comparisons must be normalized against a defined spectral reference rather than simply changing the number of voices.

Research comparing 1, 2, 4, 8 and 16 talkers explicitly equalized the resulting babble to the same long-term average spectrum.

International LTASS research also found broadly similar long-term spectra among many languages and proposed a universal LTASS for applications such as intelligibility calculations.

---

# 3. Engine architecture

Conceptually:

```text
Speech Corpus
     |
     +---- Talker Pool Manager
     |
     +---- Segment Analyzer
                |
                +-- Voice Activity Detection
                +-- RMS measurement
                +-- spectral profile
                +-- silence detection
                +-- speaker identity
                +-- language metadata
                |
                v
        Talker Scheduler
                |
     +----------+-----------+
     |          |           |
 Talker 1   Talker 2 ... Talker N
     |          |           |
   trim       trim        trim
     |          |           |
   gain       gain        gain
     |          |           |
 spectral    spectral     spectral
 shaping      shaping      shaping
     |          |           |
 spatial     spatial      spatial
 routing      routing       routing
     \          |           /
      \         |          /
       +--------+---------+
                |
          Babble Summing
                |
       Speech-Shaped Noise
                |
                v
         Statistical Monitor
                |
     LTASS / modulation correction
                |
           Master EQ
                |
          Peak Limiter
                |
          Output Matrix
```

---

# 4. Speech-source subsystem

## 4.1 Source requirement

The primary masker shall use recordings from genuinely different talkers.

The program must not obtain its default multi-talker mode by merely duplicating one speaker and pitch-shifting it.

A minimum production corpus should contain:

- 24 distinct voices
- approximately balanced lower and higher fundamental-frequency ranges
- substantial variation in vocal tract characteristics
- multiple speaking rates
- multiple prosodic patterns
- several hours of source material

Preferred target:

- 50+ independent talkers
- 15+ minutes usable speech per talker

## 4.2 Source preprocessing

Every source recording shall be analyzed offline before being admitted to the corpus.

Store:

```text
speaker_id
recording_id
language
duration
sample_rate
channels
integrated_level
peak
spectral_centroid
LTASS profile
voice_activity_regions
silence_regions
estimated F0 distribution
quality score
```

## 4.3 Silence processing

Research implementations have removed pauses greater than approximately 100 ms before constructing dense babble.

BabbleForge should not blindly copy this rule.

Instead provide a configurable parameter:

`Maximum Internal Gap`

Suggested range:

```text
50 ms – 1000 ms
```

Scientific Dense preset:

```text
100 ms
```

Natural Conversation preset:

```text
350–600 ms
```

The preprocessing system may shorten long pauses without altering normal phonetic gaps.

## 4.4 Linguistic-fragment mitigation

Clearly understandable isolated sentences can draw attention and reduce subjective quality.

The scheduler shall therefore:

- use independent source excerpts
- randomize start positions
- avoid repeatedly selecting nearby source segments
- prevent the same voice from immediately repeating
- optionally exclude source sections containing long solo pauses
- optionally cross language groups
- optionally use phonemically plausible but semantically meaningless material in future corpus packs

Do not reverse speech in the default scientific modes because reversing it changes temporal and linguistic characteristics.

---

# 5. Talker scheduler

The scheduler is central to the product.

Each virtual talker has:

```text
speaker identity
current source file
source offset
current gain
target gain
activity state
next state transition
virtual azimuth
virtual distance
output channel distribution
```

## 5.1 Activity states

```text
OFF
FADE_IN
ACTIVE
FADE_OUT
WAIT
```

State changes must be statistically controlled.

## 5.2 Aggregate occupancy

Define:

`Occupancy = fraction of time at least one masker talker is active`

Also track:

`Mean Simultaneous Talkers`

The engine should target mean simultaneous talker count rather than simply maintaining N continuous recordings.

Example:

```text
Configured pool:        12
Mean simultaneous:       7.5
Minimum simultaneous:    5
Maximum simultaneous:   10
```

This produces a less mechanical result.

## 5.3 Voice-level variation

Individual talkers shall have slow randomized level variations.

Suggested normal range:

```text
±1.5 to ±4 dB
```

The aggregate output level is separately normalized.

Fast arbitrary gain modulation must not be added because it would destroy natural speech envelope characteristics.

---

# 6. Number-of-talker modes

Research does not establish one universally optimal talker count.

BabbleForge therefore exposes:

```text
2
4
6
8
12
16
24
Custom
```

Recommended functional categories:

### Focused Speech Competition

2–4 simultaneous talkers.

Characteristics:

- high informational masking
- recognizably speech-like
- substantial modulation
- some individual phrases can emerge

### Balanced Babble

4–8 simultaneous talkers.

Characteristics:

- strong energetic and informational components
- fewer temporal gaps
- individual voices less dominant

This should be the general-purpose default.

### Dense Babble

8–16 simultaneous talkers.

Characteristics:

- strong aggregate energetic masking
- reduced envelope depth
- less distinct individual speech

### Crowd

16–24+ simultaneous talkers.

Characteristics:

- increasingly noise-like
- high temporal density
- lower salience of individual speakers

---

# 7. Speech-shaped noise layer

The engine shall contain an independently generated noise layer.

This is not generic white noise.

Generate broadband pseudorandom noise and filter it toward a configurable speech-derived target spectrum.

Controls:

```text
Off
5%
10%
15%
20%
25%
Custom
```

Percentage represents its contribution to masker energy, not simple sample amplitude.

Purpose:

- fill deep babble envelope minima
- reduce temporal glimpses
- smooth spectrum
- stabilize masking when speech activity fluctuates

Default Balanced mode:

```text
10–15%
```

The GUI shall clearly distinguish:

`Speech Babble`

from

`Stabilizing Noise`

---

# 8. Spectrum engine

The spectrum subsystem continuously measures the generated babble.

Analysis resolution:

- octave bands
- one-third-octave bands
- FFT display for visualization

Core analysis region:

```text
125 Hz – 8 kHz
```

This also aligns well with the frequency coverage used in STI methodology. IEC 60268-16 analyzes seven octave bands from 125 Hz through 8 kHz.

## 8.1 Target spectral modes

### LTASS

Tracks an established long-term average speech spectrum.

### Privacy Curve

Lower and middle frequency weighted masking curve with a downward slope toward high frequencies.

Field research frequently discusses masking spectra with approximately -5 dB/octave slope, although more recent literature reports other preferred slopes and demonstrates that subjective response matters.

Therefore the curve is configurable rather than hard-coded.

### Flat Speech Band

Used primarily for testing.

### Custom

16 or 31 EQ control points.

---

# 9. Spectral correction

A slow control loop compares actual output to target spectrum.

Important:

This shall not operate like a fast multiband compressor.

Update period:

```text
2–10 seconds
```

Correction smoothing:

```text
30–120 seconds
```

Maximum automatic correction:

```text
±6 dB
```

This preserves short-term natural speech fluctuations while maintaining correct long-term behavior.

---

# 10. Modulation analyzer

Continuously calculate aggregate masker-envelope modulation.

Metrics should include:

```text
envelope crest factor
modulation depth
percentage of low-level gaps
gap duration histogram
speech occupancy
mean concurrent talker count
```

Optional advanced analysis:

```text
0.5–16 Hz modulation spectrum
```

This range corresponds closely to modulation rates important to speech intelligibility; IEC STI methodology evaluates speech-envelope modulation over approximately 0.63–12.5 Hz.

---

# 11. Spatial engine

Spatial release from masking can substantially improve speech recognition when target and masker appear spatially separated.

Consequently, a privacy masker should not unnecessarily collapse all babble into one apparent point source.

Supported configurations:

```text
Mono
Stereo
2.1
Quad
5.1
7.1
8-channel custom
16-channel custom
```

## 11.1 Distributed mode

Each physical output receives a different combination of speakers.

Example:

```text
Output A: voices 1,3,6,8
Output B: voices 2,4,7,10
Output C: voices 1,5,9,12
Output D: voices 3,6,8,11
```

Talkers periodically migrate very slowly between virtual positions.

## 11.2 Correlation control

The engine shall calculate cross-correlation among output channels.

Avoid identical masker signals on every speaker.

Benefits:

- avoids a conspicuous single source
- reduces coherent summation artifacts
- improves spatial diffusion
- makes sound-field coverage easier to tune

## 11.3 Stereo mode

Default stereo width should be moderate rather than extreme.

Individual talkers:

```text
random position roughly ±60°
```

Slow movement only.

No obvious ping-pong effects.

---

# 12. Output level system

Version 1 cannot know actual room SPL merely from digital output amplitude.

Therefore V1 must display:

```text
digital RMS
LUFS
true peak
crest factor
```

It must not display estimated dBA as measured fact.

Version 2 adds calibrated SPL.

## 12.1 Limiter

Mandatory output limiter.

Features:

- true-peak detection
- adjustable ceiling
- transparent attack/release
- overload warning
- persistent clipping counter

Default ceiling:

```text
-1 dBTP
```

---

# 13. Presets

Presets represent starting configurations, not guarantees.

## 13.1 Small Room

Examples:

- private office
- small meeting room
- 2–6 occupants

Initial engine target:

```text
Talkers:                  4–6
Noise contribution:       10–15%
Gap maximum:              100–200 ms
Spatial width:            medium
Spectral curve:           Privacy
Modulation density:       medium-high
Suggested outputs:        2–4
```

Because reflections can make a small room acoustically dense, avoid unnecessarily high masker level.

Version 2 nominal calibrated target should begin around the low 40 dBA range rather than assuming 45 dBA.

## 13.2 Conference Room

```text
Talkers:                  6
Noise contribution:       10%
Gap maximum:              150 ms
Spatial width:            medium
Outputs:                  2–4
Speech-band emphasis:     medium
```

Designed for relatively localized speech sources.

## 13.3 Open Office

```text
Talkers:                  8–12
Noise contribution:       15–20%
Gap maximum:              100 ms
Spatial width:            high
Outputs:                  4+
Spectral curve:           Privacy
Output correlation:       low
```

Spatial uniformity is especially important.

Published field measurements show that even professionally configured masking systems can vary significantly by location and that more masking speakers improve uniformity.

## 13.4 Large Room

Examples:

- hall
- large boardroom
- event space

```text
Talkers:                  10–16
Noise contribution:       15%
Gap maximum:              100–150 ms
Spatial width:            high
Outputs:                  4–8+
Independent zones:        enabled
```

Main challenge:

uniform coverage.

## 13.5 Outdoor / Free Field

Outdoors requires fundamentally different assumptions because there is little or no useful room reflection.

```text
Talkers:                  12–20
Noise contribution:       15–25%
Gap maximum:              ~100 ms
Spatial distribution:     maximum
Outputs:                  multiple
Room compensation:        disabled
```

Priority becomes geometrical coverage.

A single distant loudspeaker is specifically discouraged.

Version 2 should treat this as `Free Field` rather than attempting indoor reverberation correction.

## 13.6 Reception / Common Area

Designed to sound less artificial.

```text
Talkers:                  8–16
Noise contribution:       5–10%
Natural pauses:           increased
Talker-level variance:    increased
Spectral correction:      relaxed
Spatial motion:           slow
```

## 13.7 Maximum Density

Research/engineering preset.

```text
Talkers:                  16–24
Noise contribution:       20–30%
Gap maximum:              75–100 ms
LTASS control:            strict
```

This is intentionally dense but is not to be labeled "maximum privacy" until validated experimentally.

## 13.8 Laboratory

Allows direct selection of:

```text
1 / 2 / 4 / 8 / 16 talkers
```

with identical aggregate spectrum and RMS.

This reproduces the general experimental methodology used in published babble research and provides a valuable QA/reference mode.

---

# 14. Preset comparison system

A/B switch:

```text
A = current preset
B = comparison preset
```

Switching must preserve master output level.

Useful comparisons:

```text
4 vs 8 talkers
babble vs speech-shaped noise
10% vs 20% stabilization
LTASS vs privacy spectrum
```

---

# 15. GUI architecture

Main window consists of five pages:

```text
1. RUN
2. BABBLE
3. SPACE
4. ANALYSIS
5. SETTINGS
```

Version 2 adds:

```text
6. CALIBRATE
```

---

# 16. RUN page

This is the everyday operator screen.

Top:

```text
BABBLEFORGE

[ SMALL ROOM ▼ ]

SYSTEM: READY
```

Large center control:

```text
START MASKING
```

When operating:

```text
MASKING ACTIVE
02:17:34
```

Primary controls:

```text
Intensity
Density
Natural ↔ Dense
```

Intensity affects aggregate digital output gain.

Density alters scheduling/talker overlap without changing overall output level.

Natural ↔ Dense modifies:

- pause retention
- number of concurrent voices
- noise contribution
- level variation

Meters:

```text
OUTPUT       █████████
DENSITY      ███████░░
PEAK         -5.1 dBTP
TALKERS      7.6 average
```

Quick profile cards:

```text
Small Room
Conference
Open Office
Large Room
Outdoor
Common Area
```

---

# 17. BABBLE page

## Basic section

```text
Talker pool              12
Average active          7.5
Minimum active            5
Maximum active           10

Voice diversity          High
Speech-shaped noise      12%
Maximum speech gap       100 ms
```

## Voice balance

Display:

```text
Lower voices     50%
Higher voices    50%
```

Avoid gender-dependent labeling at DSP level; acoustic characteristics matter more than demographic metadata.

## Speech activity

Graph:

```text
voices
 10 |        ███
  8 | ████ █████ ████
  6 |██████████████████
  4 |██████████████████
    +----------------------
          time
```

---

# 18. Spectrum panel

Real-time graph overlays:

```text
Actual
Target
Tolerance
```

Switch:

```text
FFT
1/3 octave
Octave
```

Statistics:

```text
LTASS deviation
Speech-band energy
Spectral slope
Low-frequency content
High-frequency content
```

---

# 19. SPACE page

Visual representation of loudspeakers and virtual talkers.

Example:

```text
        SPK 1

    T3       T7

SPK 4           SPK 2

    T1       T5

        SPK 3
```

Controls:

```text
Output configuration
Spatial spread
Movement
Channel decorrelation
Zone assignment
```

Clicking a physical output shows:

```text
Device
Channel
Gain
Delay
EQ
Assigned talkers
Correlation
```

---

# 20. ANALYSIS page

Advanced technical display.

## Speech statistics

```text
Talkers active
Average concurrency
Occupancy
Gap frequency
Median gap
95th percentile gap
Envelope crest factor
```

## Frequency

```text
31-band analyzer
LTASS deviation
Spectral curve
```

## Modulation

```text
0.5–16 Hz envelope spectrum
```

## Output

```text
RMS
LUFS-S
LUFS-I
True Peak
Limiter reduction
```

---

# 21. Corpus page

Accessible through Settings.

Shows:

```text
Talkers          54
Source duration  17 h 42 min
Usable speech    14 h 09 min
Languages        4
```

Corpus validator detects:

- clipped files
- excessive background noise
- duplicate talkers
- incompatible sample rates
- very low speech activity
- repeated files

---

# 22. Repetition prevention

Maintain a source-history database.

Rules:

- no segment reuse inside configured cooldown
- no same-speaker consecutive segment
- randomly vary excerpt lengths
- randomized entry points
- persistent shuffle state

Target:

No perceptibly repeating sequence during normal continuous operation.

---

# 23. Reliability requirements

BabbleForge may be expected to run continuously.

Requirements:

- audio callback performs no file I/O
- no memory allocation inside real-time audio callback
- source segments prefetched
- lock-free communication where practical
- recovery from missing audio device
- recovery from corpus file error
- watchdog for stalled audio engine
- configuration autosave
- clean shutdown
- last-known-good configuration
- crash logging

If an individual talker stream fails:

```text
remove stream
replace asynchronously
continue playback
```

Playback must not stop.

If corpus availability drops below preset requirement:

```text
continue with available voices
increase stabilizing noise if configured
show degraded-state warning
```

---

# 24. Audio backend

Recommended implementation architecture:

```text
JUCE C++
```

Targets:

```text
Windows standalone
WASAPI
ASIO

future:
CoreAudio
ALSA/PipeWire
```

Internal processing:

```text
32-bit float minimum
48 kHz default
44.1 / 48 / 88.2 / 96 kHz supported
```

Preferred default:

```text
48 kHz
```

---

# 25. CPU architecture

Individual voices should not each require an expensive independent convolution engine.

Expected processing:

```text
source decode
gain
simple EQ
pan/matrix
sum
```

Global:

```text
spectral monitoring
master correction EQ
noise generator
limiter
meters
```

Target on a modern desktop:

```text
<5% CPU typical stereo mode
<10% multichannel
```

Exact performance target must be established by benchmarks.

---

# 26. Version 2: acoustic calibration

V2 adds measurement rather than changing the fundamental babble generator.

The calibration system should operate as:

```text
Audio outputs
      |
 calibration signal
      |
   room
      |
measurement microphone
      |
      v
Acoustic analyzer
      |
 room response
 background level
 decay
 uniformity
      |
Optimizer
      |
per-output gain
delay
EQ
masking spectrum
masking level
```

---

# 27. Calibration signal set

Do not rely solely on sine tones.

Provide:

### Level tone

```text
1 kHz sine
```

Used for:

- checking signal path
- gain setup

### Band tests

```text
125
250
500
1k
2k
4k
8k Hz
```

### Pink noise

Used for broadband level and spectrum measurement.

### Speech-shaped noise

Used for speech-band analysis.

### Logarithmic sine sweep

Preferred room-response measurement.

Approximate useful sweep:

```text
50 Hz – 12 kHz
```

with primary calibration analysis concentrated on the speech range.

---

# 28. Microphone requirements

Three confidence classes:

### Class A

Calibrated measurement microphone with known sensitivity/calibration file.

### Class B

Measurement microphone without absolute calibration.

Good frequency-response calibration; absolute SPL requires manual reference.

### Class C

Generic microphone.

Useful for approximate relative frequency/coverage measurement only.

GUI must display confidence accordingly.

---

# 29. V2 calibration workflow

## Step 1: Hardware

Select:

```text
output interface
speaker channels
microphone
microphone calibration file
```

## Step 2: Quiet measurement

Record ambient room sound for:

```text
30–60 seconds
```

Measure:

```text
LAeq if calibrated
octave spectrum
1/3-octave spectrum
background variability
```

## Step 3: Speaker test

Each speaker independently emits test signal.

Detect:

- missing speaker
- incorrect routing
- polarity anomaly
- excessive distortion indication
- large gain mismatch

## Step 4: Response sweep

Perform logarithmic sweep for each output.

Estimate:

```text
frequency response
room impulse response
arrival time
decay characteristics
```

## Step 5: Measurement points

User moves microphone through requested positions.

Example:

```text
Small Room:      3–5 locations
Conference:      4–8
Open Office:     grid of locations
Large Room:      8+
Outdoor:         coverage points
```

The program stores spatial coordinates if supplied.

---

# 30. Acoustic metrics

V2 should compute or estimate:

```text
background SPL
frequency-dependent background level
output frequency response
speaker-to-position level
spatial level variance
impulse response
reverberation / decay estimates
```

Where technically validated:

```text
SII estimate
STI-related prediction
distraction-distance estimate
```

ISO 3382-3 defines distraction distance as the location where STI drops below 0.50 and treats a sound-masking system as a system producing spatially constant background sound.

---

# 31. Standards caution

Babble itself is fluctuating noise.

IEC documentation specifically notes limitations involving fluctuating interference.

Therefore:

BabbleForge shall not claim IEC-certified STI measurement unless the measurement algorithm and hardware have been independently validated.

Instead use labels such as:

```text
Estimated STI
Estimated SII
Relative Privacy Improvement
```

until certification work is completed.

---

# 32. Spatial calibration

This is one of V2's highest-value features.

For each measurement location and band:

```text
error = measured level - target level
```

Optimize:

```text
speaker gain
speaker EQ
optional delay
target babble spectrum
```

Objective:

minimize spatial variance without creating excessive local level.

Published office measurements found that ±1 dB overall level uniformity was achievable across most measurement locations in a carefully configured example installation, while one-third-octave spectral uniformity was substantially harder; more loudspeakers were particularly helpful.

---

# 33. Automatic optimization

Optimization cost function:

```text
J =
w1 * spatial_level_variance
+
w2 * spectral_target_error
+
w3 * predicted_speech_intelligibility
+
w4 * excessive_level_penalty
+
w5 * speaker_EQ_penalty
```

Hard constraints:

```text
maximum user-defined calibrated SPL
maximum EQ boost
maximum output gain
maximum limiter activity
```

---

# 34. Iterative calibration

Calibration must run:

```text
MEASURE
   ↓
OPTIMIZE
   ↓
PLAY TEST MASKER
   ↓
MEASURE AGAIN
   ↓
VERIFY
```

Do not calculate EQ once and assume success.

The verification stage determines whether the optimization actually improved:

```text
uniformity
spectrum
speech-band masking
```

---

# 35. V2 Small Room strategy

Priorities:

1. avoid excessive output
2. flatten major speaker/room anomalies
3. maintain speech-spectrum coverage
4. prevent strong localized source
5. use reflections rather than fighting all of them

Suggested starting calibrated range:

```text
~39–42 dBA
```

Then tune based on measured speech level and desired privacy.

This is an engineering starting point, not a universal scientific threshold.

---

# 36. V2 Open Office strategy

Priorities:

1. spatial uniformity
2. speech-band spectrum
3. moderate overall level
4. multiple speaker zones
5. low speaker-to-speaker correlation

Suggested initial target:

```text
~41–44 dBA
```

Research and field guidance commonly place masking in approximately the low-to-mid 40 dBA region; importantly, long-term field research also found that 45 dBA can itself be annoying, so the optimizer should seek the lowest level that achieves the required masking effect.

---

# 37. V2 Large Room strategy

Priorities:

1. multiple measurement points
2. distributed output
3. zone-specific gain
4. zone EQ
5. avoid attempting to solve spatial problems with extreme EQ

---

# 38. V2 Outdoor strategy

Outdoor mode shall disable room-reverberation assumptions.

Measure:

```text
ambient spectrum
speaker level versus distance
coverage variance
wind contamination
```

Optimization focuses on:

```text
speaker placement guidance
channel level
coverage overlap
speech-band spectrum
```

Repeated measurements with excessive wind noise must be rejected.

---

# 39. Calibration GUI

Wizard:

```text
ROOM CALIBRATION

1 Hardware       ✓
2 Ambient        ✓
3 Speakers       ✓
4 Measurements   ●
5 Optimize
6 Verify
```

Room map:

```text
+-------------------------+
| S1                  S2  |
|                         |
|      M1      M2         |
|                         |
|      M3      M4         |
|                         |
| S4                  S3  |
+-------------------------+
```

After calibration:

```text
Coverage Uniformity      GOOD
Spectrum Match           GOOD
Speech Privacy Estimate  IMPROVED
Peak Location            M2
Lowest Masking           M4
```

Avoid meaningless proprietary scores where the underlying metric can be displayed.

---

# 40. Calibration report

Export:

```text
PDF
JSON
CSV
```

Report includes:

- date
- audio hardware
- microphone
- calibration confidence
- speaker configuration
- measurement positions
- ambient spectrum
- measured level by position
- calibrated target
- actual achieved result
- spectrum graphs
- output settings
- warnings
- software version

---

# 41. Validation suite

The software must be tested scientifically, not only listened to.

## Digital tests

For each preset measure:

```text
average simultaneous talkers
RMS
true peak
LTASS error
1/3-octave spectrum
modulation spectrum
gap histogram
channel correlation
```

## Reproducibility

Provide deterministic test mode:

```text
Random Seed = fixed
```

so QA can reproduce exactly the same masker.

## Reference tests

Generate:

```text
1-talker
2-talker
4-talker
8-talker
16-talker
```

with:

```text
equal duration
equal RMS
equal target LTASS
```

These become regression-test fixtures.

---

# 42. Speech-intelligibility experiment

A future validation package should contain standardized target-speech material.

Measure listener recognition against:

```text
no masker
speech-shaped noise
2-talker
4-talker
8-talker
16-talker
hybrid masker
```

at several SNR values.

This allows the project to experimentally validate its presets rather than treating theoretical assumptions as measured fact.

---

# 43. Machine speech-separation testing

Because modern recording systems may apply speech enhancement, an optional future QA suite should also test recordings with common open-source speech-separation algorithms.

Report:

```text
original intelligibility
masked intelligibility
post-separated intelligibility
```

This is a testing metric only.

No preset should claim that speech is unrecoverable.

---

# 44. Configuration format

Human-readable JSON:

```json
{
  "profile": "open_office",
  "talkerPool": 12,
  "meanConcurrentTalkers": 8,
  "speechNoiseRatio": 0.15,
  "maxGapMs": 100,
  "spectrum": "privacy",
  "spatialMode": "distributed"
}
```

Presets should be data-driven rather than compiled into application logic.

---

# 45. Logging

Log:

```text
engine start/stop
audio-device changes
corpus failures
underruns
limiter overload
preset changes
calibration events
measurement failures
```

Do not record microphone audio during ordinary masking operation.

Calibration recordings should be discarded by default after extracting acoustic measurements unless explicitly saved.

---

# 46. Project structure

Proposed repository:

```text
babbleforge/
├── README.md
├── LICENSE
├── CMakeLists.txt
├── docs/
│   ├── SPEC.md
│   ├── ENGINE.md
│   ├── GUI.md
│   ├── CALIBRATION_V2.md
│   ├── VALIDATION.md
│   └── research/
│       ├── RESEARCH_NOTES.md
│       └── REFERENCES.md
├── Source/
│   ├── Audio/
│   ├── Babble/
│   ├── Corpus/
│   ├── Analysis/
│   ├── Spatial/
│   ├── Calibration/
│   ├── UI/
│   └── Tests/
├── resources/
│   └── presets/
└── tests/
```

---

# 47. Version milestones

## V0.1

Offline research/reference generator.

- corpus loader
- N-talker mixer
- RMS matching
- deterministic output

## V0.2

Real-time engine.

- scheduling
- noise layer
- spectrum monitoring
- limiter

## V0.3

GUI.

- Run
- Babble
- Analysis

## V0.4

Spatial engine.

- stereo
- multichannel
- channel decorrelation

## V0.5

Presets and reliability.

## V0.9

Scientific validation and QA.

## V1.0

Stable non-calibrated release.

## V2.0

Room measurement and automatic calibration.

---

# 48. Definition of V1 success

V1 is ready when it can:

1. continuously generate non-repeating multi-talker babble
2. reproduce selected talker-count experiments
3. maintain target aggregate level
4. maintain target LTASS within specified tolerance
5. maintain expected talker occupancy
6. supply multiple independent output channels
7. operate continuously without audio dropouts
8. produce reproducible QA output
9. expose scientifically relevant controls
10. prove through listening experiments that at least one preset materially reduces intelligibility relative to the unmasked condition

---

# 49. Definition of V2 success

V2 is ready when it can:

1. detect and validate a measurement microphone
2. measure room/background response
3. measure each output independently
4. create a spatial acoustic map
5. optimize output gain and EQ
6. select the lowest practical masking level consistent with the configured target
7. repeat measurements automatically
8. quantify improvement
9. export calibration results
10. reproduce the calibration outcome within defined tolerances

The core design principle is:

**Generate babble statistically, measure it objectively, and calibrate the acoustic field rather than assuming that something which sounds like a crowd is an effective masker.**