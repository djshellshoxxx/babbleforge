#!/usr/bin/env python3
"""Prepare the web-demo speech subset from LibriSpeech dev-clean (CC BY 4.0).

Concatenates the first utterances of each selected speaker (in corpus order, 250 ms of digital
silence between utterances) into one ~45 s WAV per speaker, upsampled 16 -> 48 kHz (polyphase,
scipy) and stored as 16-bit, laid out
as <out>/<speaker>/<speaker>.wav so that `bfcorpus import` assigns speakers by folder name
(docs/CORPUS.md, speaker identity rule 2). Writes <out>/attribution.json.

  python3 tools/webdemo/prepare_librispeech.py <LibriSpeech dir containing dev-clean/> <out> [--seconds 45]

LibriSpeech is 16 kHz audio (~8 kHz bandwidth). The importer rejects sources below 32 kHz
(reject.sample_rate), hence the upsampling; the importer's bandwidth check still applies
(warn.bandwidth for all, reject.bandwidth for the narrowest), and export_corpus.py keeps only
accepted recordings. Every dev-clean speaker whose folder is present is prepared.

Requires: numpy, scipy, soundfile.
"""
import argparse
import json
import os
import sys

import numpy as np
import soundfile as sf
from scipy.signal import resample_poly



def speakers_txt(root):
    info = {}
    with open(os.path.join(root, "SPEAKERS.TXT"), encoding="utf-8") as f:
        for line in f:
            if line.startswith(";"):
                continue
            parts = [p.strip() for p in line.split("|")]
            if len(parts) >= 5 and parts[0].isdigit():
                info[int(parts[0])] = {"sex": parts[1], "subset": parts[2], "name": parts[4]}
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("librispeech")
    ap.add_argument("out")
    ap.add_argument("--seconds", type=float, default=45.0)
    a = ap.parse_args()
    meta = speakers_txt(a.librispeech)
    dev = os.path.join(a.librispeech, "dev-clean")
    gap = np.zeros(int(0.25 * 16000), dtype=np.float32)
    attribution = {
        "source": "LibriSpeech ASR corpus, dev-clean subset (Panayotov et al., 2015), https://www.openslr.org/12",
        "license": "CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/)",
        "readers": "LibriVox volunteers; recordings are public-domain LibriVox audiobooks",
        "processing": "first utterances of each speaker concatenated with 250 ms silence, upsampled 16 -> 48 kHz, analysed by the BabbleForge corpus importer (bfcorpus), exported as 16-bit FLAC by tools/webdemo/export_corpus.py (stored at 16 kHz, resampled to 48 kHz by the browser)",
        "speakers": [],
    }
    present = sorted(int(d) for d in os.listdir(dev) if d.isdigit())
    for spk in present:
        sdir = os.path.join(dev, str(spk))
        files = []
        for chapter in sorted(os.listdir(sdir), key=int):
            cdir = os.path.join(sdir, chapter)
            files += [os.path.join(cdir, f) for f in sorted(os.listdir(cdir)) if f.endswith(".flac")]
        pieces, used, total = [], [], 0.0
        for fpath in files:
            x, fs = sf.read(fpath, dtype="float32")
            assert fs == 16000, fpath
            if pieces:
                pieces.append(gap)
            pieces.append(x)
            used.append(os.path.basename(fpath)[:-5])
            total += len(x) / fs
            if total >= a.seconds:
                break
        y = np.concatenate(pieces)
        y48 = np.clip(resample_poly(y, 3, 1).astype(np.float32), -1.0, 1.0)
        odir = os.path.join(a.out, f"ls{spk}")
        os.makedirs(odir, exist_ok=True)
        sf.write(os.path.join(odir, f"ls{spk}.wav"), y48, 48000, subtype="PCM_16")
        m = meta.get(spk, {})
        attribution["speakers"].append({"externalId": f"ls{spk}", "librispeechId": spk, "sex": m.get("sex"),
                                        "reader": m.get("name"), "utterances": used,
                                        "seconds": round(len(y) / 16000, 2)})
        print(f"ls{spk}: {len(used)} utterances, {len(y) / 16000:.1f} s", file=sys.stderr)
    with open(os.path.join(a.out, "attribution.json"), "w", encoding="utf-8") as f:
        json.dump(attribution, f, indent=2)


if __name__ == "__main__":
    main()
