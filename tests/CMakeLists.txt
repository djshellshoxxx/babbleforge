add_executable(bfcore_tests
)

# Glob all test sources with CONFIGURE_DEPENDS
file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS *.cpp)
target_sources(bfcore_tests PRIVATE ${test_sources})

target_link_libraries(bfcore_tests
  PRIVATE
    bfcore
    Catch2::Catch2WithMain
)

target_compile_definitions(bfcore_tests PRIVATE
  BF_DATA_DIR="${PROJECT_SOURCE_DIR}/resources/data"
  BF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

bf_set_warnings(bfcore_tests)

include(CTest)
include(Catch)
catch_discover_tests(bfcore_tests)
