# Developer Guide

Build the core library, tools and tests without the JUCE app:

```sh
cmake -S . -B build-core -DCMAKE_BUILD_TYPE=Release -DBF_BUILD_TESTS=ON -DBF_BUILD_TOOLS=ON -DBF_BUILD_APP=OFF -DBF_SQLITE_USE_SYSTEM=ON
cmake --build build-core -j 8
ctest --test-dir build-core --output-on-failure    # 255 of 255 pass on this snapshot
```

- The JUCE app (`-DBF_BUILD_APP=ON`) fetches JUCE 8.0.x. See `docs/BUILDING.md`. Not built in this pass.
- `BF_RT_CHECK=ON` builds an RT-check variant with allocation hooks that abort or count on real-time threads (CMake option).
- Known issue: `-DBF_WITH_CORPUS_DB=OFF` does not link (`EngineController.cpp` references `bf::loadCorpus`).
- Without `BF_SQLITE_USE_SYSTEM`, CMake fetches the SQLite amalgamation from sqlite.org.
