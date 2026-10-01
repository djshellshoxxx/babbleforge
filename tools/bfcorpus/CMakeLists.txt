add_executable(bfcorpus main.cpp)

target_link_libraries(bfcorpus
  PRIVATE
    bfcore
)

bf_set_warnings(bfcorpus)
