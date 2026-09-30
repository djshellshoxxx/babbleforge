cmake_minimum_required(VERSION 3.24)

project(BabbleForge VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(BF_BUILD_TESTS "Build tests" ON)
option(BF_BUILD_TOOLS "Build tools (bfrender)" ON)
option(BF_BUILD_APP "JUCE app, later" OFF)
option(BF_RT_CHECK "RT-check build: allocation hooks abort/count on RT threads in every executable" OFF)

# Dependencies
include(cmake/Dependencies.cmake)
include(cmake/Warnings.cmake)

# Core library
add_subdirectory(src/core)

# Tests
if(BF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Tools
if(BF_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
