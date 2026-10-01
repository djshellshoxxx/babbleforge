"""Scientific validation fixtures (docs/VALIDATION.md 5): scenario templates + a bfrender driver.

    python -m bfval.fixtures write-templates [--dir scenarios]
    python -m bfval.fixtures render --bfrender build/tools/bfrender/bfrender --out-dir out/fixtures
           [--synthetic-corpus 32 | --corpus <root>] [--only B4,SSN-L] [--seeds 1001,1002]
           [--duration 60] [--data-dir resources/data]

Templates are plain bfrender scenarios (docs/PRESETS.md 6.3; fields accepted by tools/bfrender: schema, preset,
seed, durationS, sampleRate, laboratory{mode,talkers,maxGapMs,babbleFraction,rmsNormalization,spectrumMatch,
limiter}). seed (and optionally durationS) are filled in at render time. This module never calls bfanalyze.
"""
import argparse
import copy
import json
import subprocess
import sys
from pathlib import Path

SEEDS = [1001, 1002, 1003, 1004, 1005]
DEFAULT_SCENARIO_DIR = Path(__file__).resolve().parent.parent / "scenarios"


def _lab(mode, **kw):
    return {"mode": mode, "rmsNormalization": "two-pass", **kw}


def _scenario(name, desc, duration, laboratory=None, area="office", strategy="research", layout="mono", spectrum=None):
    preset = {"name": f"validation-{name}", "area": area, "strategy": strategy, "outputs": {"layout": layout}}
    if spectrum:
        preset["spectrum"] = {"target": spectrum}
    sc = {"schema": "babbleforge.scenario", "schemaVersion": "1.0", "x-fixture": name, "x-description": desc,
          "preset": preset, "seed": SEEDS[0], "durationS": duration, "sampleRate": 48000,
          "outputFormat": {"container": "wav", "sampleFormat": "float32"}}
    if laboratory:
        sc["laboratory"] = laboratory
    return sc


def fixture_table():
    t = {}
    for n in (1, 2, 3, 4, 5, 7, 8, 16):
        t[f"B{n}"] = _scenario(f"B{n}", f"LaboratoryMask continuousN N={n}, maxGap 100 ms, nested speaker sets", 300,
                               _lab("continuousN", talkers=n, maxGapMs=100, spectrumMatch="strict"))
    t["SSN-L"] = _scenario("SSN-L", "Stationary speech-shaped noise, Universal LTASS", 300, _lab("ssn"),
                           spectrum="ltass_universal")
    for s in (5, 7, 9):
        t[f"SL{s}"] = _scenario(f"SL{s}", f"Stationary noise, -{s} dB/oct", 300, _lab("ssn"), spectrum=f"slope_-{s}")
    t["PINK"] = _scenario("PINK", "Pink reference noise", 300, _lab("pink"), spectrum="pink")
    for h in (25, 50, 75):
        t[f"H{h}"] = _scenario(f"H{h}", f"Hybrid, 8-talker babble, b={h / 100:.2f}", 300,
                               _lab("hybrid", talkers=8, maxGapMs=100, babbleFraction=h / 100, spectrumMatch="strict"))
    for tag, strat in (("NAT", "natural"), ("BAL", "balanced"), ("DEN", "dense")):
        t[tag] = _scenario(tag, f"Office stochastic {strat}", 600, None, area="office", strategy=strat, layout="stereo")
    return t


def write_templates(directory=DEFAULT_SCENARIO_DIR):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    for name, sc in fixture_table().items():
        (directory / f"{name}.json").write_text(json.dumps(sc, indent=2) + "\n")
    return sorted(fixture_table())


def load_template(name, directory=DEFAULT_SCENARIO_DIR):
    return json.loads((Path(directory) / f"{name}.json").read_text())


def render_fixture(bfrender, name, seed, out_dir, corpus_args, duration=None, scenario_dir=DEFAULT_SCENARIO_DIR,
                   data_dir=None, extra=()):
    """Render one fixture/seed; returns dict(name, seed, wav, events, sidecar). Raises on bfrender failure."""
    sc = copy.deepcopy(load_template(name, scenario_dir))
    sc["seed"] = int(seed)
    if duration:
        sc["durationS"] = float(duration)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = f"{name}_s{seed}"
    spath = out_dir / f"{stem}.scenario.json"
    spath.write_text(json.dumps(sc, indent=2))
    wav, ev = out_dir / f"{stem}.wav", out_dir / f"{stem}.events.json"
    cmd = [str(bfrender), "--scenario", str(spath), *corpus_args, "--out", str(wav), "--events", str(ev), *extra]
    if data_dir:
        cmd += ["--data-dir", str(data_dir)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"bfrender failed for {stem} (exit {r.returncode}): {r.stderr.strip()}")
    return {"name": name, "seed": seed, "wav": wav, "events": ev, "sidecar": out_dir / f"{stem}.sidecar.json"}


def render_all(bfrender, out_dir, corpus_args, names=None, seeds=SEEDS, duration=None, **kw):
    names = names or sorted(fixture_table())
    return [render_fixture(bfrender, n, s, out_dir, corpus_args, duration, **kw) for n in names for s in seeds]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("write-templates")
    w.add_argument("--dir", default=str(DEFAULT_SCENARIO_DIR))
    r = sub.add_parser("render")
    r.add_argument("--bfrender", default="build/tools/bfrender/bfrender")
    r.add_argument("--out-dir", required=True)
    g = r.add_mutually_exclusive_group(required=True)
    g.add_argument("--corpus")
    g.add_argument("--synthetic-corpus", type=int)
    r.add_argument("--only", default="")
    r.add_argument("--seeds", default=",".join(map(str, SEEDS)))
    r.add_argument("--duration", type=float)
    r.add_argument("--data-dir")
    r.add_argument("--scenario-dir", default=str(DEFAULT_SCENARIO_DIR))
    a = ap.parse_args(argv)
    if a.cmd == "write-templates":
        print("wrote", ", ".join(write_templates(a.dir)))
        return 0
    corpus = ["--corpus", a.corpus] if a.corpus else ["--synthetic-corpus", str(a.synthetic_corpus)]
    names = [n for n in a.only.split(",") if n] or None
    res = render_all(a.bfrender, a.out_dir, corpus, names, [int(s) for s in a.seeds.split(",")], a.duration,
                     scenario_dir=a.scenario_dir, data_dir=a.data_dir)
    for x in res:
        print(x["wav"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
