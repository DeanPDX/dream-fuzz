cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(DreamFuzz VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.14
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_binary_data(DreamFuzzAssets
    SOURCES Assets/background.png)

juce_add_plugin(DreamFuzz
    COMPANY_NAME              "Sembit"
    BUNDLE_ID                 "com.sembit.dreamfuzz"
    PLUGIN_MANUFACTURER_CODE  Smbt
    PLUGIN_CODE               Drfz
    FORMATS                   VST3 AU Standalone
    PRODUCT_NAME              "Dream Fuzz"
    IS_SYNTH                  FALSE
    NEEDS_MIDI_INPUT          FALSE
    NEEDS_MIDI_OUTPUT         FALSE
    IS_MIDI_EFFECT            FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD   TRUE
    VST3_CATEGORIES           "Fx" "Distortion"
    AU_MAIN_TYPE              "kAudioUnitType_Effect")

target_sources(DreamFuzz PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

set(DREAMFUZZ_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

set(DREAMFUZZ_LIBS
    DreamFuzzAssets
    juce::juce_audio_utils
    juce::juce_dsp)

target_compile_definitions(DreamFuzz PUBLIC ${DREAMFUZZ_DEFS})
target_link_libraries(DreamFuzz
    PRIVATE ${DREAMFUZZ_LIBS}
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---- Offline test harness: audio verification + UI snapshot ----
juce_add_console_app(DreamFuzzTests PRODUCT_NAME "DreamFuzzTests")

target_sources(DreamFuzzTests PRIVATE
    Tests/EngineTest.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_include_directories(DreamFuzzTests PRIVATE Source)
target_compile_definitions(DreamFuzzTests PRIVATE ${DREAMFUZZ_DEFS} JUCE_PLUGINHOST_VST3=1)
target_link_libraries(DreamFuzzTests
    PRIVATE ${DREAMFUZZ_LIBS}
    PUBLIC juce::juce_recommended_config_flags)
