"""Talker statistics from bfrender *.events.json (babbleforge.events/1; docs/VALIDATION.md 4).

Each event: start/end (sample index, may be < 0 for events already running at t=0), slot, speaker,
recording, segment, gain, levelVarDb. All statistics are clipped to [0, frames).
"""
import json
from collections import defaultdict

import numpy as np


def load_events(path):
    with open(path) as f:
        return json.load(f)


def _clip(ev, frames):
    out = []
    for e in ev["events"]:
        a, b = max(0, int(e["start"])), min(frames, int(e["end"]))
        if b > a:
            out.append((a, b, e))
    return out


def talker_stats(ev, frames, fs=None):
    """Same definitions as bfanalyze's "talkers": events, speakers, meanActive (k_a mean over the
    file), minActive, maxActive (k_a extremes); plus start spacing and reuse statistics."""
    fs = fs or float(ev.get("sampleRate", 48000.0))
    evs = _clip(ev, frames)
    delta = defaultdict(int)
    active = 0.0
    for a, b, _ in evs:
        delta[a] += 1
        delta[b] -= 1
        active += b - a
    k, kmin, kmax, prev = 0, 1 << 30, 0, 0
    for s in sorted(delta):
        if s > prev:
            kmin, kmax = min(kmin, k), max(kmax, k)
        k += delta[s]
        prev = s
    if prev < frames:
        kmin, kmax = min(kmin, k), max(kmax, k)
    out = {"events": len(evs), "speakers": len({e.get("speaker", 0) for _, _, e in evs}),
           "meanActive": active / frames if frames else 0.0,
           "minActive": 0 if not delta else kmin, "maxActive": kmax}
    out.update(start_spacing(evs, fs))
    out.update(reuse_stats(evs, fs))
    return out


def start_spacing(evs, fs):
    """Spacing between consecutive event starts (events that start inside the file), seconds."""
    st = np.sort([e["start"] for _, _, e in evs if e["start"] >= 0]) / fs
    if len(st) < 2:
        return {"startSpacingMeanS": 0.0, "startSpacingMinS": 0.0, "startSpacingP5S": 0.0}
    d = np.diff(st)
    return {"startSpacingMeanS": float(d.mean()), "startSpacingMinS": float(d.min()),
            "startSpacingP5S": float(np.percentile(d, 5))}


def reuse_stats(evs, fs):
    """Speaker reuse: for events in start order, the number of selections between two uses of the same
    speaker (5th percentile reported; VALIDATION 4 wants >= R_spk). Region reuse: how often the same
    (recording, segment) is selected again and the shortest time between the start of two such events."""
    order = sorted((e for _, _, e in evs), key=lambda e: e["start"])
    last_idx, gaps = {}, []
    for i, e in enumerate(order):
        s = e.get("speaker", 0)
        if s in last_idx:
            gaps.append(i - last_idx[s])
        last_idx[s] = i
    seen, reuse, min_dt = {}, 0, None
    for e in order:
        key = (e.get("recording"), e.get("segment"))
        if key in seen:
            reuse += 1
            dt = (e["start"] - seen[key]) / fs
            min_dt = dt if min_dt is None else min(min_dt, dt)
        seen[key] = e["start"]
    return {"speakerReuseGapP5": float(np.percentile(gaps, 5)) if gaps else None,
            "speakerReuseGapMin": int(min(gaps)) if gaps else None,
            "speakerReuseRate": len(gaps) / len(order) if order else 0.0,
            "regionReuseCount": reuse, "regionReuseMinDtS": min_dt}


def level_offsets(ev):
    """Per-event level-variation offsets (dB) for the level-variation distribution check."""
    return np.array([float(e.get("levelVarDb", 0.0)) for e in ev["events"]])
