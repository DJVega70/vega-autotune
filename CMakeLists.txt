cmake_minimum_required(VERSION 3.22)
project(VegaAutotune VERSION 1.0.0)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

if(DEFINED JUCE_DIR)
    add_subdirectory(${JUCE_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.9 GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

set(VEGA_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND VEGA_FORMATS AU)
endif()

juce_add_plugin(VegaAutotune
    COMPANY_NAME "Vega Productions"
    BUNDLE_ID com.vegaproductions.autotune
    PLUGIN_MANUFACTURER_CODE Vgap
    PLUGIN_CODE Vat8
    FORMATS ${VEGA_FORMATS}
    PRODUCT_NAME "Vega Productions Autotune"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx Pitch Shift
    AU_MAIN_TYPE kAudioUnitType_MusicEffect
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(VegaAutotune PRIVATE Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_compile_definitions(VegaAutotune PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(VegaAutotune
    PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

option(VEGA_TESTS "Build offline test" OFF)
if(VEGA_TESTS)
    juce_add_console_app(VegaTuneTest PRODUCT_NAME "VegaTuneTest")
    target_sources(VegaTuneTest PRIVATE tests/TuneTest.cpp)
    target_include_directories(VegaTuneTest PRIVATE $<TARGET_PROPERTY:VegaAutotune,INCLUDE_DIRECTORIES>)
    target_compile_definitions(VegaTuneTest PRIVATE $<TARGET_PROPERTY:VegaAutotune,COMPILE_DEFINITIONS>)
    target_link_libraries(VegaTuneTest PRIVATE VegaAutotune)
endif()
