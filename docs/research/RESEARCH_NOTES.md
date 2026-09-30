# BabbleForge Research Notes

Research date: September 2026

## Purpose

These notes document the scientific basis behind BabbleForge.

They are not a substitute for the complete text of the cited standards. Where an implementation depends on exact equations or conformity with a standard, the project should obtain and work from the applicable official standard.

---

# 1. Multi-talker babble is not equivalent to arbitrary noise

Rosen, Souza, Ekelund and Majeed compared natural babble containing 1, 2, 4, 8 and 16 talkers with vocoded babble, envelope-modulated noise and steady noise.

Important findings:

- all maskers were adjusted to have the same long-term spectrum
- natural speech was consistently an especially effective masker
- some of the greatest changes occurred between 1 and 2–4 talkers
- increasing talker count reduces temporal opportunities to glimpse target speech
- informational and energetic masking interact
- a greater number of talkers does not produce a simple monotonic improvement in all experimental conditions

Their babble construction also removed speech pauses exceeding approximately 100 ms and RMS-normalized individual talkers before mixture construction.

Design consequence:

BabbleForge should independently control spectrum, level, talker count and temporal density.

---

# 2. Informational masking depends strongly on speaker count

Freyman, Balakrishnan and Helfer studied 3, 4, 6 and 10 interfering speakers and found that the number of masking speakers changes informational masking and spatial-release behavior.

Other speech-on-speech experiments likewise find that small numbers of competing voices can produce particularly important informational-masking effects.

Design consequence:

Do not use "maximum number of speakers" as the sole optimization strategy.

Provide experimentally useful counts:

```text
2
4
6
8
12
16
```

---

# 3. Large talker populations approach speech-shaped noise

As additional independent speech sources overlap, aggregate babble becomes less deeply modulated and more acoustically similar to stationary speech-shaped noise.

Design consequence:

The application should distinguish:

```text
few-talker informational masker
balanced babble
dense energetic masker
speech-shaped noise
```

instead of treating them as equivalent.

---

# 4. Long-term average speech spectrum

Byrne et al. measured long-term average speech spectra for numerous languages.

Although statistically significant differences existed, the broad LTASS was similar across languages and the authors proposed a universal long-term speech spectrum for many acoustic applications.

Design consequence:

BabbleForge can use a standard/reference LTASS as a stable baseline while retaining optional corpus-specific spectral profiles.

---

# 5. Temporal modulation is important

Speech intelligibility depends strongly on speech-envelope information.

IEC 60268-16's STI model is based on preservation of speech intensity modulation across octave bands and modulation frequencies. The standard covers octave bands from 125 Hz through 8 kHz and modulation rates roughly 0.63–12.5 Hz.

Separate research shows that amplitude modulation of a masker changes masking release and speech recognition.

Design consequence:

BabbleForge should measure its modulation behavior rather than only FFT magnitude.

---

# 6. Speech privacy is fundamentally spatial

ASTM E1130 describes open-plan speech privacy as dependent on:

- source vocal effort/orientation
- distance attenuation
- intervening barriers
- room reflections
- ambient/background sound

The masking system changes only part of that acoustic system.

Design consequence:

A digital preset cannot guarantee a particular room result.

This is the main motivation for V2 calibration.

---

# 7. Spatial separation can release a listener from masking

Experiments on speech masking show substantial improvements in recognition when target and masker are perceived at different locations.

Design consequence:

A masker emitted from one obvious loudspeaker can be easier to perceptually separate from the target.

Prefer spatially distributed, low-correlation output.

---

# 8. Spatial uniformity matters

Zarei et al. measured a carefully configured open-office sound-masking system.

Key findings included:

- masking uniformity varies significantly with position
- one-third-octave spectral consistency is harder to achieve than broadband level consistency
- number of masking loudspeakers substantially affects spatial uniformity
- the authors relate masking-field variation to resulting speech intelligibility/privacy

Their field measurements indicate that very tight spectral tolerances throughout a real room are difficult even with good equipment.

Design consequence:

V2 requires multiple measurement positions.

One microphone measurement cannot adequately characterize a large room.

---

# 9. Overall masking level has practical limits

Multiple office studies place practical electronic masking levels roughly in the low-to-mid 40 dBA region.

A long-duration field study used masking below approximately 45 dBA and a spectrum near -5 dB/octave but found no universal subjective benefit; it specifically cautioned that 45 dBA may itself be too high for some environments.

A separate long-term office experiment operated around 43–45 dBA and found conventional pseudo-random masking preferable to several water-based alternatives.

A more recent adaptive-masking field study raised previously quiet environments into approximately the low 40 dBA range and reported reduced intelligible-speech distraction.

Design consequence:

The objective should be the **lowest masking level that achieves the configured privacy objective**, rather than automatically targeting 45–48 dBA.

---

# 10. Spectrum should be configurable

Literature frequently describes downward-sloping masking spectra.

One long-term field study describes approximately -5 dB/octave as a commonly used recommendation.

More recent literature cites other slopes, demonstrating that there is no justification for treating one curve as universally optimal.

Design consequence:

Ship research-informed curves but retain an editable spectrum.

---

# 11. Naturalistic sounds are not automatically superior

Water-derived masking sounds have been proposed as more pleasant alternatives to conventional masking noise.

A long-term office experiment did not find them superior to pseudo-random masking and found several conditions more distracting.

A 2026 study reports some benefits from natural masking conditions, illustrating that this remains an active area rather than settled science.

Design consequence:

Do not add water/rain/nature masking to the scientific default simply because it sounds pleasant.

It can become an experimental module later.

---

# 12. STI

IEC 60268-16:2020 defines the Speech Transmission Index model and measurement/prediction techniques.

STI represents degradation of speech-envelope information through the transmission path.

Important caveat:

The IEC notes limitations where background noise fluctuates, including babble-like interference. Earlier descriptions likewise note increased measurement variability in fluctuating noise.

Design consequence:

Do not claim certified STI measurements from BabbleForge V2 until its implementation is validated against conforming instrumentation.

---

# 13. ISO 3382-3

ISO 3382-3:2022 covers acoustic measurements in open-plan offices.

Important quantities include:

```text
D2,S   spatial decay rate of speech
Lp,A,S,4m
Lp,B   background level
STI
rD     distraction distance
```

The standard defines distraction distance as the shortest distance at which STI falls below 0.50.

Design consequence:

The eventual calibration subsystem should work with similar spatial concepts rather than producing only a single "room score."

---

# 14. Speech Intelligibility Index

ASA/ANSI S3.5-1997 (R2024) defines SII, a physical measure derived from speech and noise spectral information that correlates with intelligibility across groups of talkers and listeners.

Design consequence:

SII is potentially useful in V2 because BabbleForge will already measure one-third-octave speech and masker levels.

Exact standards-conforming implementation should use the purchased/current standard rather than reconstructed equations from secondary sources.

---

# 15. Open-office babble

Research specifically examining background babble in offices confirms that simultaneous voices can act as a natural masker, but the cognitive and acoustic effects are complex rather than simply improving with every additional voice.

Design consequence:

The presets should control both effectiveness and perceptual acceptability.

---

# 16. Why the hybrid architecture was chosen

Pure prerecorded crowd recording:

Advantages:

- realistic
- simple

Weaknesses:

- fixed spectral properties
- fixed talker count
- fixed temporal behavior
- obvious loops
- difficult multichannel decorrelation
- poor reproducibility

Fully synthetic/procedural speech:

Advantages:

- unlimited
- controllable

Weaknesses:

- uncertain correspondence with published human-speech babble research
- synthesis artifacts
- risk that multiple synthetic voices share correlated characteristics

Hybrid independent real talkers plus controlled noise:

Advantages:

- directly matches research paradigm
- controllable talker number
- controlled LTASS
- controllable gaps
- controlled modulation
- controllable multichannel placement
- noise layer fills excessive temporal minima
- experimentally reproducible

This is therefore the recommended architecture.

---

# 17. Evidence confidence

## High confidence

- competing speech is an effective masker
- number of competing talkers matters
- natural speech and stationary noise behave differently
- long-term spectrum matters
- temporal modulation matters
- spatial separation matters
- room acoustics strongly affect privacy
- spatial masking uniformity matters

## Moderate confidence

- broad low/mid-heavy downward-sloping masking curves are useful
- low-to-mid 40 dBA masking levels are practical in many office situations
- hybrid babble plus speech-shaped noise will provide greater stability than uncontrolled babble

## Requires project validation

- exact optimum number of voices for each BabbleForge preset
- exact optimum noise/babble energy ratio
- exact preset spectral slope
- exact outdoor settings
- exact target level for arbitrary rooms
- relationship between BabbleForge's predicted privacy metric and real listeners
- resistance of generated masks to modern speech-separation algorithms

Those parameters should therefore be experimentally tuned instead of presented as settled science.

---

# 18. Primary standards and literature to keep in the project bibliography

ASA/ANSI S3.5-1997 (R2024), Methods for Calculation of the Speech Intelligibility Index.

IEC 60268-16:2020, Objective Rating of Speech Intelligibility by Speech Transmission Index.

ISO 3382-3:2022, Acoustics — Measurement of Room Acoustic Parameters — Part 3: Open Plan Offices.

ASTM E1130-16(2021), Objective Measurement of Speech Privacy in Open Plan Spaces.

Rosen et al., Listening to Speech in a Background of Other Talkers: Effects of Talker Number and Noise Vocoding.

Freyman et al., Effect of Number of Masking Talkers and Auditory Priming on Informational Masking in Speech Recognition.

Byrne et al., An International Comparison of Long-Term Average Speech Spectra.

Zarei et al., Evaluation of the Uniformity of Sound-Masking Systems in an Open-Plan Office.

Lenne et al., Long-Term Effects of the Use of a Sound Masking System in Open-Plan Offices.

Hongisto et al., Perception of Water-Based Masking Sounds — Long-Term Experiment in an Open-Plan Office.