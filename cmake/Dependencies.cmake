include(FetchContent)

# Catch2 v3.5.4
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.4
)

# nlohmann/json v3.11.3
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)

FetchContent_MakeAvailable(Catch2 nlohmann_json)

# ---------------------------------------------------------------------------
# Corpus ingestion dependencies (docs/CORPUS.md §2, §3). All built static; the
# tests/programs of the dependencies are OFF.
# ---------------------------------------------------------------------------
option(BF_WITH_CORPUS_DB "Build the SQLite corpus DB, loader and bfcorpus tool" ON)
option(BF_SQLITE_USE_SYSTEM "Link the system SQLite3 instead of fetching the amalgamation" OFF)

# libFLAC 1.4.3 (BSD-3-Clause), no Ogg, library only.
FetchContent_Declare(
  flac
  GIT_REPOSITORY https://github.com/xiph/flac.git
  GIT_TAG 1.4.3
  GIT_SHALLOW TRUE
)

# r8brain-free-src (MIT) sample-rate converter.
FetchContent_Declare(
  r8brain
  GIT_REPOSITORY https://github.com/avaneev/r8brain-free-src.git
  GIT_TAG version-6.4
  GIT_SHALLOW TRUE
)

# libfvad v1.0 (BSD-3-Clause): standalone extraction of the WebRTC VAD.
FetchContent_Declare(
  libfvad
  GIT_REPOSITORY https://github.com/dpirch/libfvad.git
  GIT_TAG v1.0
  GIT_SHALLOW TRUE
)

# SQLite 3.46.1 amalgamation (public domain), compiled as C in a small target.
FetchContent_Declare(
  sqlite_amalgamation
  URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(_bf_saved_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WITH_OGG OFF CACHE BOOL "" FORCE)
set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(INSTALL_PKGCONFIG_MODULES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(flac r8brain libfvad)
if(BF_WITH_CORPUS_DB AND NOT BF_SQLITE_USE_SYSTEM)
  # No URL_HASH: the archive hash could not be verified from this environment.
  message(STATUS "Fetching the SQLite amalgamation from sqlite.org; if the download fails "
                 "(offline/firewalled), re-configure with -DBF_SQLITE_USE_SYSTEM=ON")
  FetchContent_MakeAvailable(sqlite_amalgamation)
  if(NOT EXISTS "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
    message(FATAL_ERROR "Could not download the SQLite amalgamation from sqlite.org. "
                        "Re-run CMake with -DBF_SQLITE_USE_SYSTEM=ON to use the system SQLite3 instead.")
  endif()
endif()

set(BUILD_TESTING ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS ${_bf_saved_shared} CACHE BOOL "" FORCE)

if(BF_WITH_CORPUS_DB AND BF_SQLITE_USE_SYSTEM)
  find_package(SQLite3 REQUIRED)
  add_library(bf_sqlite INTERFACE)
  target_link_libraries(bf_sqlite INTERFACE SQLite::SQLite3)
elseif(BF_WITH_CORPUS_DB)
  add_library(bf_sqlite STATIC ${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c)
  set_target_properties(bf_sqlite PROPERTIES C_STANDARD 11)
  target_include_directories(bf_sqlite SYSTEM PUBLIC ${sqlite_amalgamation_SOURCE_DIR})
  target_compile_definitions(bf_sqlite PUBLIC SQLITE_THREADSAFE=1)
  target_compile_definitions(bf_sqlite PRIVATE
    SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0 SQLITE_DEFAULT_MEMSTATUS=0)
  target_compile_options(bf_sqlite PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>)
  if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(bf_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
  endif()
endif()

add_library(bf_r8brain STATIC ${r8brain_SOURCE_DIR}/r8bbase.cpp)
target_include_directories(bf_r8brain SYSTEM PUBLIC ${r8brain_SOURCE_DIR})
target_compile_options(bf_r8brain PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>)

file(GLOB _bf_fvad_src ${libfvad_SOURCE_DIR}/src/*.c ${libfvad_SOURCE_DIR}/src/*/*.c)
add_library(bf_fvad STATIC ${_bf_fvad_src})
set_target_properties(bf_fvad PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_include_directories(bf_fvad PRIVATE ${libfvad_SOURCE_DIR}/src)
target_include_directories(bf_fvad SYSTEM PUBLIC ${libfvad_SOURCE_DIR}/include)
target_compile_options(bf_fvad PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>)
