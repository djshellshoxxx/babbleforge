add_library(bf_juce_backend STATIC JuceAudioBackend.cpp JuceAudioBackend.h)
target_include_directories(bf_juce_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bf_juce_backend
  PUBLIC bfcore juce::juce_audio_devices juce::juce_events juce::juce_core
  PRIVATE juce::juce_recommended_config_flags)
bf_juce_defaults(bf_juce_backend)
