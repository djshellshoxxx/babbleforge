juce_add_gui_app(BabbleForge PRODUCT_NAME "BabbleForge" COMPANY_NAME "BabbleForge")
target_sources(BabbleForge PRIVATE
  Main.cpp
  MainComponent.cpp MainComponent.h
  StatusBar.cpp StatusBar.h
  LookAndFeel.cpp LookAndFeel.h
  Widgets.cpp Widgets.h
  model/AppSettings.cpp model/AppSettings.h
  model/AppState.cpp model/AppState.h
  model/Catalog.cpp model/Catalog.h
  model/EngineBridge.cpp model/EngineBridge.h
  model/PresetSession.cpp model/PresetSession.h
  model/OutputModel.cpp model/OutputModel.h
  # Pages self-register (pages/Page.h); add new page sources here.
  pages/Page.cpp pages/Page.h
  pages/RunPage.cpp pages/RunPage.h
  pages/MaskPage.cpp pages/MaskPage.h
  pages/AreaPage.cpp pages/AreaPage.h pages/AreaOutputWidgets.h
  pages/OutputPage.cpp pages/OutputPage.h
  pages/PlaceholderPages.cpp
  tests/UiTests.cpp
  tests/AreaOutputTests.cpp)
target_include_directories(BabbleForge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(BabbleForge
  PRIVATE bf_juce_backend juce::juce_gui_basics juce::juce_data_structures juce::juce_recommended_config_flags)
target_compile_definitions(BabbleForge PRIVATE
  BF_DEFAULT_DATA_DIR="${PROJECT_SOURCE_DIR}/resources/data"
  JUCE_MODAL_LOOPS_PERMITTED=1)  # the headless UI tests pump the message loop
if(MSVC)
  target_compile_options(BabbleForge PRIVATE /utf-8 /bigobj)
endif()
bf_juce_defaults(BabbleForge)

# Headless UI tests (docs/GUI_ARCHITECTURE.md). Linux needs an X server: xvfb-run is used
# when available. Registered even with BF_BUILD_TESTS=OFF: `ctest --test-dir <build>/app/gui`.
enable_testing()
find_program(BF_XVFB_RUN xvfb-run)
if(UNIX AND NOT APPLE AND BF_XVFB_RUN)
  add_test(NAME gui_ui_tests COMMAND ${BF_XVFB_RUN} -a $<TARGET_FILE:BabbleForge> --run-ui-tests)
else()
  add_test(NAME gui_ui_tests COMMAND $<TARGET_FILE:BabbleForge> --run-ui-tests)
endif()
set_tests_properties(gui_ui_tests PROPERTIES TIMEOUT 180 LABELS gui)
