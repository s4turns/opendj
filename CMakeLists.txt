cmake_minimum_required(VERSION 3.22)

project(OpenDJ
    VERSION 0.1.0
    DESCRIPTION "Free and open source DJ software"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(OPENDJ_BUILD_TESTS "Build the unit test target" ON)
option(OPENDJ_ENABLE_ASIO "Enable ASIO output on Windows (requires the ASIO SDK)" OFF)
set(OPENDJ_ASIO_SDK_PATH "" CACHE PATH "Path to a local copy of the Steinberg ASIO SDK")

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/external" CACHE PATH "" FORCE)

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------------------
# Application target
# ---------------------------------------------------------------------------
juce_add_gui_app(OpenDJ
    PRODUCT_NAME  "OpenDJ"
    COMPANY_NAME  "OpenDJ"
    BUNDLE_ID     "me.interdo.opendj"
    VERSION       ${PROJECT_VERSION}
    NEEDS_CURL    FALSE)

juce_generate_juce_header(OpenDJ)

target_sources(OpenDJ PRIVATE
    src/app/Main.cpp
    src/app/MainComponent.cpp
    src/core/AudioEngine.cpp
    src/core/Deck.cpp
    src/core/Mixer.cpp
    src/analysis/TrackAnalysis.cpp
    src/analysis/TrackAnalyser.cpp
    src/control/Action.cpp
    src/control/ActionDispatcher.cpp
    src/control/MidiMapping.cpp
    src/control/MidiControlSurface.cpp
    src/ui/DeckComponent.cpp
    src/ui/MidiSetupComponent.cpp
    src/ui/WaveformComponent.cpp
    src/ui/MixerComponent.cpp)

target_include_directories(OpenDJ PRIVATE src)

target_compile_definitions(OpenDJ PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:OpenDJ,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:OpenDJ,JUCE_VERSION>")

if(OPENDJ_ENABLE_ASIO AND WIN32)
    if(NOT OPENDJ_ASIO_SDK_PATH)
        message(FATAL_ERROR "OPENDJ_ENABLE_ASIO is ON but OPENDJ_ASIO_SDK_PATH is not set.")
    endif()
    target_compile_definitions(OpenDJ PRIVATE JUCE_ASIO=1)
    target_include_directories(OpenDJ PRIVATE "${OPENDJ_ASIO_SDK_PATH}/common")
endif()

target_link_libraries(OpenDJ PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_devices
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(OpenDJ PRIVATE /utf-8)
endif()

if(OPENDJ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
