"""Compare a bfanalyze JSON (or a bfrender sidecar's "metrics") with the Python metrics.

compare(cpp, py, tol) returns a list of Check rows (name, cpp, py, diff, tol, ok). Tolerances (TOL) are the
defaults used by the integration test; the 1/3-octave filterbank-vs-FFT tolerance follows
SPECTRUM_ENGINE 5.1 (0.2 dB nominal) with margin for the Butterworth reference. Only the operating
range 100 Hz-10 kHz is compared (outside it the C++ analyzer's band-edge handling differs by design).
"""
import json
from dataclasses import dataclass

import numpy as np

TOL = {
    "rmsDb": 0.01,
    "truePeakDb": 0.1,
    "thirdOctDb": 0.5,
    "octaveDb": 0.4,
    "envelopeDb": 0.01,
    "modDepth": 0.01,
    "gapS": 0.011,
    "gapCount": 0,
    "modSpectrumAbs": 0.005,
    "modSpectrumRel": 0.02,
    "correlation": 0.005,
    "talkerMean": 1e-6,
    "deviationRmsDb": 1.0,
}

# Non-stationary (babble) signals: FFT aggregation vs a filterbank differ more per 1/3-octave band.
TOL_BABBLE = {"thirdOctDb": 1.0, "octaveDb": 0.5, "deviationRmsDb": 1.5}


@dataclass
class Check:
    name: str
    cpp: float
    py: float
    tol: float
    ok: bool

    @property
    def diff(self):
        return self.py - self.cpp


def _chk(rows, name, a, b, tol):
    if a is None or b is None:
        return
    a, b = float(a), float(b)
    rows.append(Check(name, a, b, tol, bool(abs(a - b) <= tol)))


def load_metrics(path):
    with open(path) as f:
        j = json.load(f)
    return j.get("metrics", j)  # accept sidecars


def compare(cpp, py, tol=None, talkers_py=None):
    t = dict(TOL)
    t.update(tol or {})
    rows = []
    _chk(rows, "level.rmsDb", cpp["level"]["rmsDb"], py["level"]["rmsDb"], t["rmsDb"])
    _chk(rows, "level.truePeakDbtp", cpp["level"]["truePeakDbtp"], py["level"]["truePeakDbtp"], t["truePeakDb"])
    cs, ps = cpp["spectrum"], py["spectrum"]
    for i in range(3, 24):  # 100 Hz .. 10 kHz
        _chk(rows, f"third.{cs['thirdOctCentresHz'][i]:g}Hz", cs["thirdOctDb"][i], ps["thirdOctDb"][i], t["thirdOctDb"])
    for i, fc in enumerate(cs["octaveCentresHz"]):
        _chk(rows, f"octave.{fc:g}Hz", cs["octaveDb"][i], ps["octaveDb"][i], t["octaveDb"])
    ce, pe = cpp["temporal"]["envelope"], py["temporal"]["envelope"]
    for k in ("L10Db", "L90Db", "L10minusL90Db"):
        _chk(rows, f"envelope.{k}", ce[k], pe[k], t["envelopeDb"])
    _chk(rows, "envelope.modulationDepth10s", ce["modulationDepth10s"], pe["modulationDepth10s"], t["modDepth"])
    cg, pg = cpp["temporal"]["gaps"], py["temporal"]["gaps"]
    _chk(rows, "gaps.count", cg["count"], pg["count"], t["gapCount"])
    for k in ("medianS", "p95S", "maxS"):
        _chk(rows, f"gaps.{k}", cg[k], pg[k], t["gapS"])
    cm = {round(float(k), 3): v for k, v in cpp["temporal"].get("modulationSpectrum", {}).items()}
    pm = {round(float(k), 3): v for k, v in py["temporal"].get("modulationSpectrum", {}).items()}
    for f in sorted(set(cm) & set(pm)):
        tol_m = max(t["modSpectrumAbs"], t["modSpectrumRel"] * abs(cm[f]))
        _chk(rows, f"mod.{f:g}Hz", cm[f], pm[f], tol_m)
    if "correlation" in cpp and "correlation" in py:
        a, b = np.array(cpp["correlation"]["matrix"]), np.array(py["correlation"]["matrix"])
        _chk(rows, "correlation.maxAbsDiff", 0.0, float(np.max(np.abs(a - b))), t["correlation"])
    if "deviation" in cs and "deviation" in ps:
        _chk(rows, "deviation.rms125to8kDb", cs["deviation"]["rms125to8kDb"], ps["deviation"]["rms125to8kDb"],
             t["deviationRmsDb"])
    if talkers_py and "talkers" in cpp:
        ct = cpp["talkers"]
        for k in ("events", "speakers", "minActive", "maxActive"):
            _chk(rows, f"talkers.{k}", ct.get(k), talkers_py.get(k), 0)
        _chk(rows, "talkers.meanActive", ct.get("meanActive"), talkers_py.get("meanActive"), t["talkerMean"])
    return rows


def summarize(rows):
    bad = [r for r in rows if not r.ok]
    return f"{len(rows) - len(bad)}/{len(rows)} checks passed" + "".join(
        f"\n  FAIL {r.name}: cpp={r.cpp:.4f} py={r.py:.4f} diff={r.diff:+.4f} tol={r.tol:g}" for r in bad)
