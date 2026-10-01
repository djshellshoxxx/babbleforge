# BabbleForge web demo: engine-only subset of bfcore compiled to WebAssembly (Emscripten).
#
# Standalone project (does not touch the desktop build): it globs the engine modules from
# src/core and leaves out SQLite, FLAC, r8brain, libfvad, JUCE and the real-time host (rt/*.cpp).
#   emcmake cmake -S web/wasm -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web
cmake_minimum_required(VERSION 3.24)
project(BabbleForgeWeb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(BF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(BF_CORE "${BF_ROOT}/src/core")

include(FetchContent)
FetchContent_Declare(nlohmann_json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(nlohmann_json)

# Version / build info (same template as the desktop build).
file(STRINGS "${BF_ROOT}/CMakeLists.txt" _bf_proj REGEX "^project\\(BabbleForge VERSION")
string(REGEX MATCH "VERSION ([0-9.]+)" _m "${_bf_proj}")
if(CMAKE_MATCH_1)
  set(PROJECT_VERSION ${CMAKE_MATCH_1})
endif()
configure_file("${BF_CORE}/BuildInfo.h.in" "${CMAKE_CURRENT_BINARY_DIR}/BuildInfo.h" @ONLY)

file(GLOB BF_WEB_CORE_SOURCES CONFIGURE_DEPENDS
  ${BF_CORE}/Version.cpp
  ${BF_CORE}/config/*.cpp
  ${BF_CORE}/random/*.cpp
  ${BF_CORE}/math/*.cpp
  ${BF_CORE}/io/*.cpp
  ${BF_CORE}/dsp/*.cpp
  ${BF_CORE}/analysis/*.cpp
  ${BF_CORE}/spectrum/*.cpp
  ${BF_CORE}/engine/*.cpp
  ${BF_CORE}/talker/*.cpp
  ${BF_CORE}/spatial/*.cpp
  ${BF_CORE}/strategy/*.cpp
  ${BF_CORE}/corpus/CorpusSnapshot.cpp
  ${BF_CORE}/corpus/SyntheticCorpus.cpp)

add_executable(bfweb ${BF_WEB_CORE_SOURCES} bf_web.cpp ResampleWeb.cpp)
target_include_directories(bfweb PRIVATE ${BF_ROOT}/src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(bfweb PRIVATE nlohmann_json::nlohmann_json)
# Same deterministic-math flag as the desktop build (cmake/Warnings.cmake).
target_compile_options(bfweb PRIVATE -ffp-contract=off -O3)

set(BF_WEB_EXPORTS
  _malloc _free _bf_init _bf_catalog _bf_effective _bf_corpus_synthetic _bf_corpus_begin
  _bf_corpus_set_audio _bf_corpus_finish _bf_corpus_info _bf_start _bf_set_preset _bf_render _bf_channels
  _bf_stats _bf_position _bf_stop _bf_render_scenario_sha)
list(JOIN BF_WEB_EXPORTS "," BF_WEB_EXPORTS_STR)

target_link_options(bfweb PRIVATE
  -O3
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createBabbleForge
  -sENVIRONMENT=web,worker,node
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sMAXIMUM_MEMORY=2GB
  -sSTACK_SIZE=1MB
  -sEXPORTED_FUNCTIONS=${BF_WEB_EXPORTS_STR}
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToNewUTF8,HEAPF32,HEAPU8
  -sFILESYSTEM=1
  --embed-file ${BF_ROOT}/resources/data@/data)
set_target_properties(bfweb PROPERTIES OUTPUT_NAME bfweb SUFFIX ".mjs")
