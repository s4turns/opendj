/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "visual/Visualizer.h"

#include <algorithm>
#include <array>

#if OPENDJ_HAVE_VISUALIZER

// Mesa declares every core entry point past 1.1 in glext.h and libGL exports
// them, so the framebuffer calls below need no loader of their own. That is a
// Linux convenience rather than a portable one, and it is part of why this
// file is built only there; see the note in CMakeLists.txt.
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <projectM-4/projectM.h>
#include <projectM-4/playlist.h>

#endif

namespace opendj
{

#if OPENDJ_HAVE_VISUALIZER

namespace
{
    /** How much audio the FIFO holds. projectM only wants the most recent
        couple of thousand samples per frame, so this is already several
        frames' worth of slack at any sane rate; it exists so a render thread
        that stalls briefly costs nothing, not so that every sample survives.
        Samples that do not fit are counted and dropped, which is the right
        trade for something whose whole job is to look roughly right. */
    constexpr int fifoCapacitySamples = 16384;
}

//==============================================================================
class Visualizer::Renderer final : private juce::Thread
{
public:
    Renderer (Visualizer& ownerToUse, const VisualizerSettings& settingsToUse)
        : juce::Thread ("OpenDJ visualiser"),
          owner (ownerToUse),
          settings (settingsToUse),
          fifo (fifoCapacitySamples),
          fifoData ((size_t) fifoCapacitySamples * 2, 0.0f)
    {
        const auto pixels = (size_t) settings.width * settings.height * 3;

        for (auto& buffer : frames)
            buffer.assign (pixels, 0);

        scratch.assign (pixels, 0);
    }

    ~Renderer() override
    {
        stopThread (4000);
    }

    /** Starts the thread and waits for it to say whether the context and
        projectM came up, so `Visualizer::start` can report a real reason
        rather than a hopeful yes. */
    bool startUp (juce::String& error)
    {
        startThread (juce::Thread::Priority::normal);

        if (! startupDone.wait (15000))
        {
            error = "The visualiser did not finish starting within fifteen seconds.";
            signalThreadShouldExit();
            return false;
        }

        if (! startupSucceeded)
        {
            error = startupError;
            return false;
        }

        return true;
    }

    void shutDown()
    {
        signalThreadShouldExit();
        stopThread (4000);
    }

    //==========================================================================
    // Audio thread
    //==========================================================================

    void writeAudio (const juce::AudioBuffer<float>& master, int numSamples)
    {
        const auto scope = fifo.write (numSamples);
        const auto taken = scope.blockSize1 + scope.blockSize2;

        if (taken < numSamples)
            owner.droppedSamples.fetch_add (numSamples - taken, std::memory_order_relaxed);

        const auto* left = master.getReadPointer (0);
        const auto* right = master.getNumChannels() > 1 ? master.getReadPointer (1) : left;

        auto interleave = [&] (int start, int count, int sourceOffset)
        {
            for (int i = 0; i < count; ++i)
            {
                fifoData[(size_t) (start + i) * 2]     = left[sourceOffset + i];
                fifoData[(size_t) (start + i) * 2 + 1] = right[sourceOffset + i];
            }
        };

        interleave (scope.startIndex1, scope.blockSize1, 0);
        interleave (scope.startIndex2, scope.blockSize2, scope.blockSize1);
    }

    //==========================================================================
    // Any thread
    //==========================================================================

    bool copyLatestFrame (std::vector<unsigned char>& destination) const
    {
        const juce::ScopedLock lock (frameLock);

        if (readyFrame < 0)
            return false;

        destination = frames[(size_t) readyFrame];
        return true;
    }

    juce::String currentPreset() const
    {
        const juce::ScopedLock lock (presetLock);
        return presetName;
    }

    void requestNextPreset() { nextPresetWanted.store (true, std::memory_order_relaxed); }

private:
    //==========================================================================
    void run() override
    {
        // Everything OpenGL, projectM included, has to be made and destroyed
        // on the thread whose context it belongs to, which is why none of this
        // happens in the constructor.
        startupSucceeded = createContext (startupError) && createProjectM (startupError);
        startupDone.signal();

        if (! startupSucceeded)
        {
            destroyProjectM();
            destroyContext();
            return;
        }

        owner.running.store (true, std::memory_order_relaxed);

        const auto frameIntervalMs = 1000.0 / juce::jmax (1, settings.fps);
        auto nextFrameMs = juce::Time::getMillisecondCounterHiRes();

        while (! threadShouldExit())
        {
            feedAudioToProjectM();

            if (nextPresetWanted.exchange (false, std::memory_order_relaxed))
                projectm_playlist_play_next (playlist, true);

            renderOneFrame();

            // Paced against a running deadline rather than sleeping a fixed
            // interval, so a slow frame is absorbed instead of making every
            // frame after it late as well.
            nextFrameMs += frameIntervalMs;
            const auto waitMs = nextFrameMs - juce::Time::getMillisecondCounterHiRes();

            if (waitMs > 0.0)
                wait ((int) waitMs);
            else
                nextFrameMs = juce::Time::getMillisecondCounterHiRes();
        }

        owner.running.store (false, std::memory_order_relaxed);
        destroyProjectM();
        destroyContext();
    }

    void feedAudioToProjectM()
    {
        const auto wanted = (int) projectm_pcm_get_max_samples();
        const auto available = juce::jmin (wanted, fifo.getNumReady());

        if (available <= 0)
            return;

        const auto scope = fifo.read (available);
        pcm.resize ((size_t) available * 2);

        auto gather = [&] (int start, int count, int destinationOffset)
        {
            for (int i = 0; i < count; ++i)
            {
                pcm[(size_t) (destinationOffset + i) * 2]     = fifoData[(size_t) (start + i) * 2];
                pcm[(size_t) (destinationOffset + i) * 2 + 1] = fifoData[(size_t) (start + i) * 2 + 1];
            }
        };

        gather (scope.startIndex1, scope.blockSize1, 0);
        gather (scope.startIndex2, scope.blockSize2, scope.blockSize1);

        projectm_pcm_add_float (projectM, pcm.data(), (unsigned int) available, PROJECTM_STEREO);
    }

    void renderOneFrame()
    {
        glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
        glViewport (0, 0, settings.width, settings.height);

        projectm_opengl_render_frame (projectM);

        // Straight into a scratch buffer rather than the one a reader might
        // be copying out of, so a frame is only ever published whole.
        glReadPixels (0, 0, settings.width, settings.height,
                      GL_RGB, GL_UNSIGNED_BYTE, scratch.data());

        glBindFramebuffer (GL_FRAMEBUFFER, 0);

        // OpenGL hands rows back bottom first, and everything downstream --
        // the panel's image, ffmpeg's raw video -- wants them top first.
        // Turned over once here rather than in each of those, which would be
        // two places to get it wrong and one of them a `vflip` filter in a
        // command line nobody would think to look at.
        const auto rowBytes = (size_t) settings.width * 3;
        auto* rows = scratch.data();

        for (int top = 0, bottom = settings.height - 1; top < bottom; ++top, --bottom)
            std::swap_ranges (rows + (size_t) top * rowBytes,
                              rows + (size_t) (top + 1) * rowBytes,
                              rows + (size_t) bottom * rowBytes);

        publishFrame();
        owner.framesRendered.fetch_add (1, std::memory_order_relaxed);

        if (owner.frameSink != nullptr)
            owner.frameSink (scratch.data(), (int) scratch.size());
    }

    void publishFrame()
    {
        // Two buffers and an index rather than one buffer and a longer lock:
        // the swap is what a reader waits on, and it is a pointer's worth of
        // work, not a frame's. The swap itself happens outside the lock, and
        // what makes that safe is that a reader holds the lock for its whole
        // copy while this thread takes it once between every two writes to
        // the same buffer; so by the time a buffer is written again, whoever
        // was reading it has let go.
        const auto target = writeFrame;
        frames[(size_t) target].swap (scratch);

        {
            const juce::ScopedLock lock (frameLock);
            readyFrame = target;
        }

        writeFrame = 1 - target;
        scratch.resize ((size_t) settings.width * settings.height * 3);
    }

    //==========================================================================
    bool createContext (juce::String& error)
    {
        display = eglGetPlatformDisplay (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);

        if (display == EGL_NO_DISPLAY)
        {
            error = "No offscreen EGL display. A machine with no GPU driver cannot draw visuals.";
            return false;
        }

        if (! eglInitialize (display, nullptr, nullptr))
        {
            error = "EGL would not initialise.";
            display = EGL_NO_DISPLAY;
            return false;
        }

        const EGLint configAttributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                            EGL_NONE };
        EGLConfig config {};
        EGLint numConfigs = 0;

        if (! eglChooseConfig (display, configAttributes, &config, 1, &numConfigs) || numConfigs == 0)
        {
            error = "No EGL configuration this machine can render OpenGL into.";
            return false;
        }

        if (! eglBindAPI (EGL_OPENGL_API))
        {
            error = "This machine's EGL has no desktop OpenGL, only OpenGL ES.";
            return false;
        }

        // 3.3 core, which is what projectM's shaders are written against.
        const EGLint contextAttributes[] = { EGL_CONTEXT_MAJOR_VERSION, 3,
                                             EGL_CONTEXT_MINOR_VERSION, 3,
                                             EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                             EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                             EGL_NONE };

        context = eglCreateContext (display, config, EGL_NO_CONTEXT, contextAttributes);

        if (context == EGL_NO_CONTEXT)
        {
            error = "This machine's OpenGL is older than the 3.3 the visuals need.";
            return false;
        }

        // No surface at all: everything is drawn into a framebuffer object,
        // so there is nothing a window would be for.
        if (! eglMakeCurrent (display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
        {
            error = "The offscreen OpenGL context could not be made current.";
            return false;
        }

        return createFramebuffer (error);
    }

    bool createFramebuffer (juce::String& error)
    {
        glGenTextures (1, &colourTexture);
        glBindTexture (GL_TEXTURE_2D, colourTexture);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, settings.width, settings.height,
                      0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glGenFramebuffers (1, &framebuffer);
        glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, colourTexture, 0);

        // projectM draws with a depth buffer, and a framebuffer without one
        // renders the warp mesh in the wrong order.
        glGenRenderbuffers (1, &depthBuffer);
        glBindRenderbuffer (GL_RENDERBUFFER, depthBuffer);
        glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, settings.width, settings.height);
        glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                   GL_RENDERBUFFER, depthBuffer);

        if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            error = "The offscreen framebuffer came out incomplete at "
                  + juce::String (settings.width) + "x" + juce::String (settings.height) + ".";
            return false;
        }

        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        return true;
    }

    void destroyContext()
    {
        if (display == EGL_NO_DISPLAY)
            return;

        if (framebuffer != 0)    glDeleteFramebuffers (1, &framebuffer);
        if (depthBuffer != 0)    glDeleteRenderbuffers (1, &depthBuffer);
        if (colourTexture != 0)  glDeleteTextures (1, &colourTexture);

        eglMakeCurrent (display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (context != EGL_NO_CONTEXT)
            eglDestroyContext (display, context);

        eglTerminate (display);

        framebuffer = depthBuffer = colourTexture = 0;
        context = EGL_NO_CONTEXT;
        display = EGL_NO_DISPLAY;
    }

    //==========================================================================
    bool createProjectM (juce::String& error)
    {
        projectM = projectm_create();

        if (projectM == nullptr)
        {
            error = "projectM would not start.";
            return false;
        }

        projectm_set_window_size (projectM, (size_t) settings.width, (size_t) settings.height);
        projectm_set_fps (projectM, settings.fps);
        projectm_set_preset_duration (projectM, settings.presetDurationSeconds);
        projectm_set_beat_sensitivity (projectM, settings.beatSensitivity);

        playlist = projectm_playlist_create (projectM);

        if (playlist == nullptr)
        {
            error = "projectM's playlist would not start.";
            return false;
        }

        projectm_playlist_set_preset_switched_event_callback (playlist, presetSwitched, this);
        projectm_playlist_set_shuffle (playlist, true);

        // An empty or missing folder is deliberately not an error: projectM
        // has an idle preset built in, and drawing that is far better than
        // refusing to show anything because nobody has downloaded a preset
        // pack yet.
        if (settings.presetFolder.isDirectory())
            projectm_playlist_add_path (playlist,
                                        settings.presetFolder.getFullPathName().toRawUTF8(),
                                        true, false);

        if (projectm_playlist_size (playlist) > 0)
            projectm_playlist_play_next (playlist, true);

        return true;
    }

    void destroyProjectM()
    {
        if (playlist != nullptr)
        {
            projectm_playlist_destroy (playlist);
            playlist = nullptr;
        }

        if (projectM != nullptr)
        {
            projectm_destroy (projectM);
            projectM = nullptr;
        }
    }

    static void presetSwitched (bool, unsigned int index, void* userData)
    {
        auto* self = static_cast<Renderer*> (userData);

        if (auto* name = projectm_playlist_item (self->playlist, index))
        {
            const juce::ScopedLock lock (self->presetLock);
            self->presetName = juce::File (juce::String::fromUTF8 (name)).getFileNameWithoutExtension();
            projectm_playlist_free_string (name);
        }
    }

    //==========================================================================
    Visualizer& owner;
    VisualizerSettings settings;

    juce::AbstractFifo fifo;
    std::vector<float> fifoData;
    std::vector<float> pcm;

    // Two finished frames and one being drawn into, so a reader always has a
    // whole frame to copy while the next one is filled in.
    std::array<std::vector<unsigned char>, 2> frames;
    std::vector<unsigned char> scratch;
    int writeFrame = 0;
    int readyFrame = -1;
    juce::CriticalSection frameLock;

    juce::String presetName;
    mutable juce::CriticalSection presetLock;
    std::atomic<bool> nextPresetWanted { false };

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    GLuint framebuffer = 0, colourTexture = 0, depthBuffer = 0;

    projectm_handle projectM = nullptr;
    projectm_playlist_handle playlist = nullptr;

    juce::WaitableEvent startupDone;
    bool startupSucceeded = false;
    juce::String startupError;
};

#endif // OPENDJ_HAVE_VISUALIZER

//==============================================================================
Visualizer::Visualizer() = default;

Visualizer::~Visualizer()
{
    stop();
}

bool Visualizer::start ([[maybe_unused]] const VisualizerSettings& settings, juce::String& error)
{
#if OPENDJ_HAVE_VISUALIZER
    stop();

    frameWidth = settings.width;
    frameHeight = settings.height;

    renderer = std::make_unique<Renderer> (*this, settings);

    if (! renderer->startUp (error))
    {
        renderer.reset();

        const juce::ScopedLock lock (failureLock);
        failureReason = error;
        return false;
    }

    {
        const juce::ScopedLock lock (failureLock);
        failureReason.clear();
    }

    return true;
#else
    error = "This build has no visualiser: see the visualiser note in ROADMAP.md.";

    const juce::ScopedLock lock (failureLock);
    failureReason = error;
    return false;
#endif
}

void Visualizer::stop()
{
#if OPENDJ_HAVE_VISUALIZER
    if (renderer != nullptr)
    {
        renderer->shutDown();
        renderer.reset();
    }
#endif

    running.store (false, std::memory_order_relaxed);
}

void Visualizer::write ([[maybe_unused]] const juce::AudioBuffer<float>& master,
                        [[maybe_unused]] int numSamples)
{
#if OPENDJ_HAVE_VISUALIZER
    // The audio thread calls this whether or not anything is running, so the
    // cheap way out has to come first: one relaxed load and a branch.
    if (! running.load (std::memory_order_relaxed) || renderer == nullptr)
        return;

    if (numSamples > 0 && master.getNumChannels() > 0)
        renderer->writeAudio (master, numSamples);
#endif
}

bool Visualizer::copyLatestFrame ([[maybe_unused]] std::vector<unsigned char>& destination) const
{
#if OPENDJ_HAVE_VISUALIZER
    return renderer != nullptr && renderer->copyLatestFrame (destination);
#else
    return false;
#endif
}

juce::String Visualizer::getCurrentPresetName() const
{
#if OPENDJ_HAVE_VISUALIZER
    return renderer != nullptr ? renderer->currentPreset() : juce::String();
#else
    return {};
#endif
}

void Visualizer::nextPreset()
{
#if OPENDJ_HAVE_VISUALIZER
    if (renderer != nullptr)
        renderer->requestNextPreset();
#endif
}

juce::String Visualizer::getStatusMessage() const
{
    {
        const juce::ScopedLock lock (failureLock);

        if (failureReason.isNotEmpty())
            return "Visuals: " + failureReason;
    }

    if (! isRunning())
        return {};

    juce::String message;
    message << "Visuals " << getFrameWidth() << "x" << getFrameHeight();

    if (const auto preset = getCurrentPresetName(); preset.isNotEmpty())
        message << ", " << preset;

    return message;
}

} // namespace opendj
