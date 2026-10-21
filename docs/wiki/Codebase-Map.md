# Codebase Map

| I want to... | Look in |
|---|---|
| Masker engine and core types | `src/core/` |
| Corpus database and loader | `src/core/` (`BF_WITH_CORPUS_DB`, the `bf_sqlite` target) |
| Command-line tools | `tools/` (`bfrender`, `bfanalyze`, `bfbench`) |
| Tests | `tests/` (255 ctest tests with the build above) |
| JUCE app: device layer, CLI player, GUI | `app/` (`app/backend`, `app/cli`, `app/gui`) |
| Build options and dependencies | `CMakeLists.txt`, `cmake/Dependencies.cmake`, `docs/BUILDING.md` |
| Specs | `docs/` (`ENGINE.md`, `GUI.md`, `SPATIAL_ENGINE.md`, `SPECTRUM_ENGINE.md`, `TALKER_ENGINE.md`, `MASK_STRATEGIES.md`, `CORPUS.md`, `PRESETS.md`, `REALTIME_ARCHITECTURE.md`) |
| CI and Pages | `.github/workflows/` |
