# tools/validation: independent Python cross-check of bfanalyze

A from-scratch Python re-implementation of the `bfanalyze` metrics (docs/VALIDATION.md 2.2, 5; SPECTRUM_ENGINE 5, 8),
plus a fixture driver for `bfrender`, a Markdown report and tests. It never calls C++ code except by running the
`bfrender` binary to produce audio (fixtures.py) and, in the integration test, optionally `bfanalyze` for comparison.

Metrics: RMS, 1/3-octave and octave levels (IEC 61260 style base-10 Butterworth filterbank, 6th-order
band-pass, scipy SOS), LTASS deviation, 10 ms envelope, gap statistics (threshold Leq10s - 12 dB), L10-L90,
modulation spectrum 0.5-16 Hz (SPECTRUM_ENGINE 8.3), channel correlation, talker statistics from `*.events.json`.

## Setup

```bash
pip install numpy scipy pytest soundfile matplotlib   # soundfile/matplotlib optional
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBF_BUILD_TESTS=OFF
cmake --build build --target bfrender bfanalyze
```

## Tests

```bash
python3 -m pytest tools/validation -q          # unit tests + (if build/tools/bfrender/bfrender exists) integration
BFRENDER=/path/bfrender BFANALYZE=/path/bfanalyze python3 -m pytest tools/validation -q
```

## Fixtures (VALIDATION 5), compare, report

```bash
cd tools/validation
python3 -m bfval.fixtures write-templates                     # regenerate scenarios/*.json
python3 -m bfval.fixtures render --bfrender ../../build/tools/bfrender/bfrender \
    --out-dir /tmp/fx --synthetic-corpus 32 --only B1,B4,SSN-L,PINK --seeds 1001 --duration 60
#   real corpus: replace --synthetic-corpus 32 with --corpus <root>; all fixtures/seeds: drop --only/--seeds/--duration
python3 -m bfval.report --fixtures /tmp/fx --out /tmp/fx/report.md --plots

# one file, Python vs C++:
../../build/tools/bfanalyze/bfanalyze /tmp/fx/B4_s1001.wav --events /tmp/fx/B4_s1001.events.json --out /tmp/fx/cpp.json
python3 - <<'PY'
import sys; sys.path.insert(0, ".")
from bfval import wavio, metrics, events, compare
x, fs = wavio.read_wav("/tmp/fx/B4_s1001.wav")
py = metrics.analyze(x, fs)
tk = events.talker_stats(events.load_events("/tmp/fx/B4_s1001.events.json"), x.shape[1], fs)
print(compare.summarize(compare.compare(compare.load_metrics("/tmp/fx/cpp.json"), py, compare.TOL_BABBLE, talkers_py=tk)))
PY
```

`scenarios/*.json` are bfrender scenarios (B1-B5, B7, B8, B16, SSN-L, SL5/7/9, PINK, H25/50/75, NAT/BAL/DEN);
seed and (optionally) duration are overridden at render time. Full 300-600 s fixtures take minutes each.

## Notes

- Band levels are mean-square band power; channel powers are added (as in bfanalyze). The 1/3-octave filterbank vs
  FFT-aggregation difference is ~0.2-0.3 dB (stationary) and up to ~0.8 dB (babble) per band; compare.py tolerances reflect that.
- Measured SSN modulation floor at 8-16 Hz is ~0.1-0.15 (both implementations), above the < 0.05 quoted in
  SPECTRUM_ENGINE 8.3; at 4 Hz it is < 0.05.
- Tuning workflow (VALIDATION 9): see `tools/tuning/tune.py`.
