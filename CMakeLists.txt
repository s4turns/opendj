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

if(WIN32)
    # windows.h defines min and max as macros, which turns every
    # std::numeric_limits<T>::max() inside Rubber Band into a syntax error.
    # NOMINMAX is the standard cure and the reason this target would not
    # compile on MSVC at all.
    target_compile_definitions(opendj_rubberband PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(opendj_rubberband PRIVATE /utf-8)
endif()

# LAME encodes the MP3 the recorder writes. JUCE bundles a FLAC encoder and a
# Vorbis one but no MP3 encoder at all: its LAMEEncoderAudioFormat shells out to
# a lame binary the user has to install, and buffers the whole recording to a
# temporary WAV before converting it, which for a three hour set is gigabytes of
# scratch file and minutes of waiting after the stop button. Compiling the
# library in costs one dependency and removes both. LGPL 2.1 or later, so
# licence compatible.
FetchContent_Declare(lame
    URL      https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz
    URL_HASH SHA256=ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e)
FetchContent_MakeAvailable(lame)

include("${CMAKE_SOURCE_DIR}/cmake/lame.cmake")

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

# OpenVINO runs the stem separation model in this process. Optional on purpose:
# it is a large dependency and a hundred megabytes of weights, and a build
# without it is a complete DJ application that simply cannot separate stems.
find_package(OpenVINO QUIET COMPONENTS Runtime)

if(OpenVINO_FOUND)
    message(STATUS "OpenVINO found: stem separation will be built in")
else()
    message(STATUS "OpenVINO not found: OpenDJ will build without stem separation")
endif()

# projectM draws the MilkDrop visuals, both in the window and into the
# broadcast. Optional in the same spirit as OpenVINO above: a build without it
# is a complete DJ application that simply has no visualiser.
#
# It needs an OpenGL context that does not belong to a window, so that the
# visuals keep being drawn with the panel closed and so a test can measure real
# pixels on a machine with no display at all. On Linux that context comes from
# EGL, which is what is looked for here; the visualiser is built only if it is
# there.
option(OPENDJ_ENABLE_VISUALIZER "Build the projectM visualiser" ON)

set(OPENDJ_VISUALIZER_AVAILABLE OFF)

if(OPENDJ_ENABLE_VISUALIZER)
    if(UNIX AND NOT APPLE)
        find_package(OpenGL QUIET COMPONENTS OpenGL EGL)

        if(TARGET OpenGL::EGL AND TARGET OpenGL::GL)
            set(OPENDJ_VISUALIZER_AVAILABLE ON)
        else()
            message(STATUS "EGL not found: OpenDJ will build without the visualiser")
        endif()
    elseif(WIN32)
        # Windows has no surfaceless anything, so the context comes from WGL
        # and hangs off a window that is never shown; see Visualizer.cpp. This
        # asks for nothing but opengl32, which is part of the OS, so it is
        # effectively always on. Whether the machine can actually give out a
        # 3.3 core context is a question only the driver can answer, and it is
        # asked at run time, where a no becomes a sentence in the status bar
        # rather than a build without visuals.
        find_package(OpenGL QUIET COMPONENTS OpenGL)

        if(TARGET OpenGL::GL)
            set(OPENDJ_VISUALIZER_AVAILABLE ON)
        else()
            message(STATUS "No OpenGL found: OpenDJ will build without the visualiser")
        endif()
    else()
        # The offscreen context is written against EGL and WGL, and nothing has
        # been written against CGL yet. Said plainly rather than failing to
        # configure, in the same spirit as the platform table in ROADMAP.md.
        message(STATUS "The visualiser is not written for macOS yet: building without it")
    endif()
endif()

if(OPENDJ_VISUALIZER_AVAILABLE)
    # v4, not the 3.x most distributions package: the C API used here only
    # exists in v4, and the two are not source compatible. Built here rather
    # than found so every platform gets the same version. LGPL 2.1 or later,
    # so licence compatible.
    FetchContent_Declare(projectm
        GIT_REPOSITORY https://github.com/projectM-visualizer/projectm.git
        GIT_TAG        v4.1.4
        GIT_SHALLOW    TRUE)

    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(ENABLE_PLAYLIST ON CACHE BOOL "" FORCE)
    set(ENABLE_SYSTEM_PROJECTM_EVAL OFF CACHE BOOL "" FORCE)
    set(ENABLE_SDL_UI OFF CACHE BOOL "" FORCE)
    set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

    if(WIN32)
        # projectM reaches OpenGL through GLEW on Windows -- its own
        # projectM-opengl.h includes <GL/glew.h> and <GL/wglew.h> there, where
        # on Linux it takes the entry points Mesa exports -- and it asks for it
        # with find_package(GLEW REQUIRED). Everyone else answers that with
        # vcpkg; this repository builds its dependencies, so GLEW is built here
        # too, like JUCE and Rubber Band and projectM itself.
        #
        # Perlmint's fork rather than the original because it ships the
        # generated glew.c. The original generates it from the OpenGL registry
        # as part of its release tarball, so its git tree alone will not build.
        # Modified BSD, so licence compatible.
        FetchContent_Declare(glew
            GIT_REPOSITORY https://github.com/Perlmint/glew-cmake.git
            GIT_TAG        glew-cmake-2.2.0-1
            GIT_SHALLOW    TRUE)

        set(glew-cmake_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(glew-cmake_BUILD_STATIC ON CACHE BOOL "" FORCE)
        set(ONLY_LIBS ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(glew)

        # projectM looks for a target called GLEW::glew, and no GLEW build
        # defines that name in its own tree: this one calls it libglew_static,
        # and the original only puts the GLEW:: namespace on an installed
        # export set. So the name it looks for is made here. Without this the
        # configure succeeds, GLEW quietly drops out of projectM's link line,
        # and the build ends in hundreds of unresolved __glew symbols.
        add_library(GLEW::glew ALIAS libglew_static)

        # ...and cmake/FindGLEW.cmake, which only reports that target, goes
        # where projectM's find_package(GLEW REQUIRED) will find it. Windows
        # only, so nothing about the Linux configure changes.
        list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
    endif()

    FetchContent_MakeAvailable(projectm)

    message(STATUS "projectM found: the visualiser will be built in")
endif()

# Sources shared by the application and the test binary, so the two cannot
# drift apart. Nothing in here includes a UI header.
set(OPENDJ_ENGINE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/core/AudioEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Deck.cpp
    ${CMAKE_SOURCE_DIR}/src/core/MicInput.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Mixer.cpp
    ${CMAKE_SOURCE_DIR}/src/core/OutputRouter.cpp
    ${CMAKE_SOURCE_DIR}/src/stream/Broadcaster.cpp
    ${CMAKE_SOURCE_DIR}/src/stream/IcecastConnection.cpp
    ${CMAKE_SOURCE_DIR}/src/stream/RtmpBroadcaster.cpp
    ${CMAKE_SOURCE_DIR}/src/stream/RtmpConnection.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Sampler.cpp
    ${CMAKE_SOURCE_DIR}/src/core/SetRecorder.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Mp3Writer.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/KeyDetector.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/TrackAnalysis.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/TrackAnalyser.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/TrackDecoder.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/StemDsp.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/StemSeparator.cpp
    ${CMAKE_SOURCE_DIR}/src/analysis/StemModel.cpp
    ${CMAKE_SOURCE_DIR}/src/control/Action.cpp
    ${CMAKE_SOURCE_DIR}/src/control/ActionDispatcher.cpp
    ${CMAKE_SOURCE_DIR}/src/control/MidiMapping.cpp
    ${CMAKE_SOURCE_DIR}/src/control/MidiControlSurface.cpp
    ${CMAKE_SOURCE_DIR}/src/control/AlsaMidiCompat.cpp
    ${CMAKE_SOURCE_DIR}/src/library/Library.cpp
    ${CMAKE_SOURCE_DIR}/src/library/LibraryScanner.cpp
    ${CMAKE_SOURCE_DIR}/src/library/TagReader.cpp
    ${CMAKE_SOURCE_DIR}/src/visual/Visualizer.cpp)

# ---------------------------------------------------------------------------
# Application target
# ---------------------------------------------------------------------------
juce_add_gui_app(OpenDJ
    PRODUCT_NAME  "OpenDJ"
    COMPANY_NAME  "OpenDJ"
    BUNDLE_ID     "me.interdo.opendj"
    VERSION       ${PROJECT_VERSION}
    # The same mark the repository carries, so the taskbar, the title bar and
    # the readme are recognisably one product. JUCE builds the platform icon
    # from these: an .ico on Windows, an .icns in a macOS bundle.
    ICON_BIG      ${CMAKE_SOURCE_DIR}/images/icon.png
    ICON_SMALL    ${CMAKE_SOURCE_DIR}/images/icon-256.png
    NEEDS_CURL    FALSE)

juce_generate_juce_header(OpenDJ)

target_sources(OpenDJ PRIVATE
    ${OPENDJ_ENGINE_SOURCES}
    src/app/Main.cpp
    src/app/MainComponent.cpp
    src/app/Settings.cpp
    src/ui/BeatMatchComponent.cpp
    src/ui/BrowserComponent.cpp
    src/ui/DeckComponent.cpp
    src/ui/MidiSetupComponent.cpp
    src/ui/PlatterComponent.cpp
    src/ui/WaveformComponent.cpp
    src/ui/MixerComponent.cpp
    src/ui/SamplerComponent.cpp
    src/ui/MicComponent.cpp
    src/ui/VisualizerComponent.cpp)

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

if(OpenVINO_FOUND)
    target_compile_definitions(OpenDJ PRIVATE OPENDJ_HAVE_OPENVINO=1)
    target_link_libraries(OpenDJ PRIVATE openvino::runtime)
endif()

if(OPENDJ_VISUALIZER_AVAILABLE)
    target_compile_definitions(OpenDJ PRIVATE OPENDJ_HAVE_VISUALIZER=1)
    target_link_libraries(OpenDJ PRIVATE libprojectM::projectM libprojectM::playlist)

    if(WIN32)
        # opengl32 for wgl* and the 1.1 entry points, GLEW for everything
        # after them, gdi32 for the pixel format and user32 for the hidden
        # window. JUCE asks for user32 with a pragma of its own, but the test
        # binary has no graphics module in it, so both are named here rather
        # than arriving by luck.
        target_link_libraries(OpenDJ PRIVATE GLEW::glew OpenGL::GL gdi32 user32)
    else()
        target_link_libraries(OpenDJ PRIVATE OpenGL::EGL OpenGL::GL)
    endif()
endif()

target_link_libraries(OpenDJ PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_devices
    juce::juce_dsp
    opendj_rubberband
    opendj_sqlite3
    opendj_lame
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(OpenDJ PRIVATE /utf-8)
endif()

# ---------------------------------------------------------------------------
# Installation
#
# The mappings have to land somewhere the application will look. It searches
# up from its own executable for share/opendj/mappings, so an installed
# /usr/local/bin/opendj finds /usr/local/share/opendj/mappings, and it reads
# the XDG data directories too, so a user's own mappings in
# ~/.local/share/opendj/mappings take precedence over the ones installed here.
#
# Everything here is in the "opendj" component, and installing means
#
#     cmake --install build --component opendj
#
# rather than a bare `cmake --install`. JUCE is brought in with FetchContent and
# carries its own install rules, which are not guarded by anything, so a plain
# install would also deposit the JUCE CMake package and its juceaide build tool
# into the prefix. Naming a component leaves all of that behind.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

# Lower case on the command line, whatever the product is called in the window
# title, because that is what a binary in the path is expected to look like.
install(PROGRAMS "$<TARGET_FILE:OpenDJ>"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        RENAME opendj
        COMPONENT opendj)

# `cmake --install --strip` only knows how to strip what install(TARGETS) put
# there, and the rename above rules that form out. The default configuration
# carries debug info that is over ninety percent of the binary, so honour the
# flag by hand for the one file it matters for.
if(CMAKE_STRIP)
    install(CODE "
        if(CMAKE_INSTALL_DO_STRIP)
            execute_process(COMMAND \"${CMAKE_STRIP}\"
                \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/opendj\")
        endif()"
        COMPONENT opendj)
endif()

install(DIRECTORY mappings/
        DESTINATION "${CMAKE_INSTALL_DATADIR}/opendj/mappings"
        COMPONENT opendj
        FILES_MATCHING PATTERN "*.json")

if(UNIX AND NOT APPLE)
    install(FILES packaging/opendj.desktop
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
            COMPONENT opendj)

    install(FILES packaging/opendj.svg
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
            COMPONENT opendj)
endif()

if(OPENDJ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
