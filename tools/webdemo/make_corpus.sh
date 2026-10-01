#!/usr/bin/env bash
# Rebuilds web/public/corpus from LibriSpeech dev-clean (CC BY 4.0).
#
#   tools/webdemo/make_corpus.sh <LibriSpeech dir containing dev-clean/ and SPEAKERS.TXT> [work dir]
#
# Needs a native build with the corpus DB (build/tools/bfcorpus/bfcorpus; BF_BFCORPUS overrides)
# and python3 with numpy, scipy, soundfile (pip install numpy scipy soundfile).
# Only the folders present under dev-clean/ are used, so a partial download works; the demo
# keeps 12 female + 12 male speakers (the best importer quality per sex).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LS="${1:?usage: make_corpus.sh <LibriSpeech dir> [work dir]}"
WORK="${2:-$(mktemp -d)}"
BFCORPUS="${BF_BFCORPUS:-$ROOT/build/tools/bfcorpus/bfcorpus}"
rm -rf "$WORK/in" "$WORK/corpus"
python3 "$ROOT/tools/webdemo/prepare_librispeech.py" "$LS" "$WORK/in" --seconds 45
"$BFCORPUS" import "$WORK/in" "$WORK/corpus" --no-source-paths
rm -rf "$ROOT/web/public/corpus"
python3 "$ROOT/tools/webdemo/export_corpus.py" "$WORK/corpus" "$ROOT/web/public/corpus" \
  --attribution "$WORK/in/attribution.json" --female 12 --male 12
