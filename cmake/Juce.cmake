# JUCE (device layer + GUI), only included when BF_BUILD_APP=ON.
include(FetchContent)

set(BF_JUCE_TAG "8.0.15" CACHE STRING "JUCE git tag used for the app")
set(BF_ASIO_SDK_DIR "" CACHE PATH "Steinberg ASIO SDK root (contains common/iasiodrv.h); empty: no ASIO")

set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG ${BF_JUCE_TAG}
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

# Common definitions for every JUCE target of the app.
function(bf_juce_defaults target)
  target_compile_definitions(${target} PUBLIC
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)
  if(BF_ASIO_SDK_DIR AND WIN32)
    target_compile_definitions(${target} PUBLIC JUCE_ASIO=1)
    target_include_directories(${target} PUBLIC "${BF_ASIO_SDK_DIR}/common")
  endif()
endfunction()
