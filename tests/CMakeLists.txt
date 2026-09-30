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

bf_set_warnings(bfcore_tests)

include(CTest)
include(Catch)
catch_discover_tests(bfcore_tests)
