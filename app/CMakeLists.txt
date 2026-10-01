add_subdirectory(backend)
add_subdirectory(cli)

# The GUI needs X11/freetype/fontconfig on Linux; BF_BUILD_GUI=OFF builds the audio-only app.
option(BF_BUILD_GUI "Build the BabbleForge GUI app" ON)
if(BF_BUILD_GUI)
  add_subdirectory(gui)
endif()
