#!/usr/bin/env python3
"""Export an imported BabbleForge corpus (bfcorpus import) for the web demo.

Reads <corpusRoot>/corpus.sqlite and the 48 kHz FLAC cache and writes
  <out>/corpus.json        speakers (features, weight), recordings (length, VAD regions, ASL,
                           quality, per-anchor ASL, excluded anchors) -- exactly the inputs that
                           CorpusLoader (src/core/corpus/CorpusLoader.cpp) hands to
                           CorpusSnapshot::build, so the browser builds the same snapshot;
  <out>/audio/<id>.flac    mono 16-bit FLAC of each cache file, at --audio-rate (default 16 kHz:
                           LibriSpeech has no content above 8 kHz, so storing 48 kHz would triple
                           the download for nothing; the browser's decodeAudioData resamples back
                           to the 48 kHz engine rate);
  <out>/ATTRIBUTION.md     source, licence and reader credits (from attribution.json).

  python3 tools/webdemo/export_corpus.py <corpusRoot> <out> --attribution <prep>/attribution.json
         [--female 12 --male 12]

Selection: the usable speakers (enabled, a recording with quality class > 0 and a cache file),
best mean recording quality first, up to --female / --male per sex (sex from attribution.json).
Requires: numpy, scipy, soundfile.
"""
import argparse
import hashlib
import json
import math
import os
import sqlite3

import numpy as np
import soundfile as sf
from scipy.signal import resample_poly

KC = 48000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("out")
    ap.add_argument("--attribution", required=True)
    ap.add_argument("--female", type=int, default=12)
    ap.add_argument("--male", type=int, default=12)
    ap.add_argument("--audio-rate", type=int, default=16000, choices=[16000, 24000, 48000])
    a = ap.parse_args()

    attr = json.load(open(a.attribution, encoding="utf-8"))
    sex_of = {s["externalId"]: s.get("sex") for s in attr["speakers"]}
    db = sqlite3.connect(os.path.join(a.root, "corpus.sqlite"))
    db.row_factory = sqlite3.Row
    info = {r["key"]: r["value"] for r in db.execute("SELECT key, value FROM corpus_info")}
    speakers = list(db.execute("SELECT * FROM speaker WHERE enabled = 1 ORDER BY speaker_id"))
    recs = list(db.execute("SELECT * FROM recording WHERE quality_class > 0 AND cache_file IS NOT NULL "
                           "AND cache_file != '' ORDER BY recording_id"))
    recs_of = {}
    for r in recs:
        recs_of.setdefault(r["speaker_id"], []).append(r)

    usable = [s for s in speakers if s["speaker_id"] in recs_of]
    usable.sort(key=lambda s: (-(s["quality_mean"] or 0.0), s["speaker_id"]))
    want = {"F": a.female, "M": a.male}
    chosen = []
    for s in usable:
        sx = sex_of.get(s["external_id"])
        if sx in want and want[sx] > 0:
            want[sx] -= 1
            chosen.append(s)
    chosen.sort(key=lambda s: s["speaker_id"])  # CorpusLoader order

    langs = sorted({s["language"] for s in chosen if s["language"]})
    out_spk, out_rec, credits = [], [], []
    os.makedirs(os.path.join(a.out, "audio"), exist_ok=True)
    h = hashlib.sha256()
    for si, s in enumerate(chosen):
        f = [0.0] * 8
        f[0] = 12.0 * math.log2(s["f0_median_hz"] / 100.0) if (s["f0_median_hz"] or 0) > 0 else 0.0
        f[1] = s["f0_range_st"] or 0.0
        f[2] = s["speaking_rate_sps"] or 0.0
        f[3] = math.log2(s["spectral_centroid_hz"]) if (s["spectral_centroid_hz"] or 0) > 0 else 0.0
        f[4], f[5], f[6] = s["ltass_pc1"] or 0.0, s["ltass_pc2"] or 0.0, s["ltass_pc3"] or 0.0
        f[7] = float(langs.index(s["language"]) + 1) if s["language"] else 0.0
        out_spk.append({"externalId": s["external_id"], "features": f,
                        "weight": max(0.01, (s["quality_mean"] or 0.0) / 100.0)})
        meta = next((x for x in attr["speakers"] if x["externalId"] == s["external_id"]), {})
        credits.append(meta)
        for r in recs_of[s["speaker_id"]]:
            rid = r["recording_id"]
            regions = [[x[0], x[1]] for x in db.execute(
                "SELECT start_sample, end_sample FROM speech_region WHERE recording_id = ? ORDER BY start_sample", (rid,))]
            anchor_asl, excluded = [], []
            for g in db.execute("SELECT anchor_sample, asl_10s_dbfs, excluded FROM segment WHERE recording_id = ? "
                                "ORDER BY anchor_sample", (rid,)):
                if g[2]:
                    excluded.append(g[0])
                else:
                    anchor_asl.append([g[0], g[1]])
            length = int(round(r["duration_s"] * KC))
            x, fs = sf.read(os.path.join(a.root, r["cache_file"]), dtype="float32")
            assert fs == KC
            x = x[:length]
            name = f"{s['external_id']}_{rid}.flac"
            y = x if a.audio_rate == KC else resample_poly(x, 1, KC // a.audio_rate).astype(np.float32)
            sf.write(os.path.join(a.out, "audio", name), np.clip(y, -1, 1), a.audio_rate, subtype="PCM_16",
                     format="FLAC")
            h.update(name.encode() + b"\0" + x.tobytes())
            out_rec.append({"speaker": si, "file": "audio/" + name, "length": length, "speech": regions,
                            "aslDb": r["asl_dbfs"], "quality": min(1.0, max(0.0, (r["quality_score"] or 0) / 100.0)),
                            "anchorAsl": anchor_asl, "excludedAnchors": excluded})
    doc = {
        "schema": "babbleforge.webcorpus/1",
        "corpusVersion": "web-" + info.get("corpusVersion", "") + "-" + h.hexdigest()[:8],
        "sampleRate": KC,
        "audioRate": a.audio_rate,
        "source": attr["source"],
        "license": attr["license"],
        "speakers": out_spk,
        "recordings": out_rec,
    }
    with open(os.path.join(a.out, "corpus.json"), "w", encoding="utf-8") as fp:
        json.dump(doc, fp, separators=(",", ":"))
    lines = ["# Demo speech corpus: attribution and licence", "",
             f"Source: {attr['source']}", "", f"Licence: {attr['license']}", "",
             f"Readers: {attr['readers']}", "", f"Processing: {attr['processing']}", "",
             "| Speaker | LibriSpeech id | Sex | Reader | Utterances |", "|---|---|---|---|---|"]
    for c in credits:
        lines.append(f"| {c.get('externalId')} | {c.get('librispeechId')} | {c.get('sex')} | {c.get('reader')} | "
                     f"{', '.join(c.get('utterances', []))} |")
    open(os.path.join(a.out, "ATTRIBUTION.md"), "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print(f"exported {len(out_spk)} speakers, {len(out_rec)} recordings -> {a.out}")


if __name__ == "__main__":
    main()
