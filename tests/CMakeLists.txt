add_executable(bfcore_tests
)

# Glob all test sources with CONFIGURE_DEPENDS
file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS *.cpp)
# RT-check tests run in their own binary, linked with the operator new/delete hooks.
set(rt_check_sources ${CMAKE_CURRENT_SOURCE_DIR}/test_rt_alloc_check.cpp)
list(REMOVE_ITEM test_sources ${rt_check_sources})
target_sources(bfcore_tests PRIVATE ${test_sources})

target_link_libraries(bfcore_tests
  PRIVATE
    bfcore
    Catch2::Catch2WithMain
)

target_compile_definitions(bfcore_tests PRIVATE
  BF_DATA_DIR="${PROJECT_SOURCE_DIR}/resources/data"
  BF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  BF_TEST_OUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_out")

# MSVC only exposes M_PI with _USE_MATH_DEFINES.
target_compile_definitions(bfcore_tests PRIVATE _USE_MATH_DEFINES)

bf_set_warnings(bfcore_tests)

# RT-check test binary (REALTIME_ARCHITECTURE.md §1.3): allocation hooks always linked here;
# with BF_RT_CHECK=ON bfcore already carries them.
add_executable(bfcore_rtcheck_tests ${rt_check_sources})
if(NOT BF_RT_CHECK)
  target_sources(bfcore_rtcheck_tests PRIVATE $<TARGET_OBJECTS:bf_rtcheck_hooks>)
endif()
target_link_libraries(bfcore_rtcheck_tests PRIVATE bfcore Catch2::Catch2WithMain)
target_compile_definitions(bfcore_rtcheck_tests PRIVATE
  BF_DATA_DIR="${PROJECT_SOURCE_DIR}/resources/data"
  BF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  BF_TEST_OUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_out")
bf_set_warnings(bfcore_rtcheck_tests)

include(CTest)
include(Catch)
# Hidden test cases ("[.long]": 5 min / 1 h renders, 10 min soak) are not discovered here.
catch_discover_tests(bfcore_tests)
# The 20 s paced run is timing-sensitive (xruns, late starts): run it alone, never alongside other tests under ctest -j.
catch_discover_tests(bfcore_rtcheck_tests TEST_PREFIX "rtcheck: " PROPERTIES RUN_SERIAL TRUE)

# Long acceptance renders (ENGINE.md §12 long forms). Excluded from the default run: they skip
# (exit code 4, reported as "Skipped") unless BF_LONG_TESTS=1, e.g.
#   BF_LONG_TESTS=1 ctest -L long --output-on-failure
add_test(NAME long.acceptance COMMAND bfcore_tests "[long]" --durations yes)
set_tests_properties(long.acceptance PROPERTIES LABELS long SKIP_RETURN_CODE 4 TIMEOUT 7200)

# CLI smoke tests (bfrender / bfanalyze executables).
if(BF_BUILD_TOOLS)
  set(_bf_smoke "${CMAKE_CURRENT_BINARY_DIR}/cli_smoke")
  add_test(NAME cli.bfrender_smoke
    COMMAND bfrender --scenario "${CMAKE_CURRENT_SOURCE_DIR}/data/render/smoke_scenario.json"
            --synthetic-corpus 12 --out "${_bf_smoke}/smoke.wav" --taps T1,T4)
  set_tests_properties(cli.bfrender_smoke PROPERTIES FIXTURES_SETUP bf_cli_smoke LABELS cli)
  add_test(NAME cli.bfrender_outputs
    COMMAND ${CMAKE_COMMAND} -E sha256sum "${_bf_smoke}/smoke.wav" "${_bf_smoke}/smoke.events.json"
            "${_bf_smoke}/smoke.sidecar.json" "${_bf_smoke}/smoke.T1.wav" "${_bf_smoke}/smoke.T4.wav")
  set_tests_properties(cli.bfrender_outputs PROPERTIES FIXTURES_REQUIRED bf_cli_smoke LABELS cli)
  add_test(NAME cli.bfanalyze_smoke
    COMMAND bfanalyze "${_bf_smoke}/smoke.wav" --events "${_bf_smoke}/smoke.events.json"
            --target ltass_universal --out "${_bf_smoke}/smoke.metrics.json")
  set_tests_properties(cli.bfanalyze_smoke PROPERTIES FIXTURES_REQUIRED bf_cli_smoke LABELS cli)
  add_test(NAME cli.bfrender_usage COMMAND bfrender --bogus)
  set_tests_properties(cli.bfrender_usage PROPERTIES WILL_FAIL TRUE LABELS cli)
  add_test(NAME bench.bfbench_smoke COMMAND bfbench --seconds 2 --synthetic-corpus 8)
  set_tests_properties(bench.bfbench_smoke PROPERTIES LABELS bench)
endif()
