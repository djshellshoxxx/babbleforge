"""Markdown validation report over a directory of rendered fixtures (docs/VALIDATION.md 5).

    python -m bfval.report --fixtures out/fixtures --out report.md [--bfanalyze path] [--plots]

For each <name>_s<seed>.wav the Python metrics are computed; if the render's sidecar holds bfanalyze metrics
they are compared (compare.py). Checks the VALIDATION 5 expected trends: modulation falls monotonically with N,
SSN modulation at the floor (< 0.05), octave LTASS deviation <= 1 dB. Plots (matplotlib) only if available.
bfanalyze is never executed by this module; it only reads the metrics bfrender stored in sidecars.
"""
import argparse
import json
import re
import sys
from pathlib import Path

import numpy as np

from . import compare as cmp
from . import events as evmod
from . import metrics as M
from . import wavio

TARGET_DIR = Path(__file__).resolve().parents[3] / "resources" / "data" / "targets"
FIXTURE_TARGET = {"SSN-L": "ltass_universal", "SL5": "slope_-5", "SL7": "slope_-7", "SL9": "slope_-9", "PINK": "pink"}


def load_target_levels(target_id, targets_dir=TARGET_DIR):
    """26 band levels of a resources/data/targets/*.json file (shape only)."""
    for p in Path(targets_dir).glob("*.json"):
        j = json.loads(p.read_text())
        if j.get("id") == target_id:
            return j["levelsDb"]
    return None


def analyze_file(wav, events_path=None, target_id=None):
    x, fs = wavio.read_wav(wav)
    tl = load_target_levels(target_id) if target_id else None
    if tl:
        tl = M.apply_target_rolloff(tl, fs=fs)
    m = M.analyze(x, fs, tl)
    t = None
    if events_path and Path(events_path).exists():
        t = evmod.talker_stats(evmod.load_events(events_path), x.shape[1], fs)
        m["talkers"] = t
    return m, t


def _fmt(v, p=2):
    return "-" if v is None or (isinstance(v, float) and np.isnan(v)) else f"{v:.{p}f}"


def collect(fixture_dir):
    rows = {}
    for wav in sorted(Path(fixture_dir).glob("*.wav")):
        mt = re.match(r"(.+)_s(\d+)\.wav$", wav.name)
        if not mt:
            continue
        name, seed = mt.group(1), int(mt.group(2))
        ev = wav.with_suffix("").with_suffix(".events.json")
        py, tk = analyze_file(wav, ev, FIXTURE_TARGET.get(name))
        cpp = None
        sc = wav.with_suffix("").with_suffix(".sidecar.json")
        if sc.exists():
            cpp = cmp.load_metrics(sc)
        rows[(name, seed)] = {"py": py, "talkers": tk, "cpp": cpp, "wav": wav}
    return rows


def _natkey(n):
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", n)]


def trend_checks(rows):
    """Returns list of (description, ok, detail)."""
    out = []
    by = {}
    for (name, seed), r in rows.items():
        by.setdefault(name, []).append(r["py"])

    def mean(name, f):
        return float(np.mean([f(p) for p in by[name]]))

    m4 = lambda p: M.modulation_at(np.array(list(p["temporal"]["modulationSpectrum"].values())), 4.0)
    ns = [n for n in (1, 2, 3, 4, 5, 7, 8, 16) if f"B{n}" in by]
    if len(ns) >= 2:
        vals = [mean(f"B{n}", m4) for n in ns]
        ok = all(a >= b - 1e-9 for a, b in zip(vals, vals[1:]))
        out.append(("m(4 Hz) non-increasing with N (B-series)", ok, ", ".join(f"B{n}={v:.3f}" for n, v in zip(ns, vals))))
    if "SSN-L" in by:
        v = mean("SSN-L", m4)
        out.append(("SSN modulation at the floor (m(4 Hz) < 0.05)", v < 0.05, f"m(4 Hz) = {v:.3f}"))
    for name in sorted(by, key=_natkey):
        dv = [p["spectrum"].get("deviation") for p in by[name]]
        if all(dv):
            rms = float(np.mean([d["rms125to8kDb"] for d in dv]))
            out.append((f"{name}: LTASS shape RMS dev 125-8k <= 1 dB", rms <= 1.0, f"{rms:.2f} dB"))
    return out


def render_report(rows, plots_dir=None):
    L = ["# BabbleForge validation report", "",
         f"Fixtures: {len(rows)} renders. Metrics: independent Python implementation (`tools/validation/bfval`).", ""]
    L += ["## Level, temporal and spectral summary", "",
          "| Fixture | seed | RMS dB | crest dB | L10-L90 dB | gap med s | gap p95 s | m(4 Hz) | oct dev max dB | k_a mean | k_a min/max |",
          "|---|---|---|---|---|---|---|---|---|---|---|"]
    for (name, seed) in sorted(rows, key=lambda k: (_natkey(k[0]), k[1])):
        p, tk = rows[(name, seed)]["py"], rows[(name, seed)]["talkers"]
        mod = np.array(list(p["temporal"]["modulationSpectrum"].values()))
        dev = p["spectrum"].get("deviation")
        L.append("| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
            name, seed, _fmt(p["level"]["rmsDb"]), _fmt(p["level"]["crestDb"]),
            _fmt(p["temporal"]["envelope"]["L10minusL90Db"]), _fmt(p["temporal"]["gaps"]["medianS"], 3),
            _fmt(p["temporal"]["gaps"]["p95S"], 3), _fmt(M.modulation_at(mod, 4.0), 3),
            _fmt(dev["maxAbsDb"]) if dev else "-", _fmt(tk["meanActive"]) if tk else "-",
            f"{tk['minActive']}/{tk['maxActive']}" if tk else "-"))
    L += ["", "## 1/3-octave levels (dB, mean-square band power; first seed per fixture)", ""]
    first = {}
    for (name, seed), r in sorted(rows.items(), key=lambda kv: (_natkey(kv[0][0]), kv[0][1])):
        first.setdefault(name, r)
    cen = M.THIRD_OCT_NOMINAL[3:24]
    L.append("| Fixture | " + " | ".join(f"{c:g}" for c in cen) + " |")
    L.append("|---|" + "---|" * len(cen))
    for name, r in first.items():
        L.append(f"| {name} | " + " | ".join(_fmt(v, 1) for v in r["py"]["spectrum"]["thirdOctDb"][3:24]) + " |")
    L += ["", "## Expected trends (VALIDATION 5)", "", "| Check | Result | Detail |", "|---|---|---|"]
    for d, ok, det in trend_checks(rows):
        L.append(f"| {d} | {'PASS' if ok else 'FAIL'} | {det} |")
    cmp_rows = [(k, r) for k, r in rows.items() if r["cpp"]]
    if cmp_rows:
        L += ["", "## bfanalyze vs Python", "", "| Fixture | seed | checks | failures |", "|---|---|---|---|"]
        for (name, seed), r in sorted(cmp_rows, key=lambda kv: (_natkey(kv[0][0]), kv[0][1])):
            checks = cmp.compare(r["cpp"], r["py"], talkers_py=r["talkers"])
            bad = [c.name for c in checks if not c.ok]
            L.append(f"| {name} | {seed} | {len(checks)} | {', '.join(bad[:6]) or 'none'}{' ...' if len(bad) > 6 else ''} |")
    if plots_dir:
        L += _plots(first, Path(plots_dir), L)
    return "\n".join(L) + "\n"


def _plots(first, plots_dir, _unused):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception:
        return ["", "_matplotlib not available: plots skipped._"]
    plots_dir.mkdir(parents=True, exist_ok=True)
    fig, ax = plt.subplots(figsize=(8, 4))
    for name, r in first.items():
        ax.semilogx(M.THIRD_OCT_NOMINAL[3:24], np.array(r["py"]["spectrum"]["thirdOctDb"][3:24]), label=name)
    ax.set_xlabel("Hz"); ax.set_ylabel("dB"); ax.legend(fontsize=6, ncol=3); ax.grid(True, alpha=.3)
    fig.savefig(plots_dir / "third_octave.png", dpi=110, bbox_inches="tight")
    fig2, ax2 = plt.subplots(figsize=(8, 4))
    for name, r in first.items():
        ax2.semilogx(M.MOD_BAND_NOMINAL, list(r["py"]["temporal"]["modulationSpectrum"].values()), label=name)
    ax2.set_xlabel("modulation Hz"); ax2.set_ylabel("m(f)"); ax2.legend(fontsize=6, ncol=3); ax2.grid(True, alpha=.3)
    fig2.savefig(plots_dir / "modulation.png", dpi=110, bbox_inches="tight")
    return ["", "## Plots", "", f"![third-octave]({plots_dir.name}/third_octave.png)",
            f"![modulation]({plots_dir.name}/modulation.png)"]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fixtures", required=True)
    ap.add_argument("--out", default="validation_report.md")
    ap.add_argument("--plots", action="store_true")
    a = ap.parse_args(argv)
    rows = collect(a.fixtures)
    if not rows:
        print("no <name>_s<seed>.wav files found", file=sys.stderr)
        return 1
    out = Path(a.out)
    out.write_text(render_report(rows, out.parent / (out.stem + "_plots") if a.plots else None))
    print("wrote", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
