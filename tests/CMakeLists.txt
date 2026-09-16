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
    ActionDispatcher_test.cpp
    AudioEngine_test.cpp
    Broadcast_test.cpp
    RtmpBroadcast_test.cpp
    Visualizer_test.cpp
    Mixer_test.cpp
    OutputRouter_test.cpp
    Deck_test.cpp
    Loop_test.cpp
    MicInput_test.cpp
    Sampler_test.cpp
    Settings_test.cpp
    ${CMAKE_SOURCE_DIR}/src/app/Settings.cpp
    SetRecorder_test.cpp
    TrackDecoder_test.cpp
    TrackAnalyser_test.cpp
    KeyDetector_test.cpp
    MidiMapping_test.cpp
    AngleMath_test.cpp
    StemDsp_test.cpp
    Library_test.cpp
    TagReader_test.cpp
    ${OPENDJ_ENGINE_SOURCES})

target_include_directories(opendj-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_definitions(opendj-tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_STANDALONE_APPLICATION=1)

if(OpenVINO_FOUND)
    target_compile_definitions(opendj-tests PRIVATE OPENDJ_HAVE_OPENVINO=1)
    target_link_libraries(opendj-tests PRIVATE openvino::runtime)
endif()

if(OPENDJ_VISUALIZER_AVAILABLE)
    target_compile_definitions(opendj-tests PRIVATE OPENDJ_HAVE_VISUALIZER=1)
    target_link_libraries(opendj-tests PRIVATE libprojectM::projectM libprojectM::playlist)

    if(WIN32)
        # opengl32 for wgl* and the 1.1 entry points, GLEW for everything
        # after them, gdi32 for the pixel format and user32 for the hidden
        # window. JUCE asks for user32 with a pragma of its own, but the test
        # binary has no graphics module in it, so both are named here rather
        # than arriving by luck.
        target_link_libraries(opendj-tests PRIVATE GLEW::glew OpenGL::GL gdi32 user32)
    else()
        target_link_libraries(opendj-tests PRIVATE OpenGL::EGL OpenGL::GL)
    endif()
endif()

target_link_libraries(opendj-tests PRIVATE
    Catch2::Catch2WithMain
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_dsp
    opendj_rubberband
    opendj_sqlite3
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(opendj-tests PRIVATE /utf-8)
endif()

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(opendj-tests)
