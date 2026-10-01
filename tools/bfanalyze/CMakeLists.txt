add_executable(bfanalyze main.cpp)

target_link_libraries(bfanalyze
  PRIVATE
    bfcore
)

target_compile_definitions(bfanalyze PRIVATE BF_DEFAULT_DATA_DIR="${PROJECT_SOURCE_DIR}/resources/data")

bf_set_warnings(bfanalyze)
