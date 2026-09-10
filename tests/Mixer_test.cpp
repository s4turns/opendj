/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Mixer.h"

#include <array>
#include <cmath>

using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    // Every gain in the mixer is smoothed over 20 ms, so anything that measures
    // a level has to let the ramps settle first.
    constexpr int blocksToSettle = 20;

    void fillSine (juce::AudioBuffer<float>& buffer, double frequency, double& phase, float amplitude)
    {
        const auto delta = juce::MathConstants<double>::twoPi * frequency / sampleRate;

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto value = static_cast<float> (std::sin (phase)) * amplitude;
            phase += delta;

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample (ch, i, value);
        }
    }

    struct Harness
    {
        Harness()
        {
            deckA.setSize (2, blockSize);
            deckB.setSize (2, blockSize);
            master.setSize (2, blockSize);
            cue.setSize (2, blockSize);

            mixer.prepare (sampleRate, blockSize);
            mixer.setMasterGain (1.0f);
            mixer.setChannelFader (0, 1.0f);
            mixer.setChannelFader (1, 1.0f);

            // Park the crossfader hard over on deck A so these measurements see
            // unity gain and read as the EQ response alone.
            mixer.setCrossfaderCurve (opendj::Mixer::CrossfaderCurve::linear);
            mixer.setCrossfaderPosition (-1.0f);
        }

        /** Runs the given signal through both decks until the smoothers settle,
            then returns the RMS of the master bus over one more block. */
        float measureMasterRms (double frequency, float amplitude = 0.1f)
        {
            double phaseA = 0.0, phaseB = 0.0;
            float rms = 0.0f;

            for (int block = 0; block <= blocksToSettle; ++block)
            {
                fillSine (deckA, frequency, phaseA, amplitude);
                fillSine (deckB, frequency, phaseB, 0.0f);

                std::array<juce::AudioBuffer<float>*, 2> decks { &deckA, &deckB };
                mixer.processBlock (decks, master, cue);

                rms = master.getRMSLevel (0, 0, blockSize);
            }

            return rms;
        }

        opendj::Mixer mixer;
        juce::AudioBuffer<float> deckA, deckB, master, cue;
    };

    /** Feeds a constant into each deck and reports the settled master level, so
        the crossfader law can be read directly off the output. */
    std::pair<float, float> measureCrossfaderGains (opendj::Mixer::CrossfaderCurve curve,
                                                    float position)
    {
        opendj::Mixer mixer;
        juce::AudioBuffer<float> deckA (2, blockSize), deckB (2, blockSize);
        juce::AudioBuffer<float> master (2, blockSize), cue (2, blockSize);

        mixer.prepare (sampleRate, blockSize);
        mixer.setMasterGain (1.0f);
        mixer.setChannelFader (0, 1.0f);
        mixer.setChannelFader (1, 1.0f);
        mixer.setCrossfaderCurve (curve);
        mixer.setCrossfaderPosition (position);

        // Direct current would be removed by the crossover, so use a mid band
        // tone and compare peak levels instead.
        double phaseA = 0.0, phaseB = 0.0;
        float peakA = 0.0f, peakB = 0.0f;

        for (int block = 0; block <= blocksToSettle; ++block)
        {
            fillSine (deckA, 1000.0, phaseA, 0.5f);
            deckB.clear();

            std::array<juce::AudioBuffer<float>*, 2> decks { &deckA, &deckB };
            mixer.processBlock (decks, master, cue);
            peakA = master.getMagnitude (0, 0, blockSize);
        }

        for (int block = 0; block <= blocksToSettle; ++block)
        {
            deckA.clear();
            fillSine (deckB, 1000.0, phaseB, 0.5f);

            std::array<juce::AudioBuffer<float>*, 2> decks { &deckA, &deckB };
            mixer.processBlock (decks, master, cue);
            peakB = master.getMagnitude (0, 0, blockSize);
        }

        // The source peak is 0.5, so divide it out to get the applied gain.
        return { peakA / 0.5f, peakB / 0.5f };
    }
}

TEST_CASE ("crossfader hard left passes deck A only", "[mixer][crossfader]")
{
    const auto [a, b] = measureCrossfaderGains (opendj::Mixer::CrossfaderCurve::constantPower, -1.0f);

    REQUIRE_THAT (a, WithinAbs (1.0f, 0.02f));
    REQUIRE_THAT (b, WithinAbs (0.0f, 0.001f));
}

TEST_CASE ("crossfader hard right passes deck B only", "[mixer][crossfader]")
{
    const auto [a, b] = measureCrossfaderGains (opendj::Mixer::CrossfaderCurve::constantPower, 1.0f);

    REQUIRE_THAT (a, WithinAbs (0.0f, 0.001f));
    REQUIRE_THAT (b, WithinAbs (1.0f, 0.02f));
}

TEST_CASE ("constant power crossfader holds level through the centre", "[mixer][crossfader]")
{
    const auto [a, b] = measureCrossfaderGains (opendj::Mixer::CrossfaderCurve::constantPower, 0.0f);

    // Both sides sit at -3 dB, so two uncorrelated tracks sum back to unity.
    REQUIRE_THAT (a, WithinAbs (0.7071f, 0.02f));
    REQUIRE_THAT (b, WithinAbs (0.7071f, 0.02f));
    REQUIRE_THAT (std::sqrt (a * a + b * b), WithinAbs (1.0f, 0.03f));
}

TEST_CASE ("linear crossfader halves both sides at the centre", "[mixer][crossfader]")
{
    const auto [a, b] = measureCrossfaderGains (opendj::Mixer::CrossfaderCurve::linear, 0.0f);

    REQUIRE_THAT (a, WithinAbs (0.5f, 0.02f));
    REQUIRE_THAT (b, WithinAbs (0.5f, 0.02f));
}

TEST_CASE ("cut crossfader is still at full level a quarter of the way across", "[mixer][crossfader]")
{
    // This is the whole point of the cut curve: the channel stays open almost
    // to the end of the throw, which is what makes transform and cut tricks work.
    const auto [a, b] = measureCrossfaderGains (opendj::Mixer::CrossfaderCurve::sharpCut, -0.5f);

    REQUIRE_THAT (a, WithinAbs (1.0f, 0.02f));
    REQUIRE_THAT (b, WithinAbs (1.0f, 0.02f));
}

TEST_CASE ("EQ at centre detent reconstructs the signal", "[mixer][eq]")
{
    Harness harness;

    for (int band = 0; band < 3; ++band)
        harness.mixer.setChannelEq (0, band, 0.5f);

    // A 0.1 amplitude sine has an RMS of 0.0707. The crossover bands must sum
    // back to that, or a flat EQ would not be flat.
    for (const double frequency : { 80.0, 1000.0, 8000.0 })
        REQUIRE_THAT (harness.measureMasterRms (frequency), WithinAbs (0.0707f, 0.004f));
}

TEST_CASE ("each EQ band kills its own range and leaves the others", "[mixer][eq]")
{
    SECTION ("low kill")
    {
        Harness harness;
        harness.mixer.setChannelEq (0, 0, 0.0f);

        REQUIRE (harness.measureMasterRms (60.0) < 0.002f);
        REQUIRE (harness.measureMasterRms (8000.0) > 0.06f);
    }

    SECTION ("mid kill")
    {
        Harness harness;
        harness.mixer.setChannelEq (0, 1, 0.0f);

        REQUIRE (harness.measureMasterRms (1000.0) < 0.004f);
        REQUIRE (harness.measureMasterRms (60.0) > 0.06f);
    }

    SECTION ("high kill")
    {
        Harness harness;
        harness.mixer.setChannelEq (0, 2, 0.0f);

        REQUIRE (harness.measureMasterRms (12000.0) < 0.002f);
        REQUIRE (harness.measureMasterRms (60.0) > 0.06f);
    }
}

TEST_CASE ("the cue bus ignores the channel fader and the crossfader", "[mixer][cue]")
{
    opendj::Mixer mixer;
    juce::AudioBuffer<float> deckA (2, blockSize), deckB (2, blockSize);
    juce::AudioBuffer<float> master (2, blockSize), cue (2, blockSize);

    mixer.prepare (sampleRate, blockSize);
    mixer.setCueGain (1.0f);
    mixer.setCueMix (0.0f);          // monitor the cued channel, not the master
    mixer.setChannelCue (0, true);
    mixer.setChannelFader (0, 0.0f); // fader all the way down
    mixer.setCrossfaderPosition (1.0f);

    double phase = 0.0;
    float masterPeak = 0.0f, cuePeak = 0.0f;

    for (int block = 0; block <= blocksToSettle; ++block)
    {
        fillSine (deckA, 1000.0, phase, 0.5f);
        deckB.clear();

        std::array<juce::AudioBuffer<float>*, 2> decks { &deckA, &deckB };
        mixer.processBlock (decks, master, cue);

        masterPeak = master.getMagnitude (0, 0, blockSize);
        cuePeak = cue.getMagnitude (0, 0, blockSize);
    }

    REQUIRE (masterPeak < 0.001f);
    REQUIRE (cuePeak > 0.4f);
}
