include(FetchContent)

FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

# A console app rather than a bare executable, so the JUCE modules the engine
# needs are configured exactly as they are for the application itself.
juce_add_console_app(opendj-tests
    PRODUCT_NAME "OpenDJ tests"
    NEEDS_CURL   FALSE)

target_sources(opendj-tests PRIVATE
    Mixer_test.cpp
    Deck_test.cpp
    TrackAnalyser_test.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Mixer.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Deck.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/TrackAnalysis.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/TrackAnalyser.cpp)

target_include_directories(opendj-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_definitions(opendj-tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1)

target_link_libraries(opendj-tests PRIVATE
    Catch2::Catch2WithMain
    juce::juce_audio_formats
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(opendj-tests PRIVATE /utf-8)
endif()

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(opendj-tests)
