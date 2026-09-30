add_executable(bfrender main.cpp)

target_link_libraries(bfrender
  PRIVATE
    bfcore
)

bf_set_warnings(bfrender)
