#!/usr/bin/env python3
"""Propose updated resources/data values from experiment results (docs/VALIDATION.md 9, step 2).

    python tools/tuning/tune.py results.csv [--data-dir resources/data] [--out-dir tuning_out]
                                [--map mapping.json] [--study-id S1] [--apply-label]

results.csv columns (header required; extra columns ignored; repeated rows are averaged):
    condition,metric,observed,target
  condition: "<kind>:<id>" naming a data file, e.g. area:office, strategy:dense
  metric:    a key of the mapping table below (e.g. meanActive, octave125DevDb, stationaryFraction)
  observed:  measured value (from bfanalyze / bfval)   target: the value the design intends

Each mapping says which JSON value to move and how: new = current + gain * (target - observed), skipped when
|target - observed| <= deadband, then clamped to [min, max] and rounded. Nothing is written in place:
<out-dir>/<path under data-dir> holds the full proposed files and <out-dir>/tuning_diff.md the diff report
(a pull request body). Stdlib only; never modifies resources/data.
"""
import argparse
import csv
import json
import statistics
import sys
from pathlib import Path

# metric -> how to change which data file (file template uses the condition's id)
DEFAULT_MAPPING = {
    "meanActive": {"kinds": ["area"], "file": "areas/{id}.json", "path": "talkers.meanActive",
                   "gain": 1.0, "deadband": 0.05, "min": 1, "max": 64, "round": 2,
                   "note": "mean k_a measured vs designed m"},
    "octave125DevDb": {"kinds": ["area"], "file": "areas/{id}.json", "path": "spectrum.lfTrimDb125",
                       "gain": 1.0, "deadband": 0.25, "min": -6.0, "max": 6.0, "round": 2,
                       "note": "125 Hz octave deviation (observed) -> LF trim, target normally 0"},
    "stationaryFraction": {"kinds": ["area"], "file": "areas/{id}.json", "path": "mix.balancedStationaryFraction",
                           "gain": 1.0, "deadband": 0.02, "min": 0.0, "max": 1.0, "round": 3,
                           "note": "stationary fraction needed to hit the target occupancy / gap statistics"},
    "gainVariationDb": {"kinds": ["area"], "file": "areas/{id}.json", "path": "talkers.gainVariationDb",
                        "gain": 1.0, "deadband": 0.1, "min": 0.0, "max": 8.0, "round": 2,
                        "note": "measured SD of per-event gain offsets vs sigma"},
}


def read_results(path):
    agg = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            try:
                key = (row["condition"].strip(), row["metric"].strip())
                agg.setdefault(key, []).append((float(row["observed"]), float(row["target"])))
            except (KeyError, ValueError) as e:
                raise SystemExit(f"{path}: bad row {row}: {e}")
    return {k: (statistics.fmean(o for o, _ in v), statistics.fmean(t for _, t in v), len(v)) for k, v in agg.items()}


def _get(doc, dotted):
    cur = doc
    for k in dotted.split("."):
        if not isinstance(cur, dict) or k not in cur:
            return None
        cur = cur[k]
    return cur


def _set(doc, dotted, value):
    keys = dotted.split(".")
    cur = doc
    for k in keys[:-1]:
        cur = cur.setdefault(k, {})
    cur[keys[-1]] = value


def propose(results, data_dir, mapping, study_id=None, apply_label=False):
    """Returns (proposed_docs {relpath: json}, changes [dict], skipped [str])."""
    data_dir = Path(data_dir)
    docs, changes, skipped = {}, [], []
    for (cond, metric), (obs, tgt, n) in sorted(results.items()):
        m = mapping.get(metric)
        if not m:
            skipped.append(f"{cond} / {metric}: no mapping for this metric")
            continue
        kind, _, ident = cond.partition(":")
        if kind not in m["kinds"] or not ident:
            skipped.append(f"{cond} / {metric}: condition kind '{kind}' not in {m['kinds']}")
            continue
        rel = m["file"].format(id=ident)
        src = data_dir / rel
        if not src.exists():
            skipped.append(f"{cond} / {metric}: {rel} not found")
            continue
        if rel not in docs:
            docs[rel] = json.loads(src.read_text())
        cur = _get(docs[rel], m["path"])
        if cur is None:
            cur = 0.0
        err = tgt - obs
        if abs(err) <= m.get("deadband", 0.0):
            skipped.append(f"{cond} / {metric}: within deadband (error {err:+.3f})")
            continue
        new = cur + m["gain"] * err
        new = min(max(new, m.get("min", float("-inf"))), m.get("max", float("inf")))
        new = round(new, m.get("round", 3))
        if new == cur:
            skipped.append(f"{cond} / {metric}: no change after clamping/rounding")
            continue
        _set(docs[rel], m["path"], new)
        changes.append({"file": rel, "path": m["path"], "old": cur, "new": new, "metric": metric, "condition": cond,
                        "observed": obs, "target": tgt, "n": n, "note": m.get("note", "")})
    if study_id and apply_label:
        for rel in {c["file"] for c in changes}:
            d = docs[rel]
            d["evidenceLabel"] = f"validated:{study_id}"
            refs = d.setdefault("evidenceRefs", [])
            if study_id not in refs:
                refs.append(study_id)
    return docs, changes, skipped


def diff_report(changes, skipped, results_path, study_id=None):
    L = ["# Proposed data-file changes", "", f"Source: `{results_path}`" + (f" (study `{study_id}`)" if study_id else ""),
         "", f"{len(changes)} change(s) in {len({c['file'] for c in changes})} file(s). "
         "Review, then copy the proposed files into `resources/data/` (no recompilation needed).", ""]
    if changes:
        L += ["| File | Path | Old | New | Condition | Metric | Observed | Target | n |", "|---|---|---|---|---|---|---|---|---|"]
        for c in changes:
            L.append(f"| {c['file']} | `{c['path']}` | {c['old']} | {c['new']} | {c['condition']} | {c['metric']} | "
                     f"{c['observed']:.4g} | {c['target']:.4g} | {c['n']} |")
        L += ["", "```diff"]
        for c in changes:
            L += [f"--- {c['file']}", f"+++ {c['file']} (proposed)", f"-  {c['path']}: {c['old']}",
                  f"+  {c['path']}: {c['new']}"]
        L += ["```"]
    if skipped:
        L += ["", "## Skipped", ""] + [f"- {s}" for s in skipped]
    if study_id:
        L += ["", f"When validated, set `evidenceLabel` to `validated:{study_id}` (use `--apply-label`)."]
    return "\n".join(L) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("results")
    ap.add_argument("--data-dir", default=str(Path(__file__).resolve().parents[2] / "resources" / "data"))
    ap.add_argument("--out-dir", default="tuning_out")
    ap.add_argument("--map", help="JSON file overriding/extending the metric mapping table")
    ap.add_argument("--study-id")
    ap.add_argument("--apply-label", action="store_true", help="also set evidenceLabel=validated:<study-id>")
    a = ap.parse_args(argv)
    mapping = dict(DEFAULT_MAPPING)
    if a.map:
        mapping.update(json.loads(Path(a.map).read_text()))
    docs, changes, skipped = propose(read_results(a.results), a.data_dir, mapping, a.study_id, a.apply_label)
    out = Path(a.out_dir)
    for rel in {c["file"] for c in changes}:
        p = out / "proposed" / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(docs[rel], indent=2) + "\n")
    out.mkdir(parents=True, exist_ok=True)
    (out / "tuning_diff.md").write_text(diff_report(changes, skipped, a.results, a.study_id))
    print(f"{len(changes)} change(s); wrote {out / 'tuning_diff.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
