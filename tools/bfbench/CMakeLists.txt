add_executable(bfbench main.cpp)

target_link_libraries(bfbench
  PRIVATE
    bfcore
)

target_compile_definitions(bfbench PRIVATE BF_DEFAULT_DATA_DIR="${PROJECT_SOURCE_DIR}/resources/data")

bf_set_warnings(bfbench)
