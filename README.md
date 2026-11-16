# BabbleForge

BabbleForge is a desktop speech-masking generator. It produces stationary speech-shaped masking noise, multi-talker babble, and hybrid combinations of the two, at a controlled digital level and across multichannel speaker layouts.

- **V1:** a statistically controlled, reproducible masker with no microphone or calibration.
- **V2 (planned):** measurement-microphone room analysis and automatic calibration.

## Documentation

| Document | Purpose |
|---|---|
| [docs/SPEC.md](docs/SPEC.md) | Product specification (V1 and V2 overview) |
| [docs/GUI.md](docs/GUI.md) | V1 GUI specification |
| [docs/ENGINE.md](docs/ENGINE.md) | **Engine master spec**: architecture, signal flow, gain staging, limiter, meters, CPU, defaults, open questions, priorities, acceptance criteria |
| [docs/MASK_STRATEGIES.md](docs/MASK_STRATEGIES.md) | Strategy architecture, hybrid mixer, Character / Clear Voice Reduction / Voice Amount macros, laboratory mode |
| [docs/TALKER_ENGINE.md](docs/TALKER_ENGINE.md) | Virtual talkers, planner/scheduler, talker-count modes, segment selection, voice diversity, preloading |
| [docs/SPECTRUM_ENGINE.md](docs/SPECTRUM_ENGINE.md) | Spectrum targets, FIR design, stationary masker, speech-matched profiles, slow correction, modulation analysis |
| [docs/SPATIAL_ENGINE.md](docs/SPATIAL_ENGINE.md) | Layouts, stereo, multichannel and distributed rendering, decorrelation, zones, output matrix |
| [docs/CORPUS.md](docs/CORPUS.md) | Corpus requirements, ingestion pipeline, VAD, quality, metadata schema |
| [docs/PRESETS.md](docs/PRESETS.md) | Area × Strategy model, area defaults, preset file format, data-driven tuning |
| [docs/REALTIME_ARCHITECTURE.md](docs/REALTIME_ARCHITECTURE.md) | Real-time rules, threads, lock-free messaging, memory, determinism/PRNG |
| [docs/RELIABILITY.md](docs/RELIABILITY.md) | State machine, failure handling, fallback policies, logging, diagnostics, privacy |
| [docs/VALIDATION.md](docs/VALIDATION.md) | Unit, statistical and stress tests; scientific, human and machine validation |
| [docs/V2_EXTENSION_POINTS.md](docs/V2_EXTENSION_POINTS.md) | Reserved interfaces and calibration signal bus |
| [docs/research/](docs/research/) | Research notes and deep-research addendum |

## Support

BabbleForge is free. Donations in Monero (XMR) are appreciated:

```
85cSWLFurZj8XbKWX7Kk3u1oUtp5vLGQcLSfXEdGnTUU5P9mik6GCPk8guPfAwzHdFFUCbDKChZEphQyp6BNMQwo5oyPLUD
```

## Status

The specifications are in `docs/`. Code exists: a C++20 core library (`src/core`), command-line tools (`bfrender`, `bfanalyze`, `bfbench` and related), and tests. Not production-ready. Implementation order: `docs/ENGINE.md` §11.

### Build and test (verified on this snapshot)

Linux, system SQLite 3 headers present, JUCE app off (the default):

```sh
cmake -S . -B build-core -DCMAKE_BUILD_TYPE=Release -DBF_BUILD_TESTS=ON -DBF_BUILD_TOOLS=ON -DBF_BUILD_APP=OFF -DBF_SQLITE_USE_SYSTEM=ON
cmake --build build-core -j 8
ctest --test-dir build-core --output-on-failure
```

Result: 255 of 255 ctest tests pass. The GUI and the JUCE app (`-DBF_BUILD_APP=ON`) were not built here.

Known issue: with `-DBF_WITH_CORPUS_DB=OFF` the core library fails to link, because `EngineController.cpp` calls `bf::loadCorpus`, which is only built when the corpus database is enabled. Build with the corpus database on (the default) until this is fixed.

Without the system SQLite option, configure fetches the SQLite amalgamation from sqlite.org. That download failed in the verification environment, which blocks the default configure there.

## Wiki

Wiki source pages are in [docs/wiki/](docs/wiki/Home.md): developer guide, architecture summary and codebase map.
