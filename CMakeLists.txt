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

# Rubber Band does the time stretching behind key lock. It ships a single
# compilation unit with no dependencies of its own, which is the whole reason it
# is compiled here rather than built with its own (Meson) build system. GPLv2 or
# later, so licence compatible.
FetchContent_Declare(rubberband
    URL https://github.com/breakfastquay/rubberband/archive/refs/tags/v4.0.0.tar.gz)
FetchContent_MakeAvailable(rubberband)

add_library(opendj_rubberband STATIC "${rubberband_SOURCE_DIR}/single/RubberBandSingle.cpp")
target_include_directories(opendj_rubberband SYSTEM PUBLIC "${rubberband_SOURCE_DIR}")
target_compile_options(opendj_rubberband PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
set_target_properties(opendj_rubberband PROPERTIES POSITION_INDEPENDENT_CODE ON)

# SQLite holds the track library. The system library is used where the
# platform has one, and the public domain amalgamation is fetched and compiled
# in otherwise, which is the Windows case.
find_package(SQLite3 QUIET)

if(SQLite3_FOUND)
    add_library(opendj_sqlite3 INTERFACE)
    target_link_libraries(opendj_sqlite3 INTERFACE SQLite::SQLite3)
else()
    FetchContent_Declare(sqlite3
        URL https://www.sqlite.org/2026/sqlite-amalgamation-3510200.zip)
    FetchContent_MakeAvailable(sqlite3)

    add_library(opendj_sqlite3 STATIC "${sqlite3_SOURCE_DIR}/sqlite3.c")
    target_include_directories(opendj_sqlite3 SYSTEM PUBLIC "${sqlite3_SOURCE_DIR}")
    target_compile_definitions(opendj_sqlite3 PRIVATE
        SQLITE_THREADSAFE=1
        SQLITE_OMIT_LOAD_EXTENSION=1
        SQLITE_DQS=0)
    target_compile_options(opendj_sqlite3 PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
    set_target_properties(opendj_sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)

    if(UNIX)
        target_link_libraries(opendj_sqlite3 PUBLIC ${CMAKE_DL_LIBS})
    endif()
endif()

# Sources shared by the application and the test binary, so the two cannot
# drift apart. Nothing in here includes a UI header.
set(OPENDJ_ENGINE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/core/AudioEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Deck.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Mixer.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/TrackAnalysis.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/TrackAnalyser.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/TrackDecoder.cpp
    ${CMAKE_SOURCE_DIR}/src/control/Action.cpp
    ${CMAKE_SOURCE_DIR}/src/control/ActionDispatcher.cpp
    ${CMAKE_SOURCE_DIR}/src/control/MidiMapping.cpp
    ${CMAKE_SOURCE_DIR}/src/control/MidiControlSurface.cpp
    ${CMAKE_SOURCE_DIR}/src/library/Library.cpp
    ${CMAKE_SOURCE_DIR}/src/library/LibraryScanner.cpp
    ${CMAKE_SOURCE_DIR}/src/library/TagReader.cpp)

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
    ${OPENDJ_ENGINE_SOURCES}
    src/app/Main.cpp
    src/app/MainComponent.cpp
    src/ui/BrowserComponent.cpp
    src/ui/DeckComponent.cpp
    src/ui/MidiSetupComponent.cpp
    src/ui/PlatterComponent.cpp
    src/ui/WaveformComponent.cpp
    src/ui/MixerComponent.cpp)

target_include_directories(OpenDJ PRIVATE src)

target_compile_definitions(OpenDJ PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    # MP3 is what most record collections are made of, and JUCE leaves its
    # decoder off by default. The patents it was guarding against expired in
    # 2017.
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:OpenDJ,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:OpenDJ,JUCE_VERSION>")

if(UNIX AND NOT APPLE)
    # JACK is how a Linux desktop gets at all four outputs of a controller's
    # audio interface with low latency. PipeWire presents itself as a JACK
    # server, so this covers a stock modern desktop too. JUCE loads libjack at
    # run time, so a machine without it still starts and falls back to ALSA.
    target_compile_definitions(OpenDJ PRIVATE JUCE_JACK=1)
endif()

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
    opendj_rubberband
    opendj_sqlite3
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
