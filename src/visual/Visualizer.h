/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace opendj
{

/** How the visuals are drawn. The size is the size frames are produced at,
    which is the size they reach the broadcast at; the panel in the window
    scales whatever it is given, so this is not the panel's size. */
struct VisualizerSettings
{
    int width = 1280;
    int height = 720;

    /** Frames a second. The render thread paces itself to this, so it is also
        what the broadcast's video track runs at. */
    int fps = 30;

    /** How long one preset is held before moving to the next. */
    double presetDurationSeconds = 30.0;

    /** How readily projectM decides a beat happened. Its own default is 1. */
    float beatSensitivity = 1.0f;

    /** Where the `.milk` presets are. An empty or absent folder is not an
        error: projectM has an idle preset built in, and drawing that beats
        refusing to start. */
    juce::File presetFolder;
};

/** Draws MilkDrop visuals from what the master output is doing, for the panel
    in the window and for the broadcast's video track.

    The audio thread's part is deliberately tiny, for the same reason it is
    tiny in `Broadcaster`: `write` copies into a lock free FIFO and returns.
    Everything else -- projectM, OpenGL, reading pixels back off the GPU --
    happens on this class's own thread, where it is allowed to be slow.

    That thread owns an OpenGL context that belongs to no window. This is the
    one design decision here worth stating plainly, because the obvious
    alternative is to hang the visuals off a `juce::OpenGLContext` attached to
    the panel, and that would tie them to the panel being open: close it and
    the broadcast's video track would stop mid-set. A context of its own also
    means a test can render real frames and measure real pixels on a machine
    with no display, which is what the tests beside this class do.
*/
class Visualizer
{
public:
    Visualizer();
    ~Visualizer();

    //==========================================================================
    // Message thread
    //==========================================================================

    /** Starts the render thread. Returns false with `error` filled in when no
        offscreen context could be made or projectM would not start, which on
        a machine with no usable GPU is a normal thing to be told rather than
        a fault. */
    bool start (const VisualizerSettings& settings, juce::String& error);

    void stop();

    bool isRunning() const noexcept { return running.load (std::memory_order_relaxed); }

    /** A sentence for the status bar, including why the visuals failed. */
    juce::String getStatusMessage() const;

    juce::int64 getFramesRendered() const noexcept
    {
        return framesRendered.load (std::memory_order_relaxed);
    }

    /** Samples the render thread never saw because it was busy when they
        arrived. Harmless in small numbers -- the visuals only need to know
        roughly what the music is doing -- and worth showing if it is not
        small, the same reason `Broadcaster` shows its own. */
    juce::int64 getDroppedSamples() const noexcept
    {
        return droppedSamples.load (std::memory_order_relaxed);
    }

    /** The newest frame, tightly packed RGB, copied into `destination`, which
        is resized to fit. False before the first frame has been drawn.

        A copy rather than a pointer because the render thread overwrites its
        own buffers as it goes, and a caller holding a pointer into one would
        be reading a frame being drawn over. Frames are small enough and rare
        enough that the copy does not matter.

        Deliberately raw bytes rather than a `juce::Image`: this class is
        built into the test binary, which has no graphics module, and the
        panel is the right place to know about images. */
    bool copyLatestFrame (std::vector<unsigned char>& destination) const;

    int getFrameWidth() const noexcept { return frameWidth; }
    int getFrameHeight() const noexcept { return frameHeight; }

    /** Called on the render thread with every finished frame, tightly packed
        RGB, before the next one is started. This is how frames reach the
        broadcast: a push from the thread that has them, rather than a
        second thread polling `copyLatestFrame` and hoping to keep up. The
        sink is called with the visualiser's own buffer and must not keep
        the pointer. Set before `start`; not read after. */
    using FrameSink = std::function<void (const unsigned char* rgb, int numBytes)>;
    void setFrameSink (FrameSink sink) { frameSink = std::move (sink); }

    /** The preset being drawn, or empty before the first one loads. */
    juce::String getCurrentPresetName() const;

    /** Moves to the next preset without waiting out the current one. */
    void nextPreset();

    //==========================================================================
    // Audio thread
    //==========================================================================

    /** Hands one block to the visuals. Realtime safe: a copy into a FIFO and
        nothing else. Does nothing at all when the visualiser is not running,
        so the audio callback can call it unconditionally. */
    void write (const juce::AudioBuffer<float>& master, int numSamples);

private:
    /** The render thread, the offscreen context and projectM itself, all of
        which need headers that have no business in this one. */
    class Renderer;

    std::unique_ptr<Renderer> renderer;

    std::atomic<bool> running { false };
    std::atomic<juce::int64> framesRendered { 0 };
    std::atomic<juce::int64> droppedSamples { 0 };

    int frameWidth = 0;
    int frameHeight = 0;
    FrameSink frameSink;

    juce::String failureReason;
    mutable juce::CriticalSection failureLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Visualizer)
};

} // namespace opendj
