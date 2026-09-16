/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>

#include "visual/Visualizer.h"

#include <atomic>
#include <cmath>
#include <functional>
#include <numeric>

using namespace opendj;

namespace
{
    /** The same helper the broadcast tests use. */
    bool waitFor (std::function<bool()> condition, int timeoutMs = 5000)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (juce::Time::getMillisecondCounter() < deadline)
        {
            if (condition())
                return true;

            juce::Thread::sleep (25);
        }

        return condition();
    }

    /** Small, so the frames are quick to draw and quick to check. The size
        is arbitrary as far as projectM is concerned. */
    VisualizerSettings smallSettings()
    {
        VisualizerSettings settings;
        settings.width = 320;
        settings.height = 180;
        settings.fps = 30;
        return settings;
    }

    /** A loud 220 Hz tone in stereo: something with real energy, so the
        visuals have a reason to draw anything at all. */
    void fillWithTone (juce::AudioBuffer<float>& buffer, double& phase, double sampleRate = 48000.0)
    {
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto value = 0.8f * (float) std::sin (phase);
            phase += 2.0 * juce::MathConstants<double>::pi * 220.0 / sampleRate;

            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                buffer.setSample (channel, i, value);
        }
    }

    /** Starts a visualiser, or explains why this machine cannot, so the
        tests that need one can skip rather than fail. Every test here goes
        through this, since a build without EGL or a box with no GPU driver
        is a normal thing to run the suite on. */
    bool startOrSkip (Visualizer& visualizer, const VisualizerSettings& settings)
    {
        juce::String error;

        if (visualizer.start (settings, error))
            return true;

        WARN ("Skipping: " << error);
        return false;
    }
}

TEST_CASE ("a visualiser starts idle and takes audio without complaint", "[visual]")
{
    Visualizer visualizer;

    REQUIRE_FALSE (visualizer.isRunning());
    REQUIRE (visualizer.getFramesRendered() == 0);
    REQUIRE (visualizer.getStatusMessage().isEmpty());

    // Writing while stopped is what the audio callback does for the whole of
    // a set with no visuals on, so it has to be a no-op, not an error.
    juce::AudioBuffer<float> block (2, 512);
    block.clear();
    visualizer.write (block, 512);

    std::vector<unsigned char> frame;
    REQUIRE_FALSE (visualizer.copyLatestFrame (frame));
    REQUIRE (visualizer.getDroppedSamples() == 0);
}

TEST_CASE ("the visualiser draws real frames from a signal", "[visual][gpu]")
{
    Visualizer visualizer;
    const auto settings = smallSettings();

    if (! startOrSkip (visualizer, settings))
        return;

    REQUIRE (visualizer.isRunning());
    REQUIRE (visualizer.getFrameWidth() == settings.width);
    REQUIRE (visualizer.getFrameHeight() == settings.height);

    // Feed it a second of tone in audio-sized blocks, the way the callback
    // would, while the render thread draws whatever it has.
    juce::AudioBuffer<float> block (2, 512);
    double phase = 0.0;

    for (int i = 0; i < 48000 / 512; ++i)
    {
        fillWithTone (block, phase);
        visualizer.write (block, block.getNumSamples());
        juce::Thread::sleep (10);
    }

    REQUIRE (waitFor ([&] { return visualizer.getFramesRendered() >= 10; }));

    std::vector<unsigned char> frame;
    REQUIRE (visualizer.copyLatestFrame (frame));
    REQUIRE (frame.size() == (size_t) settings.width * settings.height * 3);

    // The one measurement that matters: something was actually drawn. A
    // framebuffer nothing rendered into reads back as all zeros, and so does
    // a context that quietly failed, so a total above zero is the difference
    // between a visualiser and a very elaborate way of producing black.
    const auto total = std::accumulate (frame.begin(), frame.end(), 0LL);
    INFO ("frame total " << total << " over " << frame.size() << " bytes");
    REQUIRE (total > 0);

    REQUIRE (visualizer.getStatusMessage().startsWith ("Visuals 320x180"));

    visualizer.stop();
    REQUIRE_FALSE (visualizer.isRunning());
}

TEST_CASE ("every finished frame reaches the sink whole", "[visual][gpu]")
{
    Visualizer visualizer;
    const auto settings = smallSettings();

    std::atomic<int> framesSeen { 0 };
    std::atomic<int> wrongSized { 0 };
    std::atomic<long long> brightestTotal { 0 };
    const auto expectedBytes = settings.width * settings.height * 3;

    visualizer.setFrameSink ([&] (const unsigned char* rgb, int numBytes)
    {
        if (numBytes != expectedBytes)
            ++wrongSized;

        long long total = 0;
        for (int i = 0; i < numBytes; ++i)
            total += rgb[i];

        auto best = brightestTotal.load();
        while (total > best && ! brightestTotal.compare_exchange_weak (best, total)) {}

        ++framesSeen;
    });

    if (! startOrSkip (visualizer, settings))
        return;

    juce::AudioBuffer<float> block (2, 512);
    double phase = 0.0;

    for (int i = 0; i < 40; ++i)
    {
        fillWithTone (block, phase);
        visualizer.write (block, block.getNumSamples());
        juce::Thread::sleep (10);
    }

    REQUIRE (waitFor ([&] { return framesSeen.load() >= 10; }));
    visualizer.stop();

    REQUIRE (wrongSized.load() == 0);
    REQUIRE (brightestTotal.load() > 0);

    // The sink and the counter must agree: a frame that was drawn but not
    // pushed, or pushed twice, is exactly the bug a broadcast would show as
    // a stutter.
    REQUIRE (framesSeen.load() == visualizer.getFramesRendered());
}

TEST_CASE ("audio the render thread could not take is counted, not lost quietly", "[visual][gpu]")
{
    Visualizer visualizer;

    if (! startOrSkip (visualizer, smallSettings()))
        return;

    // Far more than the FIFO holds, all at once and faster than any frame
    // could drain it. Every sample that did not fit has to show in the
    // count; the alternative, blocking until there is room, is the one thing
    // the audio thread must never do.
    juce::AudioBuffer<float> block (2, 4096);
    double phase = 0.0;

    for (int i = 0; i < 20; ++i)
    {
        fillWithTone (block, phase);
        visualizer.write (block, block.getNumSamples());
    }

    REQUIRE (visualizer.getDroppedSamples() > 0);
    REQUIRE (visualizer.getDroppedSamples() < 20 * 4096);   // but not everything
}

TEST_CASE ("stopping is safe at any point, including twice and never", "[visual][gpu]")
{
    {
        Visualizer visualizer;

        if (! startOrSkip (visualizer, smallSettings()))
            return;

        visualizer.stop();
        visualizer.stop();
        REQUIRE_FALSE (visualizer.isRunning());

        // And again from stopped, so a second set of visuals in one session
        // is a restart rather than a leak.
        REQUIRE (startOrSkip (visualizer, smallSettings()));
        REQUIRE (waitFor ([&] { return visualizer.getFramesRendered() >= 1; }));
    }
    // Destroyed while running: the destructor has to stop the thread itself.

    SUCCEED();
}
