# Architecture Summary

From `docs/ENGINE.md`, `docs/REALTIME_ARCHITECTURE.md`, `docs/SPEC.md` (see the README table for the full list) and the code layout.

- **Core library** (`src/core`): the masker engine, with source preparation, talkers, spectrum, spatial rendering and corpus handling.
- **Real-time rules** (`docs/REALTIME_ARCHITECTURE.md`): threads, lock-free messaging, memory and determinism. The engine must be reproducible from a seed (`docs/ENGINE.md` principle 11).
- **Tools** (`tools/`): `bfrender`, `bfanalyze`, `bfbench` and related command-line tools.
- **Application** (`app/`): a JUCE device layer, CLI player and GUI, built only with `BF_BUILD_APP=ON`.
- **Corpus** (`docs/CORPUS.md`): speech corpus requirements, with an optional SQLite-backed corpus database (`BF_WITH_CORPUS_DB`).
