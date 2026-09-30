# BabbleForge Research Notes — Deep Research Addendum

Research expansion: September 2026

This document extends the initial BabbleForge research notes with additional papers, openly available manuscripts, theses, datasets and open-source implementations discovered during a deeper literature and software survey.

---

# 19. Major design finding: babble should not be the only masking engine

The original design treated genuine multi-talker babble as the primary masker with speech-shaped stationary noise as a relatively small stabilizing component.

The deeper literature indicates that BabbleForge should instead contain **two first-class masking engines**:

1. Multi-talker babble
2. Stationary speech-shaped masking noise

and a third hybrid mode that combines them.

Renz, Leistner and Liebl directly compared conventional noise falling approximately 5 dB/octave with stationary noise matched to the spectrum of disturbing speech. Speech-shaped stationary noise produced similar cognitive-performance benefits at a roughly 3 dB less favorable speech-to-noise ratio.

In other words, matching the masker spectrum closely to speech can materially improve masking efficiency.

Their follow-up comparison of stationary speech-shaped masking with fluctuating/time-reversed speech also found that the stationary masker continued improving performance as SNR decreased, whereas the fluctuating masker did not behave as consistently. The fluctuating speech masker also produced greater annoyance in some conditions.

## Design change

BabbleForge V1 should support:

```text
MASK TYPE

Speech-Shaped Stationary
Multi-Talker Babble
Hybrid
Research / Custom
```

The software should not advertise one universally superior mode.

---

# 20. Multi-voice masking has direct office-performance evidence

Keus van de Poll et al. specifically investigated using multiple voices as an office masker.

Experiment 1 compared broadband noise, multiple voices and water sounds.

Experiment 2 compared:

```text
1 voice
3 voices
5 voices
7 voices
```

For their writing task, performance improved progressively from the one-voice condition toward the seven-voice condition.

This is unusually relevant to BabbleForge because it studies multiple voices as the **masking source itself**, rather than merely using babble as laboratory background noise.

## Design consequence

Add a dedicated preset family:

```text
MULTI-VOICE MASKING

3 Voice
5 Voice
7 Voice
Dense Multi-Voice
```

These should be separate from high-count crowd babble.

Recommended experimental default:

```text
7 active voices
```

This should initially be labelled:

`Research-derived starting point`

rather than `optimal`.

---

# 21. Fewer voices and many voices produce qualitatively different masking

Brungart's work demonstrates that with one competing voice, informational masking can dominate energetic masking.

Similarity between target and masker voice characteristics significantly affects recognition. Target speech was harder to segregate under some same/similar-talker conditions and easier to segregate when vocal characteristics differed substantially.

Rosen et al. show that increasing the number of talkers progressively changes the character of the masker and its temporal opportunities for glimpsing.

## Important implication

Voice diversity cannot simply be maximized without considering the objective.

If individual masker voices are extremely different from typical target voices, perceptual segregation may become easier.

Therefore the engine should support two concepts:

```text
VOICE DIVERSITY
```

and

```text
TARGET SIMILARITY
```

The second becomes especially useful in calibrated/research operation.

Future V2/V3 could estimate the spectral/F0 range of representative room speech and select a masker population whose aggregate characteristics overlap it.

---

# 22. Add a "Matched Speech Spectrum" mode

The earlier design proposed generic LTASS and privacy curves.

The Renz research supports adding a dedicated:

```text
Matched Speech Spectrum
```

mode.

The engine measures or loads a representative target speech spectrum and generates stationary noise having approximately that spectrum.

This should be distinct from:

```text
Universal LTASS
Generic Privacy Curve
```

because the Renz experiments indicate that matching the actual disturbing speech spectrum can outperform a conventional fixed -5 dB/octave masking curve.

---

# 23. Masking spectrum should not default permanently to -5 dB/octave

The -5 dB/octave curve is historically common, but the research does not support treating it as uniquely optimal.

Renz et al. found speech-shaped stationary noise more efficient for their task.

More recent office literature also cites satisfactory low/mid-frequency weighted spectra with slopes approximately -7 to -9 dB per octave in some contexts.

Therefore preset curves should include at least:

```text
Speech Matched
LTASS Derived
-5 dB/octave
-7 dB/octave
-9 dB/octave
Custom
```

The curve should be data, not hard-coded engine behavior.

---

# 24. Low frequencies cannot simply be discarded

A particularly useful result comes from Renz et al.'s loudspeaker-location experiment.

They compared localized and conventional ceiling masking arrangements and found spatial separation between speech and masking could reduce effectiveness.

Importantly, a disadvantage observed with ceiling masking disappeared when the masker's high-pass cutoff was lowered from 200 Hz to 100 Hz.

The paper specifically identifies 125 Hz octave-band masker content as affecting cognitive-performance results.

## Design consequence

Do not arbitrarily high-pass the masker at 200–250 Hz.

The spectrum engine should explicitly monitor:

```text
125 Hz
250 Hz
500 Hz
1 kHz
2 kHz
4 kHz
8 kHz
```

For capable loudspeakers, default privacy modes should retain useful 125 Hz energy rather than automatically removing it.

However, room-calibration logic must prevent excessive LF energy caused by room modes or inadequate speakers.

---

# 25. Spatial coincidence is important

The Renz loudspeaker-location study found that presenting masker and distracting speech from different horizontal directions could impair masking efficiency compared with more similar source locations.

This agrees with the wider spatial-release-from-masking literature.

## Design consequence

The initial idea of always maximizing perceived spatial width needs refinement.

BabbleForge should distinguish:

```text
DIFFUSE COVERAGE
```

from

```text
PERCEPTUAL SEPARATION
```

We want masking energy spatially uniform throughout the area, but we should not intentionally create conspicuous, isolated masker sources that listeners can segregate from target speech.

For a room system:

- many low-level sources are preferable to a few conspicuous sources;
- adjacent speakers may use decorrelated signals;
- output should remain perceptually diffuse;
- strong hard-panned voices should be avoided.

---

# 26. Spatial uniformity measurements: concrete numbers

Zarei et al. conducted an unusually dense measurement campaign in an 18-person, 68.5 m² office.

They measured the masking field approximately every 0.6 m.

Even under deliberately favorable installation conditions:

- ±0.5 dB broadband uniformity occurred at only about 61% of positions;
- ±1 dB was reached at about 99%;
- for 250 Hz–4 kHz one-third-octave levels, ±2 dB was obtained only about 55% of the time;
- approximately ±3.5 dB was needed for 95% spectral compliance.

Increasing speaker count was the most effective tested method of improving uniformity.

The preceding master's thesis contains additional simulations and measurement details.

## Design change for V2

Do not set unrealistic calibration acceptance criteria such as:

```text
±0.5 dB everywhere
```

Suggested initial grading:

```text
Broadband

Excellent     <= ±1 dB
Good          <= ±1.5 dB
Acceptable    <= ±2 dB
Poor          >  ±2 dB
```

For one-third-octave spectral matching, tolerances must be wider.

Exact thresholds should be experimentally validated.

---

# 27. V2 should recommend additional speakers before extreme EQ

Zarei's results make this particularly clear.

Spatial problems cannot always be equalized away.

The optimizer therefore needs two types of recommendation:

```text
DSP solution
Physical-layout solution
```

Example:

```text
CALIBRATION RESULT

250–1000 Hz coverage varies by 6.1 dB.

DSP correction alone cannot meet the selected uniformity target.

Recommendation:
Add an additional masking loudspeaker near measurement positions M5/M6.
```

That is more scientifically defensible than applying +10 dB of EQ to one loudspeaker.

---

# 28. Level-adaptive masking is worth making a major V2 feature

A 2024 field study tested level-adaptive masking for two to three months.

Background levels changed approximately:

```text
Company 1:
28.7 → 41.9 dBA

Company 2:
32.4 → 42.6 dBA
```

The study reported reductions in intelligible-speech distraction and improvements in some short-term subjective measures.

This is important because a fixed masker may be louder than necessary when the environment is quiet or ineffective when occupancy increases.

## V2 addition

Add:

```text
ADAPTIVE MASKING
```

The microphone continuously measures the environmental level.

Adjustment must be very slow so users do not perceive obvious pumping.

Potential parameters:

```text
Minimum calibrated level
Maximum calibrated level
Adaptation window
Attack minutes
Release minutes
Maximum dB change/hour
```

Suggested architecture:

```text
ambient estimator
       ↓
speech/activity estimator
       ↓
required masking estimator
       ↓
slow control integrator
       ↓
masker target level
```

No short-term AGC behavior.

---

# 29. Avoid using 45 dBA as a universal target

The long-term French field study lasted 26 weeks, with 14 weeks of nominal masking.

Despite following commonly cited recommendations, it did not find universal improvements and suggested that approximately 45 dBA could itself become annoying.

The 2024 adaptive study achieved its effects around approximately 42 dBA in its environments.

## Revised design principle

V2 optimization objective:

```text
MINIMIZE MASKING LEVEL

subject to

configured intelligibility/privacy objective
+
acceptable spatial uniformity
```

This should replace any algorithm whose target is simply:

```text
make room = 45 dBA
```

---

# 30. Add target-to-masker ratio as a primary metric

A Japanese study of desk-working conditions reported that an appropriate masker's required level could be reduced relative to some non-desk scenarios and reported an optimum condition around a target-to-masker ratio of approximately -2 dB for its experimental desk-work environment.

This does not establish a universal -2 dB rule.

It does show that **speech-to-masker relationship** may be more useful than absolute masker SPL alone.

## GUI addition

V2 Analysis should show:

```text
Estimated Speech Level
Masker Level
Speech-to-Masker Ratio
```

by measurement position.

---

# 31. Cognitive performance is not predicted perfectly by one STI number

Jahncke, Hongisto and Virjonen found that effects of speech intelligibility depended on the cognitive task.

Some tasks were substantially more affected than others.

The steepest performance change did not occur at exactly the same STI region for every task.

A later laboratory study likewise found STI significantly associated with working-memory performance, discomfort and mental load, but individual/task differences remained important.

## Design consequence

V2 must never reduce room performance to a single proprietary:

```text
Privacy Score = 92%
```

Instead expose underlying quantities.

Example:

```text
Estimated STI             0.31
Estimated SII             0.22
Speech/Masker             -2.7 dB
Spatial Level Variation   ±1.4 dB
```

Then optionally provide descriptive categories.

---

# 32. Keep STI <0.20 as a research reference, not a hard target

Office-acoustics literature has proposed STI below approximately 0.20 as a region where intelligible background speech may cease producing some negative cognitive effects.

Subsequent experiments have supported this imperfectly rather than establishing a universal threshold.

Therefore:

```text
STI < 0.20
```

may be displayed as a research/reference threshold but should not become a universal success criterion.

---

# 33. Full STI and STIPA source implementations exist

Two particularly valuable open-source projects were found.

## zawi01/stipa

MATLAB STIPA implementation based on IEC 60268-16.

Features include:

- STIPA signal generation
- STI calculation
- verification tests
- control measurements against commercial instrumentation

License:

```text
GPL-3.0
```


This is excellent reference material but GPL licensing means code should not simply be copied into a differently licensed BabbleForge application without accepting GPL implications.

## Cieslar-Simon/STI

Extends the work to:

- full direct STI
- STIPA
- indirect STI from impulse response
- exponential sweep support
- octave-band analysis
- plots and intermediate results
- real-world control measurements

The corresponding 2025 master's thesis is openly available.

License:

```text
GPL-3.0
```

## Recommendation

Use these projects as:

- reference implementations;
- independent regression or validation oracles;
- a way to understand intermediate MTF calculations.

For BabbleForge production code, independently implement the required algorithms from standards/public descriptions unless BabbleForge itself adopts a compatible GPL license.

---

# 34. SII has an unusually useful reference implementation

Google published `speech_intelligibility_index`, a Python implementation of ANSI S3.5.

It includes:

- NumPy implementation
- JAX implementation
- example Colab
- tests/reference usage

It is Apache-2.0 licensed.

The repository was archived in April 2026, but remains extremely useful as a validation reference.

## Recommendation

SII should become a V2 engineering metric earlier than originally planned.

Create:

```text
tools/reference/sii/
```

during development and compare BabbleForge's own calculations with Google's implementation.

Do not copy ANSI standard text into the repository.

---

# 35. Exponential/log swept sine is strongly supported for V2

Angelo Farina's classic 2000 work describes an exponentially swept-sine method capable of deriving the linear impulse response while separating nonlinear distortion components.

The paper applies directly to loudspeaker and room-acoustics measurements.

The author's manuscript is openly hosted by Farina himself despite the AES publisher page normally requiring access.

## Design consequence

V2 should use ESS/log-sweep measurement as its main room-response method.

Not:

```text
individual sine tones only
```

Test tones remain useful for:

- channel identification;
- gain verification;
- polarity troubleshooting;
- basic hardware checks.

---

# 36. Open-source swept-sine implementation reference

`tomoyanonymous/IR_measurement_toolbox` provides an MIT-licensed impulse-response measurement implementation.

It supports:

- logarithmic swept-sine generation
- inverse-filter processing
- repeated measurements
- averaging
- multichannel recording

Its documentation explicitly references Farina's swept-sine work.

## Useful idea to emulate

V2 should support measurement repetition:

```text
Sweep 1
Sweep 2
Sweep 3
```

then reject contaminated measurements and/or average valid responses.

This is better than trusting a single sweep contaminated by a door slam or conversation.

---

# 37. Add calibration measurement-quality scoring

V2 should calculate quality indicators for every acquired sweep.

Proposed metrics:

```text
SNR
coherence/repeatability
peak clipping
ambient contamination
timing consistency
impulse-response similarity
```

Example:

```text
Measurement M4

Quality: POOR
Reason:
Transient interference detected during sweep.

[Repeat measurement]
```

Repeated-sweep consistency is more useful than simply completing the wizard successfully.

---

# 38. Open Sound Meter is an important engineering reference

Open Sound Meter is a mature C++ open-source sound-system measurement application.

It provides functionality relevant to V2 including:

- FFT/RTA style measurement
- impulse response
- SPL measurement
- calibration-file support
- coherence-derived SNR
- filters
- distortion-related measurements
- ASIO support

Desktop releases are GPLv3.

## Useful lessons

Study its architecture for:

```text
audio I/O
measurement sessions
calibration files
real-time FFT
SPL handling
ASIO device handling
measurement persistence
```

Because of GPLv3, treat implementation as architectural/reference material unless BabbleForge adopts GPL-compatible licensing.

---

# 39. Pyroomacoustics should become part of the project's validation tooling

`pyroomacoustics` is an MIT-licensed room-acoustics simulation environment.

It supports:

- multiple sources
- multiple microphones
- 2D/3D rooms
- image-source simulation
- ray tracing
- room impulse responses
- frequency-dependent materials
- source/microphone directivity
- beamforming
- source separation


## Major benefit for BabbleForge

Before V2 exists physically, simulated rooms can test the calibration optimizer.

Example validation matrix:

```text
Room A:
4 × 5 × 2.7 m
RT60 0.3 s

Room B:
8 × 12 × 3 m
RT60 0.6 s

Room C:
open office
multiple sources
partial absorption
```

Simulate:

```text
4 speakers
8 speakers
16 speakers
```

Then determine whether BabbleForge's optimizer correctly detects and minimizes nonuniformity.

## Recommendation

Do not embed Python into the production application initially.

Use Pyroomacoustics as an independent offline QA/reference system.

---

# 40. Spatial Audio Framework is highly relevant to multichannel BabbleForge

The Spatial Audio Framework is a mature C/C++ framework containing:

- Ambisonics
- VBAP
- loudspeaker decoding
- multichannel decorrelation
- room simulation
- matrix convolution
- arbitrary-array panning
- HRTF/SOFA processing

Its core modules use the permissive ISC license, although some optional modules are GPLv2.

Particularly relevant components:

```text
saf_vbap
saf_hoa
saf_reverb
decorrelator
panner
matrixconv
multiconv
```

## Recommendation

This is probably the most useful C/C++ reference for BabbleForge's eventual spatial-output engine.

Evaluate using permissively licensed SAF core modules rather than writing every spatial primitive from scratch.

---

# 41. IEM Plug-in Suite is useful for multichannel GUI/routing design

The IEM Plug-in Suite is open-source, JUCE-based, and supports Ambisonic operation up to high orders.

It contains:

- encoders
- decoders
- arbitrary speaker layouts
- multichannel routing
- dynamics
- spatial visualization
- standalone builds


## Useful lesson

Its speaker-layout and spatial visualization interfaces are good references for BabbleForge's:

```text
SPACE
```

page.

The purpose is different, so BabbleForge should remain substantially simpler.

---

# 42. Spatial Audio Framework is preferable to starting with HRTF binaural processing

For real room masking, physical loudspeaker outputs matter more than headphone virtualization.

Therefore priorities should be:

```text
1. Channel matrix
2. Gain
3. Delay
4. Decorrelated feeds
5. VBAP/distribution
6. Arbitrary speaker geometry
```

HRTF/SOFA support can remain a test/demo feature.

If it is eventually required, `libmysofa` offers a BSD-3-Clause C implementation for reading SOFA HRTF data.

---

# 43. Voice activity detection should be automated

Manual silence editing across a 20–50 speaker corpus is unnecessary.

Two practical open-source VAD references were identified.

## WebRTC VAD

Fast frame-based detector.

Supported rates include common speech rates through 48 kHz depending on implementation, with 10/20/30 ms frames.

It offers multiple aggressiveness levels.

## Silero VAD

Neural voice-activity detector distributed under MIT licensing.

## Proposed architecture

Use VAD offline during corpus ingestion.

Store speech-probability/activity metadata.

Do not run a heavyweight neural detector inside the real-time audio callback.

---

# 44. Corpus ingestion should become a formal subsystem

Instead of shipping hand-edited WAV files, create:

```text
Corpus Builder
```

Pipeline:

```text
Input audio
   ↓
Decode
   ↓
Resample
   ↓
Channel conversion
   ↓
VAD
   ↓
Noise-quality analysis
   ↓
Clipping detection
   ↓
Segmentation
   ↓
Level measurement
   ↓
LTASS/F0 metadata
   ↓
Speaker metadata
   ↓
Corpus database
```

This makes the masker corpus reproducible.

---

# 45. Mozilla Common Voice is potentially useful, with distribution caveats

Mozilla Common Voice provides large quantities of multi-speaker speech under CC0.

Current dataset pages identify Common Voice releases as CC0.

Its current terms also request that datasets not simply be mirrored elsewhere and that users access them through Mozilla Data Collective.

## Recommendation

Common Voice is valuable for:

```text
research
corpus experimentation
speaker-diversity testing
```

but the project's downloadable binary should probably not simply bundle a massive Common Voice-derived corpus without a careful legal/distribution review.

A separate corpus-builder workflow is preferable.

---

# 46. MUSAN is particularly useful for development

MUSAN contains approximately:

- speech
- music
- environmental/noise recordings

and is explicitly designed as a corpus for acoustic/audio research.

OpenSLR lists it under CC BY 4.0.

It has already been used by research projects to construct babble by selecting and combining multiple speech samples.

## Recommendation

Use MUSAN as one of the initial reproducible test corpora.

Do not necessarily make it the final production corpus.

---

# 47. Microsoft DNS Challenge offers useful data-generation patterns

Microsoft's DNS Challenge repository contains a mature noisy-speech synthesis pipeline.

It combines:

- clean speech
- environmental noise
- room impulse responses
- controlled SNR
- reproducible configuration
- unit tests


Its dataset documentation also identifies numerous speech/noise/RIR sources and their individual licenses.

## Useful lesson

BabbleForge's offline testing framework should imitate this **configuration-driven synthesis model**.

Example:

```yaml
seed: 182731
talkers: 7
target_ltas: speech_matched
noise_fraction: 0.12
duration_seconds: 300
room_ir: room_07.wav
target_snr_db: -2
```

This enables exact experiment reproduction.

---

# 48. Add an adversarial validation pipeline

A masker that works well for a person may perform differently after modern speech enhancement.

BabbleForge should therefore have two validation tracks.

## Human/acoustic

```text
STI
SII
word recognition
sentence recognition
serial recall experiments
```

## Machine

```text
ASR word error rate
speech enhancement
source separation
dereverberation
```

Microsoft's DNS Challenge is useful here because it specifically evaluates systems involving interfering talkers, enhancement and ASR-related metrics.

The goal is not to claim impossibility of recovery.

It is to quantify how masker performance changes after common enhancement techniques.

---

# 49. Stationary masker and babble should be evaluated separately against speech enhancement

This is important.

Speech enhancement systems are often optimized against approximately stationary noise.

Multi-talker speech presents a different separation problem.

Therefore BabbleForge QA needs:

```text
Stationary Speech-Shaped
Multi-Talker
Hybrid
```

tested independently.

Do not assume the acoustically strongest human masker will also be strongest after algorithmic processing.

---

# 50. Hybrid mode should be redesigned

The initial design proposed approximately:

```text
85–90% babble
10–15% stationary noise
```

as a plausible default.

The deeper literature does not justify fixing that ratio.

Instead implement:

```text
BABBLE / STATIONARY MIX

0% -------------------------- 100%
Stationary                     Babble
```

The output level remains constant while energy distribution changes.

Research presets:

```text
100 / 0
75 / 25
50 / 50
25 / 75
0 / 100
```

Then test them experimentally.

This avoids embedding an unsupported assumption.

---

# 51. Add Masker Spectrum Learning

V2 should contain a short measurement mode:

```text
LEARN SPEECH
```

Procedure:

1. User selects representative area.
2. Microphone captures ambient conversational activity.
3. VAD identifies probable speech frames.
4. Non-speech/background sections are excluded.
5. Program estimates long-term speech spectrum.
6. Program derives candidate stationary masker spectrum.
7. User may compare it with LTASS/reference curves.

No raw speech recording needs to be retained.

Stored data:

```text
spectral averages only
```

This follows the evidence that speech-matched stationary masking can outperform a generic curve.

---

# 52. Add privacy zones

Large spaces may have different acoustic requirements.

V2 should support:

```text
ZONE A
Reception

ZONE B
Meeting tables

ZONE C
Workstations

ZONE D
Walkway
```

Each zone has:

```text
measurement points
speaker channels
target spectrum
minimum level
maximum level
calibration state
```

Optimization can minimize boundary discontinuity while retaining zone-specific targets.

---

# 53. Outdoors should become a separate acoustic model

The original Outdoor preset treated outdoors primarily as a denser masking condition.

That is insufficient.

Outdoor/free-field use removes much of the beneficial diffusive effect of indoor reflections and introduces:

```text
distance attenuation
wind
changing ambient spectrum
temperature effects
large spatial coverage
high source localization
```

Therefore Outdoor mode should become:

```text
FREE-FIELD MODE
```

rather than merely another babble preset.

Its priorities:

1. distributed physical sources
2. speech-band coverage
3. source localization avoidance
4. distance/coverage map
5. ambient-level adaptation
6. wind-contaminated measurement rejection

Room-EQ assumptions must be disabled.

---

# 54. Add speaker directivity to V2 calibration

The room simulator and calibration model should account for loudspeaker directivity if known.

Pyroomacoustics supports directivity models for sources and microphones in relevant simulation modes.

Speaker profile:

```text
Speaker model
Position
Orientation
Nominal directivity
Frequency response
Maximum level
```

The optimizer should prefer physical placement changes over impossible EQ compensation outside a speaker's coverage region.

---

# 55. Recommended V1 masking modes after deeper research

The revised V1 presets should become:

```text
1. Speech-Matched Stationary
2. Balanced Babble
3. Multi-Voice 7
4. Dense Babble
5. Hybrid
6. Common Area
7. Research Laboratory
```

Room presets are then layered on top:

```text
Small Room
Conference
Open Office
Large Room
Free Field
Custom
```

This separation is better architecturally.

Example:

```text
Environment:
Open Office

Mask Strategy:
Speech-Matched Stationary
```

instead of making `Open Office` itself define one fixed noise algorithm.

---

# 56. Revised configuration model

Configuration should be two dimensional.

```json
{
  "environment": "open_office",
  "maskStrategy": "hybrid",
  "babble": {
    "meanTalkers": 7,
    "maxGapMs": 120
  },
  "stationary": {
    "spectrum": "learned_speech"
  },
  "mix": {
    "babbleEnergy": 0.50
  }
}
```

This allows science-based comparison.

---

# 57. Add research experiment mode

BabbleForge can become useful as an acoustic-research application in addition to being a masker.

Experiment Matrix:

```text
Talkers:
1 / 2 / 3 / 4 / 5 / 7 / 8 / 12 / 16

Mask:
Babble
Stationary
Hybrid

Spectrum:
LTASS
Speech Matched
-5 dB/oct
-7 dB/oct

SNR:
+6 / +3 / 0 / -3 / -6 dB
```

Batch-render each combination with deterministic seeds.

This would allow the project's assumptions to be tested rather than merely encoded.

---

# 58. Recommended open-source reference stack

These are currently the most useful projects discovered.

| Project | BabbleForge use | License |
|---|---|---|
| Google Speech Intelligibility Index | SII validation | Apache-2.0 |
| zawi01/STIPA | STIPA validation | GPL-3.0 |
| Cieslar-Simon/STI | Full/indirect STI validation | GPL-3.0 |
| Pyroomacoustics | room simulation/testing | MIT |
| Spatial Audio Framework | multichannel/spatial DSP | ISC core |
| IEM Plugin Suite | GUI/routing reference | open source |
| Open Sound Meter | measurement architecture | GPL-3.0 |
| IR Measurement Toolbox | swept-sine reference | MIT |
| Silero VAD | corpus segmentation | MIT |
| WebRTC VAD | lightweight segmentation | permissive WebRTC code |
| libmysofa | optional HRTF/SOFA | BSD-3-Clause |
| Microsoft DNS Challenge | reproducible test synthesis | open-source code / mixed dataset licenses |

Relevant sources:

---

# 59. Recommended architectural dependencies

For the production C++ application:

```text
JUCE
   |
   +-- audio devices
   +-- GUI
   +-- DSP primitives
   +-- file I/O
   +-- threading
```

Potentially:

```text
Spatial Audio Framework
   |
   +-- decorrelation
   +-- VBAP
   +-- arbitrary speaker layouts
```

Independent tooling:

```text
Python
   |
   +-- Pyroomacoustics
   +-- Google SII reference
   +-- experiment generation
   +-- statistical analysis
```

Validation-only MATLAB/Octave reference:

```text
STIPA
Full STI
```

Avoid making MATLAB or Python dependencies mandatory for normal BabbleForge operation.

---

# 60. Licensing strategy

If BabbleForge is intended to use a permissive open-source license, prefer direct integration of:

```text
MIT
BSD
ISC
Apache-2.0
```

components.

GPL projects should primarily be used as:

- algorithmic references;
- independent test oracles;
- external tools during development.

Do not copy GPL implementation code into a permissively licensed BabbleForge codebase without intentionally accepting the resulting licensing obligations.

---

# 61. Scientific validation should become a formal release gate

Before V1.0:

## A. Acoustic validation

Verify:

```text
LTASS
1/3-octave spectrum
RMS
true peak
modulation
gap distribution
talker occupancy
channel correlation
```

## B. Perceptual validation

Human sentence/word recognition.

## C. Cognitive validation

At minimum:

```text
serial recall
```

because much office-masker research uses it.

## D. Machine validation

At least:

```text
one ASR engine
one speech-enhancement engine
one separation system
```

## E. Comparative baselines

Every BabbleForge mode must be compared against:

```text
silence
generic pink noise
-5 dB/oct noise
speech-shaped stationary noise
```

Otherwise it will be impossible to know if the elaborate babble generator actually offers an advantage.

---

# 62. Most important revised hypothesis

The project should explicitly test this rather than assume it:

> The most effective practical masker may not be pure babble.

Current evidence suggests that stationary speech-shaped noise can be extraordinarily effective because it provides energetic masking without exposing the listener to the amplitude fluctuations and linguistic structure that allow glimpsing or create additional distraction.

Babble, however, offers informational masking and may be perceptually preferable or advantageous under other conditions. Multi-voice masking also has direct evidence for reducing speech-related cognitive disruption.

The scientifically strongest architecture is therefore:

```text
                    MASK ENGINE
                         |
       +-----------------+-----------------+
       |                 |                 |
 Stationary          Multi-Talker        Hybrid
 Speech-Shaped          Babble
       |                 |                 |
       +-----------------+-----------------+
                         |
                 Spectrum Control
                         |
                 Spatial Renderer
                         |
                  Level Controller
                         |
                 Physical Outputs
```

V2 then adds:

```text
Microphone
    |
Acoustic Measurement
    |
Speech Spectrum Learning
    |
Room / Coverage Model
    |
Optimizer
    |
Adaptive Level Control
```

---

# 63. Strongest design conclusion from the expanded research

BabbleForge should not attempt to discover one magical "best babble."

The science instead suggests optimizing a multi-dimensional masker:

```text
Spectrum
+
Level
+
Temporal modulation
+
Talker count
+
Voice similarity
+
Spatial distribution
+
Room acoustics
+
Speech-to-masker ratio
```

The best configuration depends on the environment.

Therefore the architecture must be measurement-driven and experiment-friendly from the first release.

That is more defensible than embedding fixed values that merely sound plausible.