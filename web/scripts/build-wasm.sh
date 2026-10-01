#!/usr/bin/env bash
# Builds the engine WASM module (web/wasm) into build-web/ with Emscripten.
# Uses $EMSDK if set (CI), else web/.emsdk (see web/README.md).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
if ! command -v emcmake >/dev/null 2>&1; then
  EMSDK_DIR="${EMSDK:-$ROOT/web/.emsdk}"
  # shellcheck disable=SC1091
  source "$EMSDK_DIR/emsdk_env.sh" >/dev/null
fi
BUILD="${BF_WEB_BUILD_DIR:-$ROOT/build-web}"
emcmake cmake -S "$ROOT/web/wasm" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j "$(nproc 2>/dev/null || echo 4)"
ls -la "$BUILD/bfweb.mjs" "$BUILD/bfweb.wasm"
