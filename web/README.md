# BabbleForge web demo

A static web page that runs the **BabbleForge masking engine in the browser**. The C++ engine
(`src/core`, offline `MaskEngine` path) is compiled to WebAssembly with Emscripten. The page
uses the same factory data files (`resources/data`) and the same compose → validate → plan
pipeline as the desktop app. Everything runs locally: no audio or settings leave the browser.

- **Pages:** RUN / MASK / AREA / OUTPUT, plus ANALYSIS in Advanced mode, laid out after
  `docs/GUI.md`. The controls are Area, Mask Type, Strength, Character, Voice Amount, Voice
  Variety, Clear Voice Reduction and Mask Mix. Advanced mode adds the Talker Engine and
  Talker Timing fields, the stationary contribution and noise seed, the spectrum target
  (including the custom 7-band curve) with its correction, Spatial Output (Mono/Stereo/4/6/8),
  Spread, Speaker Variation, the limiter, technical meters and the four Analysis cards.
- **Voices:** 24 real speakers (12 F / 12 M) from LibriSpeech dev-clean (CC BY 4.0), analysed
  by our corpus importer. See [`public/corpus/ATTRIBUTION.md`](public/corpus/ATTRIBUTION.md).
  If the corpus cannot be loaded, or you open the page with `?corpus=synthetic`, the page uses
  the engine's synthetic pseudo-speech corpus instead, and the UI labels it "synthetic voice
  placeholders".
- **Level:** all meters show digital level only (dBFS/LUFS), not SPL.

## Architecture

```
main thread (app.js)          Web Worker (engine-worker.js)            AudioWorklet (worklet.js)
 UI, preset document  ──────▶  bfweb.wasm: MaskEngine (offline path)  ──▶ small chunk queue
 decodeAudioData(FLAC)         renders ~0.5 s ahead in 2048-frame        plays, reports consumed
 meters / analysis   ◀─ 10 Hz  chunks, folds N ch → stereo, posts      frames back to the worker
                     stats     Float32Array chunks (transferables)  ◀─ (MessagePort)
```

- GitHub Pages cannot send COOP/COEP headers, so the page does not use SharedArrayBuffer.
  Audio moves through a `MessagePort` between the worker and the worklet.
- Each settings edit is composed in the worker (`bf_effective`), which gives the effective
  values shown in the UI. After a 150 ms debounce the edit is pushed to the engine
  (`bf_set_preset` → `buildScenarioPlan` → `MaskEngine::setPlan` at the current position).
  The engine's own plan crossfades apply. Changing the output layout or the
  spectral-correction settings rebuilds the engine.
- Because the worker renders ahead, setting changes are heard after about 0.5 s.
- The engine always runs at 48 kHz. If the browser refuses a 48 kHz `AudioContext`, the
  worklet resamples linearly.
- Multichannel layouts (4/6/8) are rendered with the full spatial engine. They are then
  folded to the two browser channels with a constant-power downmix by speaker azimuth; the
  UI says so.

## Build locally

Prerequisites: CMake ≥ 3.24, Node ≥ 18, git, Python 3.

```bash
# 1. Emscripten SDK (once; installed inside the repo, git-ignored)
git clone --depth 1 https://github.com/emscripten-core/emsdk web/.emsdk
web/.emsdk/emsdk install 6.0.10 && web/.emsdk/emsdk activate 6.0.10

# 2. Engine → WebAssembly (build-web/bfweb.{mjs,wasm})
bash web/scripts/build-wasm.sh

# 3. Static site → web/dist
node web/scripts/build.mjs

# 4. Serve it
npx serve web/dist        # then open the printed http://localhost:3000
```

The WASM project (`web/wasm/CMakeLists.txt`) is a separate CMake project. It globs the engine
modules from `src/core` and leaves out SQLite, libFLAC, r8brain, libfvad, JUCE and the
real-time host (`rt/*.cpp`). The desktop build is unchanged. `ResampleWeb.cpp` stands in for
the r8brain resampler; at the demo's fixed 48 kHz it is never called with a rate change.

## Tests

```bash
# Native vs WASM bit-exactness (synthetic corpus, 3 scenarios incl. plan events, ring4, mono)
# plus WASM block-size independence. Needs a native bfrender:
cmake -S . -B build-native -G Ninja -DCMAKE_BUILD_TYPE=Release -DBF_BUILD_TESTS=OFF -DBF_WITH_CORPUS_DB=OFF
cmake --build build-native --target bfrender
node web/tests/parity.mjs --bfrender build-native/tools/bfrender/bfrender

# Headless browser smoke test of web/dist (Playwright + Chromium): Start, 3 s of audio,
# played RMS near -26 dBFS, Character / Mask Type change the engine statistics, ring4 rebuild.
# Uses the preinstalled Chromium at /opt/pw-browsers if present, otherwise:
#   (cd web && npm install --no-save playwright && npx playwright install chromium)
node web/tests/smoke.mjs                       # BF_CORPUS=synthetic to skip the speech corpus
```

## Rebuilding the speech corpus

`web/public/corpus` is generated and committed (about 22 MB; 16 kHz 16-bit FLAC, because
LibriSpeech has no content above 8 kHz). To regenerate it:

```bash
pip install numpy scipy soundfile
# LibriSpeech dev-clean from https://www.openslr.org/12 (or any mirror), extracted so that
# <dir>/dev-clean/<speaker>/<chapter>/*.flac and <dir>/SPEAKERS.TXT exist.
# Needs a native build with the corpus DB (build/tools/bfcorpus/bfcorpus).
tools/webdemo/make_corpus.sh <dir>
```

The pipeline runs `prepare_librispeech.py` first. It concatenates about 45 s of utterances
per speaker and upsamples to 48 kHz, because the importer rejects sources below 32 kHz. Then
`bfcorpus import` runs our real importer (VAD, ASL, features, quality); the speakers it
rejects for bandwidth are dropped. Last, `export_corpus.py` writes `corpus.json` with exactly
the inputs `CorpusLoader` gives to `CorpusSnapshot::build`, plus the audio files and
`ATTRIBUTION.md`. The current files were made from a GitHub mirror of dev-clean, because
openslr.org was not reachable from the build environment.

## GitHub Pages

The workflow `.github/workflows/pages.yml` runs on every push to `main` and to
`claude/babbleforge-v1-engine-spec-2ivcgh`, or by hand. It installs and caches emsdk, builds
the WASM engine and a native `bfrender`, runs the parity test, builds the site, runs the
smoke test, then publishes `web/dist` with `actions/upload-pages-artifact` and
`actions/deploy-pages`.

Turn it on once in the repository settings: **Settings → Pages → Build and deployment →
Source: GitHub Actions**. By default, the `github-pages` environment only accepts
deployments from the default branch. To deploy from the feature branch as well, add it under
**Settings → Environments → github-pages → Deployment branches and tags**. The site is then
served at `https://<owner>.github.io/babbleforge/`.
