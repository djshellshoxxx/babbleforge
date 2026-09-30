# BabbleForge V1 GUI Specification

Version: 1.0 GUI Design  
Scope: Standalone desktop application  
Primary user: General user with no audio-engineering background  
Secondary user: IT staff, AV technicians, acoustics users, researchers

---

# 1. Core GUI philosophy

The program must be immediately usable without understanding:

- decibels
- LUFS
- LTASS
- SNR
- modulation spectra
- octave bands
- spatial correlation
- speech intelligibility metrics

The default workflow should require only:

1. Select room or area type.
2. Select masking style.
3. Adjust strength if desired.
4. Press Start.

Everything else should be optional.

The main interface therefore has two operating depths:

```text
SIMPLE
ADVANCED
```

`Simple` is the default.

The Simple/Advanced selection must persist between application launches.

Switching modes must never stop audio.

Advanced mode exposes additional controls within the same pages.

---

# 2. Main application layout

Main application window:

```text
┌──────────────────────────────────────────────────────────────┐
│ BabbleForge                            SIMPLE     ⚙          │
├──────────────┬───────────────────────────────────────────────┤
│              │                                               │
│ RUN          │                                               │
│ MASK         │              CURRENT PAGE                     │
│ AREA         │                                               │
│ OUTPUT       │                                               │
│ ANALYSIS     │                                               │
│              │                                               │
│              │                                               │
├──────────────┴───────────────────────────────────────────────┤
│ Audio: Speakers (Realtek)     48 kHz    Engine: READY       │
└──────────────────────────────────────────────────────────────┘
```

Simple mode pages:

```text
RUN
MASK
AREA
OUTPUT
```

Advanced mode adds:

```text
ANALYSIS
```

Settings remains accessible from the gear icon.

---

# 3. RUN page

The Run page is the main everyday operating screen.

It should require no technical knowledge.

Example:

```text
┌─────────────────────────────────────────────────┐
│                                                 │
│                  OPEN OFFICE                    │
│                                                 │
│                Balanced Masking                 │
│                                                 │
│                ┌─────────────┐                  │
│                │    START    │                  │
│                └─────────────┘                  │
│                                                 │
│ Masking Strength                               │
│ Quiet        ───────●─────────       Strong    │
│                                                 │
│ Character                                      │
│ Natural      ─────●───────────       Dense     │
│                                                 │
│ Coverage                                       │
│ ● Stereo                                       │
│ ○ Multi-Speaker                                │
│                                                 │
│ Current activity                               │
│ ███████████████░░░                             │
│                                                 │
│ 7 voices active     Output healthy             │
│                                                 │
└─────────────────────────────────────────────────┘
```

---

# 4. Primary Start control

The largest control in the program is:

```text
START MASKING
```

When running:

```text
STOP MASKING
```

Status directly above or below:

```text
READY
STARTING
MASKING ACTIVE
OUTPUT DEVICE LOST
DEGRADED
```

Elapsed runtime:

```text
02:34:18
```

Optional:

```text
Start automatically with Windows
Start masking when application opens
```

These are configured in Settings, not shown on the main page.

---

# 5. Room/area selector

At the top of the Run screen:

```text
AREA

[ Open Office ▼ ]
```

Available defaults:

```text
Small Room
Office
Conference Room
Open Office
Large Room
Common Area
Reception
Outdoor / Free Field
Custom
```

Selecting a room preset automatically loads recommended:

- masker algorithm
- average active talkers
- density
- spectral curve
- spatial distribution
- output behavior

The user does not need to understand any of those parameters.

---

# 6. Masking Strategy selector

Directly below area:

```text
MASK TYPE

[ Balanced ▼ ]
```

Simple options:

```text
Balanced
Natural
Maximum Density
Speech Noise
Multi-Voice
Hybrid
```

Descriptions appear underneath.

Example:

### Balanced

```text
General-purpose speech masking with a mix of voices
and stable background masking.
```

### Natural

```text
Sounds more like distant conversation.
```

### Maximum Density

```text
Denser masking with fewer quiet gaps.
```

### Speech Noise

```text
Steady speech-shaped noise without recognizable voices.
```

### Multi-Voice

```text
Uses several distinct voices as the main masker.
```

### Hybrid

```text
Combines multi-speaker babble and steady masking.
```

Avoid technical descriptions in Simple mode.

---

# 7. Masking Strength

Simple control:

```text
MASKING STRENGTH

Low ─────────●──────── High
```

The slider does not expose raw digital gain unless Advanced mode is active.

Internally it controls a preset-safe gain range.

Suggested labels:

```text
Gentle
Low
Normal
Strong
Very Strong
```

Default:

```text
Normal
```

No preset should start at its maximum possible output.

---

# 8. Character control

Simple mode should include one highly intuitive macro:

```text
CHARACTER

Natural ─────────●──────── Dense
```

This simultaneously modifies:

- average number of active talkers
- speech overlap
- gap duration
- stationary-noise proportion
- per-talker level variation

At the Natural end:

- more individual conversation-like movement
- slightly greater pauses
- lower simultaneous talker count
- more level variation

At the Dense end:

- more overlapping voices
- fewer gaps
- more stationary masking component
- less noticeable individual talkers

The master output level remains approximately constant while this control changes.

---

# 9. Quick preset cards

Optional cards below the primary controls:

```text
Recommended

┌────────────┐ ┌────────────┐ ┌────────────┐
│ Balanced   │ │ Natural    │ │ Dense      │
│ ●          │ │            │ │            │
└────────────┘ └────────────┘ └────────────┘
```

Selecting a card changes the relevant controls.

Do not add dozens of presets.

The goal is speed.

---

# 10. MASK page — Simple mode

The Mask page controls what the masking signal sounds like.

Simple mode:

```text
Masking Style

● Balanced
○ Speech Noise
○ Multi-Voice
○ Hybrid
○ Natural Babble
```

Under this:

```text
Voice Amount

Few ─────────●──────── Many
```

This maps approximately to:

```text
Few             3–4
Medium          5–8
Many            10–16+
```

Do not show exact numbers until Advanced mode.

---

# 11. Voice Variety

Simple control:

```text
VOICE VARIETY

Low ─────────●──────── High
```

Affects how different the talker pool is.

Higher variation changes:

- vocal range
- speech cadence
- speaker identity
- prosodic characteristics

Default:

```text
High
```

Tool tip:

```text
Controls how different the voices in the babble sound from one another.
```

---

# 12. Understandable Speech control

Useful nontechnical control:

```text
CLEAR VOICE REDUCTION

Low ─────────●──────── High
```

This does not perform destructive voice processing.

Instead it adjusts:

- simultaneous talker count
- segment scheduling
- overlap
- source selection
- stationary-mask contribution

Purpose:

Reduce the likelihood that one isolated phrase becomes prominent.

Default:

```text
High
```

This is easier for general users to understand than:

```text
Informational masking density
```

---

# 13. Noise/Babble balance

Only visible for Hybrid mode.

Simple:

```text
MASK MIX

Steady ─────────●──────── Voices
```

Left:

more stationary speech-shaped masking.

Right:

more multi-talker babble.

Center:

balanced hybrid.

Internally:

```text
0–100% energy mix
```

but do not expose percentages in Simple mode.

---

# 14. AREA page

This page configures the physical environment.

Simple screen:

```text
AREA TYPE

[ Open Office ▼ ]

Approximate Area Size

Small
Medium
Large

Speaker Setup

● 2 Speakers
○ 4 Speakers
○ More than 4
```

Selecting these values adjusts the default spatial configuration.

---

# 15. Small Room preset

Simple description:

```text
For private offices, small meeting rooms,
and other compact enclosed spaces.
```

Recommended internal starting behavior:

```text
Talkers:                4–6
Density:                medium
Stationary component:   moderate
Spatial spread:         medium
Low-frequency energy:   controlled
```

---

# 16. Office preset

For:

```text
1–4 person offices
```

Suggested:

```text
Talkers:                5–7
Character:              balanced
Spatial spread:         medium
Noise contribution:     moderate
```

---

# 17. Conference Room preset

Prioritize:

- even coverage around a table
- low apparent source localization
- moderate talker density

Suggested:

```text
Talkers:                5–8
Hybrid masking:         moderate
Output channels:        2–4+
```

---

# 18. Open Office preset

Prioritize:

- distributed masking
- uniform output
- low channel correlation
- low recognition of individual voices

Suggested:

```text
Talkers:                7–12
Density:                high
Stationary contribution: moderate-high
Spatial spread:         broad
```

---

# 19. Large Room preset

Prioritize:

```text
multiple speakers
coverage
channel independence
dense masker
```

Suggested:

```text
Talkers:                10–16
Spatial zones:          supported
Stationary contribution: moderate
```

---

# 20. Reception/Common Area preset

More natural-sounding.

Suggested:

```text
Talkers:                8–14
Density:                medium
Natural movement:       high
Stationary contribution: low
```

The intention is to resemble distant background activity rather than obvious generated noise.

---

# 21. Outdoor / Free Field preset

Interface warning:

```text
Outdoor environments usually require multiple speakers
for effective coverage.
```

Simple controls:

```text
Coverage Area

Small
Medium
Large

Speaker Layout

2
4
6
8+
```

Internally:

- room reflection assumptions disabled
- spatial spread increased
- higher talker density
- stationary contribution available
- speaker distribution prioritized

---

# 22. OUTPUT page — Simple mode

Purpose:

Select where audio goes.

Example:

```text
OUTPUT DEVICE

[ Focusrite USB ASIO ▼ ]

MODE

● Stereo
○ Multi-Speaker

MASTER LEVEL

────────●────────

Left       ███████░
Right      ███████░

Status: Healthy
```

---

# 23. Output-device test

Button:

```text
TEST SPEAKERS
```

Produces:

```text
Left
Right
```

or:

```text
Speaker 1
Speaker 2
Speaker 3
...
```

using short identification signals.

There should be a prominent:

```text
STOP TEST
```

control.

---

# 24. Basic output meter

Simple mode shows:

```text
LOW
GOOD
HIGH
```

rather than requiring users to interpret dBFS.

Example:

```text
OUTPUT

██████████████░░░░

GOOD
```

Advanced mode exposes actual values.

---

# 25. Advanced mode activation

Top right:

```text
SIMPLE   [ ADVANCED ]
```

First activation displays one short explanation:

```text
Advanced mode exposes detailed audio and masking controls.

Changing advanced settings can alter the behavior of the selected preset.

[Enable Advanced]
```

Do not show the message again unless requested.

---

# 26. Advanced MASK controls

Advanced Mask page adds a collapsible:

```text
Talker Engine
```

panel.

Controls:

```text
Talker Pool             16
Average Active          7.0
Minimum Active          5
Maximum Active          10
```

Ranges:

```text
Pool:                   2–64
Average Active:         1–32
```

Preset values should remain constrained to sane ranges.

---

# 27. Talker activity controls

Advanced:

```text
Maximum Internal Gap        120 ms

Minimum Segment Length      2 s
Maximum Segment Length      15 s

Talker Gain Variation       ±2.5 dB

Fade Time                   150 ms
```

These belong inside:

```text
Talker Timing
```

and are collapsed by default.

---

# 28. Speech activity density

Advanced control:

```text
ACTIVITY DENSITY

65%
```

Also display:

```text
Average simultaneous talkers: 7.3
```

Do not let the user accidentally create invalid combinations.

For example:

```text
Minimum active <= Average active <= Maximum active
```

The GUI should enforce this automatically.

---

# 29. Stationary masking controls

Advanced:

```text
STATIONARY MASKER

Enabled             ✓

Energy Contribution  15%

Spectrum

● Match Speech
○ LTASS
○ Privacy Curve
○ Custom

Noise Seed
[ Random ]
```

If disabled:

all dependent controls grey out.

---

# 30. Spectral controls

Advanced panel:

```text
SPECTRUM
```

Modes:

```text
Speech Matched
Universal LTASS
Privacy -5 dB/oct
Privacy -7 dB/oct
Privacy -9 dB/oct
Custom
```

Visual graph:

```text
dB
 |
 |      ───── target
 |     /
 |    /
 |___/________________
     frequency
```

Simple presets should never require editing this.

---

# 31. Custom spectrum

If Custom selected:

Use a small graphic EQ.

Recommended bands:

```text
125
250
500
1k
2k
4k
8k
```

This is preferable to exposing a 31-band EQ initially.

Advanced users get enough control without clutter.

Maximum correction:

```text
±12 dB
```

Default recommended user range:

```text
±6 dB
```

---

# 32. Spectral target correction

Advanced toggle:

```text
Maintain Target Spectrum    ON
```

Controls:

```text
Correction Speed

Slow
Normal
Fast
```

Internally:

```text
slow spectral averaging
```

Tool tip:

```text
Gradually corrects long-term spectral drift without changing
the natural short-term movement of speech.
```

---

# 33. Spatial controls

Advanced AREA page gains:

```text
SPATIAL OUTPUT
```

Modes:

```text
Mono
Stereo
4 Channel
6 Channel
8 Channel
Custom
```

---

# 34. Spread

Control:

```text
SPATIAL SPREAD

Narrow ─────────●──────── Wide
```

Controls voice distribution across physical outputs.

Default depends on area preset.

---

# 35. Channel independence

Simple name:

```text
Speaker Variation
```

Advanced technical label beneath:

```text
Channel decorrelation
```

Control:

```text
Low
Medium
High
```

Default:

```text
Medium or High
```

for distributed installations.

---

# 36. Speaker assignment

Advanced page shows:

```text
Speaker 1   Left Front
Speaker 2   Right Front
Speaker 3   Rear Left
Speaker 4   Rear Right
```

Each has:

```text
Enabled
Level
Delay
```

Example:

```text
Speaker 3

Enabled     ✓
Level       -1.5 dB
Delay       3.2 ms
```

Delay is optional in V1.

---

# 37. Zone support

V1 Advanced can include lightweight zoning without full V2 calibration.

Example:

```text
ZONE A
Channels 1–4

ZONE B
Channels 5–8
```

Each zone has:

```text
Enable
Relative Level
Masking Style
```

Do not expose separate independent DSP engines per zone in V1 unless needed.

The main purpose is level grouping.

---

# 38. Advanced output page

Adds:

```text
Driver
Sample Rate
Buffer Size
Channel Count
```

Example:

```text
Driver        ASIO
Sample Rate   48000 Hz
Buffer        256 samples
Channels      8
```

Status:

```text
Latency        5.3 ms
Audio Dropouts 0
```

---

# 39. Master-level technical meters

Advanced mode:

```text
RMS
-21.3 dBFS

LUFS-S
-20.8 LUFS

True Peak
-4.1 dBTP

Limiter
0.0 dB
```

These should not appear in Simple mode.

---

# 40. Limiter controls

Advanced only:

```text
OUTPUT LIMITER

Enabled          ON
Ceiling          -1.0 dBTP
```

The limiter should be impossible to fully bypass from Simple mode.

If user disables it in Advanced mode:

display:

```text
Limiter disabled
```

in the status bar.

---

# 41. ANALYSIS page

Advanced mode only.

This should be informative rather than operational.

Four cards:

```text
VOICE ACTIVITY
SPECTRUM
OUTPUT
SPATIAL
```

---

# 42. Voice Activity card

Displays:

```text
Active Talkers        8
Average Talkers       7.4
Speech Occupancy      96%
Average Gap           72 ms
Longest Recent Gap    181 ms
```

Small scrolling visualization:

```text
V1  ███    █████
V2     █████
V3  ███████
V4       ███████
```

---

# 43. Spectrum card

Switch:

```text
Octave
1/3 Octave
FFT
```

Overlay:

```text
Target
Actual
```

Metrics:

```text
Average target error    1.2 dB
Largest deviation       2.7 dB at 250 Hz
```

---

# 44. Modulation card

Keep this simple even in Advanced mode.

Display:

```text
Temporal Density

Low     Medium     High
                 ●
```

Additional technical view:

```text
Modulation Spectrum
0.5–16 Hz
```

Hidden behind:

```text
Show details
```

---

# 45. Spatial card

Show speaker activity:

```text
        [1]
     71%

[4]          [2]
68%          72%

        [3]
     69%
```

Also:

```text
Channel correlation

Low / Good
```

Avoid raw correlation coefficients unless:

```text
Show details
```

is expanded.

---

# 46. Preset system

Every preset has three states:

```text
Factory
Modified
User Preset
```

Example:

```text
Open Office / Balanced
Modified
```

Button:

```text
Reset to Recommended
```

---

# 47. Save preset

Advanced or Simple:

```text
Save As Preset
```

Dialog:

```text
Name:
Server Room Privacy

Based on:
Open Office

[Save]
```

Presets save:

- area
- mask strategy
- strength
- character
- output configuration
- advanced parameters

They do not save audio-device identity by default unless user selects:

```text
Remember output device with preset
```

---

# 48. Preset safety comparison

When advanced changes significantly diverge from factory settings:

```text
This configuration differs significantly from
the recommended Open Office preset.

[Review Changes]
[Keep Changes]
[Reset]
```

Do not block the user.

---

# 49. Undo/Redo

Important for advanced editing.

```text
Ctrl+Z
Ctrl+Shift+Z
```

Changes to:

- sliders
- presets
- speaker routing
- spectrum

should be undoable.

---

# 50. Settings page

Sections:

```text
Audio
Startup
Corpus
Appearance
Logging
Advanced
```

---

# 51. Startup settings

```text
Launch at system startup
Start minimized
Automatically start masking
Restore previous configuration
```

If auto-start masking enabled:

require:

```text
Remember output device
```

---

# 52. Corpus settings

Simple:

```text
VOICE LIBRARY

Status: Ready

Talkers: 48
Usable speech: 11h 23m

[Manage Library]
```

---

# 53. Manage Library

Advanced dialog:

```text
Talkers            48
Audio files        382
Total duration     13h 41m
Usable speech      11h 23m
Invalid files      0
```

Buttons:

```text
Add Audio
Scan Library
Rebuild Analysis
```

---

# 54. Corpus import wizard

Steps:

```text
1 Select Files
2 Analyze
3 Review
4 Add
```

Automatic analysis:

- speech detection
- clipping
- silence
- level
- sample rate
- duplicate detection

Simple result:

```text
138 files analyzed

132 Good
4 Usable
2 Rejected
```

Technical details only visible if expanded.

---

# 55. Help system

Every technical control should have:

```text
?
```

or tooltip.

Descriptions should explain results rather than terminology.

Bad:

```text
Adjust modulation index.
```

Better:

```text
Controls how much the masking level naturally rises and falls over time.
```

---

# 56. Contextual recommendations

The application should occasionally provide useful, nonintrusive suggestions.

Example:

```text
You selected a Large Room with only 2 output speakers.

Coverage may be uneven.

[Continue]
```

Or:

```text
This preset works best with at least 4 independent speakers.
```

No modal warning unless the configuration is actually invalid.

---

# 57. Advanced changes indicator

When Advanced mode changes factory values:

```text
ADVANCED SETTINGS MODIFIED
```

Clicking it opens:

```text
Changed from Open Office default:

Talkers            8 → 12
Noise contribution 15% → 5%
Spatial spread     70% → 95%
```

This makes experimentation reversible.

---

# 58. Device failure behavior

If an audio device disappears:

Large status:

```text
OUTPUT DEVICE LOST
```

The software should:

1. stop audio cleanly;
2. retain current state;
3. attempt safe reconnection;
4. display available devices.

Buttons:

```text
Reconnect
Choose Device
```

Do not silently jump to another output device and unexpectedly play audio.

---

# 59. Engine degradation status

If one source file fails:

Do not interrupt the user.

Status remains:

```text
MASKING ACTIVE
```

If many resources fail:

```text
MASKING ACTIVE
Reduced Voice Library
```

Click for details.

---

# 60. First-run wizard

Keep extremely short.

Page 1:

```text
Welcome to BabbleForge
```

Page 2:

```text
Select Output

[Speakers ▼]

[Test]
```

Page 3:

```text
Choose Your Area

Small Room
Office
Conference
Open Office
Large Room
Outdoor
```

Page 4:

```text
Recommended Setup

Open Office
Balanced
Normal Strength

[Start]
```

No account required.

No mandatory tutorial.

---

# 61. Status bar

Persistent bottom bar:

Simple:

```text
Output: Focusrite USB     Engine: Ready     CPU: Normal
```

Advanced:

```text
ASIO | 48 kHz | 256 samples | CPU 2.1% | XRuns 0 | 8 outputs
```

---

# 62. Keyboard shortcuts

```text
Space           Start / Stop
Ctrl+1          Run
Ctrl+2          Mask
Ctrl+3          Area
Ctrl+4          Output
Ctrl+5          Analysis
Ctrl+S          Save preset
Ctrl+Z          Undo
Ctrl+Shift+Z    Redo
F1              Help
```

Space should not trigger Start/Stop while a text field is being edited.

---

# 63. Visual priority

The UI should visually emphasize only four things:

```text
1. Area
2. Mask type
3. Start/Stop
4. Strength
```

Everything else has lower visual priority.

This prevents advanced capabilities from making the product appear complicated.

---

# 64. Recommended primary workflow

A normal user should experience:

```text
Open BabbleForge

        ↓

Choose:
Open Office

        ↓

Preset automatically selects:
Balanced

        ↓

Set Strength:
Normal

        ↓

START
```

Total required decisions:

```text
2–3
```

---

# 65. Recommended advanced workflow

Technical user:

```text
Select Open Office

        ↓

Enable Advanced

        ↓

Choose Hybrid

        ↓

7 average talkers

        ↓

50/50 stationary/babble

        ↓

Speech-Matched spectrum

        ↓

4-output distributed mode

        ↓

Inspect analyzer

        ↓

Save custom preset
```

---

# 66. V1 page hierarchy

Final recommended interface:

```text
BabbleForge
│
├── RUN
│   ├── Area
│   ├── Mask Type
│   ├── Start / Stop
│   ├── Strength
│   └── Character
│
├── MASK
│   ├── Style
│   ├── Voice Amount
│   ├── Voice Variety
│   ├── Clear Voice Reduction
│   └── Advanced
│       ├── Talker Scheduler
│       ├── Stationary Mask
│       ├── Spectrum
│       └── Timing
│
├── AREA
│   ├── Area Type
│   ├── Area Size
│   ├── Speaker Count
│   └── Advanced
│       ├── Spatial Spread
│       ├── Channel Variation
│       ├── Zones
│       └── Speaker Mapping
│
├── OUTPUT
│   ├── Device
│   ├── Test
│   ├── Level
│   └── Advanced
│       ├── Driver
│       ├── Sample Rate
│       ├── Buffer
│       ├── Channels
│       ├── Meters
│       └── Limiter
│
├── ANALYSIS
│   ├── Voices
│   ├── Spectrum
│   ├── Modulation
│   └── Spatial
│
└── SETTINGS
    ├── Audio
    ├── Startup
    ├── Voice Library
    ├── Appearance
    └── Diagnostics
```

---

# 67. Controls deliberately excluded from V1

To keep the application usable, V1 should not expose:

- individual compressor controls
- individual EQ for every talker
- per-voice pitch shifting
- manual FFT window selection
- arbitrary crossover networks
- multiband compression
- convolution editor
- HRTF editing
- detailed room simulation
- SII configuration
- STI measurement
- automatic room EQ
- microphone calibration
- measurement-position mapping
- adaptive ambient-level masking

These belong either internally or in V2.

---

# 68. Recommended default experience

On first launch:

```text
Area:
Office

Mask:
Balanced

Strength:
Normal

Character:
60% Dense

Outputs:
Stereo

Advanced:
Off
```

The application should already sound deliberate and usable at these defaults.

A new user should not need to change anything except:

```text
Area
```

and possibly:

```text
Strength
```

---

# 69. Core usability requirement

A successful V1 UI satisfies all three user levels.

### New user

Can operate BabbleForge without understanding audio terminology.

### Experienced user

Can substantially change masker behavior using a manageable set of meaningful controls.

### Research/technical user

Can inspect exact talker counts, spectral behavior, temporal statistics and output routing without the research controls overwhelming the normal application.

The guiding rule should therefore be:

**Expose the effect first and the acoustic parameter second.**