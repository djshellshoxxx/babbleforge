add_executable(bfrender main.cpp)

target_link_libraries(bfrender
  PRIVATE
    bfcore
)

target_compile_definitions(bfrender PRIVATE BF_DEFAULT_DATA_DIR="${PROJECT_SOURCE_DIR}/resources/data")

bf_set_warnings(bfrender)
