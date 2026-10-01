juce_add_gui_app(BabbleForge PRODUCT_NAME "BabbleForge" COMPANY_NAME "BabbleForge")
target_sources(BabbleForge PRIVATE Main.cpp)
target_link_libraries(BabbleForge
  PRIVATE bf_juce_backend juce::juce_gui_basics juce::juce_recommended_config_flags)
target_compile_definitions(BabbleForge PRIVATE BF_DEFAULT_DATA_DIR="${PROJECT_SOURCE_DIR}/resources/data")
bf_juce_defaults(BabbleForge)
