configure_file(BuildInfo.h.in BuildInfo.h @ONLY)

add_library(bfcore STATIC
  Version.cpp
)

file(GLOB bf_config_sources CONFIGURE_DEPENDS config/*.cpp)
target_sources(bfcore PRIVATE ${bf_config_sources})

target_include_directories(bfcore
  PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(bfcore
  PUBLIC
    nlohmann_json::nlohmann_json
    bf_detmath_flags
)

bf_set_warnings(bfcore)

# random / math / io modules (globbed)
file(GLOB bf_rng_math_io_sources CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/random/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/math/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/io/*.cpp)
target_sources(bfcore PRIVATE ${bf_rng_math_io_sources})

# dsp / analysis modules (globbed)
file(GLOB bf_dsp_analysis_sources CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/dsp/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/analysis/*.cpp)
target_sources(bfcore PRIVATE ${bf_dsp_analysis_sources})

# spectrum / engine modules (globbed)
file(GLOB bf_spectrum_engine_sources CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/spectrum/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/engine/*.cpp)
target_sources(bfcore PRIVATE ${bf_spectrum_engine_sources})
