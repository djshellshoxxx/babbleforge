configure_file(BuildInfo.h.in BuildInfo.h @ONLY)

add_library(bfcore STATIC
  Version.cpp
)

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
