juce_add_console_app(babbleforge-cli PRODUCT_NAME "babbleforge-cli")
target_sources(babbleforge-cli PRIVATE main.cpp)
target_link_libraries(babbleforge-cli
  PRIVATE bf_juce_backend juce::juce_recommended_config_flags)
target_compile_definitions(babbleforge-cli PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1 BF_DEFAULT_DATA_DIR="${PROJECT_SOURCE_DIR}/resources/data")
bf_juce_defaults(babbleforge-cli)
