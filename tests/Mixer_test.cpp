/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/AudioEngine.h"
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

                std::array<juce::AudioBuffer<float>*, opendj::Mixer::numChannels> decks { &deckA, &deckB };
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

            std::array<juce::AudioBuffer<float>*, opendj::Mixer::numChannels> decks { &deckA, &deckB };
            mixer.processBlock (decks, master, cue);
            peakA = master.getMagnitude (0, 0, blockSize);
        }

        for (int block = 0; block <= blocksToSettle; ++block)
        {
            deckA.clear();
            fillSine (deckB, 1000.0, phaseB, 0.5f);

            std::array<juce::AudioBuffer<float>*, opendj::Mixer::numChannels> decks { &deckA, &deckB };
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

        std::array<juce::AudioBuffer<float>*, opendj::Mixer::numChannels> decks { &deckA, &deckB };
        mixer.processBlock (decks, master, cue);

        masterPeak = master.getMagnitude (0, 0, blockSize);
        cuePeak = cue.getMagnitude (0, 0, blockSize);
    }

    REQUIRE (masterPeak < 0.001f);
    REQUIRE (cuePeak > 0.4f);
}

TEST_CASE ("the filter knob is transparent in the middle", "[mixer][filter]")
{
    Harness harness;
    harness.mixer.setChannelFilter (0, 0.5f);

    // Both filters are parked outside the audible range at centre, so a tone
    // should arrive at the level the EQ alone would give it.
    const auto centred = harness.measureMasterRms (1000.0);
    REQUIRE (centred > 0.01f);

    Harness reference;
    REQUIRE_THAT (centred, WithinAbs (reference.measureMasterRms (1000.0), 0.005f));
}

TEST_CASE ("turning the filter down takes the top off", "[mixer][filter]")
{
    Harness harness;
    harness.mixer.setChannelFilter (0, 0.5f);
    const auto open = harness.measureMasterRms (6000.0);

    harness.mixer.setChannelFilter (0, 0.0f);
    const auto closed = harness.measureMasterRms (6000.0);

    INFO ("6 kHz: open " << open << ", closed " << closed);
    REQUIRE (open > 0.01f);
    REQUIRE (closed < open * 0.25f);
}

TEST_CASE ("turning the filter up takes the bottom out", "[mixer][filter]")
{
    Harness harness;
    harness.mixer.setChannelFilter (0, 0.5f);
    const auto open = harness.measureMasterRms (80.0);

    harness.mixer.setChannelFilter (0, 1.0f);
    const auto closed = harness.measureMasterRms (80.0);

    INFO ("80 Hz: open " << open << ", closed " << closed);
    REQUIRE (open > 0.01f);
    REQUIRE (closed < open * 0.25f);
}

TEST_CASE ("a filtered channel still passes what is left of it", "[mixer][filter]")
{
    // Turning the knob down should not simply mute the channel: the bass has to
    // survive, which is the whole point of a DJ filter.
    Harness harness;
    harness.mixer.setChannelFilter (0, 0.0f);

    REQUIRE (harness.measureMasterRms (100.0) > 0.01f);
}

TEST_CASE ("the filter position is reported back for the interface", "[mixer][filter]")
{
    opendj::Mixer mixer;

    REQUIRE_THAT (mixer.getChannelFilter (0), WithinAbs (0.5f, 0.001f));

    mixer.setChannelFilter (0, 0.25f);
    REQUIRE_THAT (mixer.getChannelFilter (0), WithinAbs (0.25f, 0.001f));

    // Out of range values are clamped rather than believed.
    mixer.setChannelFilter (0, 5.0f);
    REQUIRE_THAT (mixer.getChannelFilter (0), WithinAbs (1.0f, 0.001f));

    // An unknown channel answers with the neutral position rather than crashing.
    REQUIRE_THAT (mixer.getChannelFilter (99), WithinAbs (0.5f, 0.001f));
}

//==============================================================================
// Echo
//==============================================================================

namespace
{
    /** Sends a single click through one channel and returns the master output,
        so the repeats can be found by looking for them rather than inferred. */
    std::vector<float> echoImpulseResponse (float amount, double echoSeconds, int blocks)
    {
        opendj::Mixer mixer;
        juce::AudioBuffer<float> deckA (2, blockSize), deckB (2, blockSize);
        juce::AudioBuffer<float> master (2, blockSize), cue (2, blockSize);

        mixer.prepare (sampleRate, blockSize);
        mixer.setMasterGain (1.0f);
        mixer.setChannelFader (0, 1.0f);
        mixer.setCrossfaderCurve (opendj::Mixer::CrossfaderCurve::linear);
        mixer.setCrossfaderPosition (-1.0f);
        mixer.setChannelEchoTime (0, echoSeconds);
        mixer.setChannelEcho (0, amount);

        // Every gain in the mixer ramps from zero, so a click sent in the first
        // block is multiplied away before it reaches the master. Let the ramps
        // finish on silence first, which is also what happens in practice.
        for (int b = 0; b <= blocksToSettle; ++b)
        {
            deckA.clear();
            deckB.clear();
            std::array<juce::AudioBuffer<float>*, opendj::Mixer::numChannels> decks { &deckA, &deckB };
            mixer.processBlock (decks, master, cue);
        }

        std::vector<float> out;
        out.reserve ((size_t) blocks * blockSize);

        for (int b = 0; b < blocks; ++b)
        {
            deckA.clear();
            deckB.clear();

            // One click, in the first block after settling.
            if (b == 0)
                for (int ch = 0; ch < 2; ++ch)
                    deckA.setSample (ch, 0, 0.5f);

            std::array<juce::AudioBuffer<float>*, opendj::Mixer::numChannels> decks { &deckA, &deckB };
            mixer.processBlock (decks, master, cue);

            for (int i = 0; i < blockSize; ++i)
                out.push_back (master.getSample (0, i));
        }

        return out;
    }

    /** The loudest sample within a window. A click through the crossover rings
        for a few dozen samples rather than arriving as a single spike, so a
        repeat is a burst to be measured, not an index to be found. */
    float peakNear (const std::vector<float>& signal, int centre, int radius)
    {
        const auto from = juce::jmax (0, centre - radius);
        const auto to = juce::jmin ((int) signal.size(), centre + radius);

        auto peak = 0.0f;

        for (int i = from; i < to; ++i)
            peak = juce::jmax (peak, std::abs (signal[(size_t) i]));

        return peak;
    }
}

TEST_CASE ("the echo is silent until it is turned up", "[mixer][echo]")
{
    const auto echoSeconds = 0.25;
    const auto dry = echoImpulseResponse (0.0f, echoSeconds, 40);
    const auto spacing = (int) (echoSeconds * sampleRate);

    // The click arrives, and nothing comes back after it.
    REQUIRE (peakNear (dry, 0, 300) > 0.01f);
    REQUIRE (peakNear (dry, spacing, 300) < 0.0005f);
    REQUIRE (peakNear (dry, spacing * 2, 300) < 0.0005f);
}

TEST_CASE ("the echo repeats at the time it was given", "[mixer][echo]")
{
    const auto echoSeconds = 0.25;
    const auto response = echoImpulseResponse (0.7f, echoSeconds, 80);
    const auto spacing = (int) (echoSeconds * sampleRate);

    // Loud where a repeat is due and quiet halfway between. Neither half alone
    // would show the delay length is right: a wash is loud everywhere, and
    // silence is quiet everywhere.
    for (int repeat = 0; repeat < 4; ++repeat)
    {
        const auto onBeat = peakNear (response, repeat * spacing, 300);
        const auto between = peakNear (response, (int) ((repeat + 0.5) * spacing), 300);

        INFO ("repeat " << repeat << ": on " << onBeat << ", between " << between);
        REQUIRE (onBeat > 0.01f);
        REQUIRE (between < onBeat * 0.2f);
    }
}

TEST_CASE ("each repeat is quieter than the one before", "[mixer][echo]")
{
    // An echo that does not decay is a mixer that will be left howling.
    const auto echoSeconds = 0.1;
    const auto response = echoImpulseResponse (0.8f, echoSeconds, 80);
    const auto spacing = (int) (echoSeconds * sampleRate);

    auto previous = peakNear (response, 0, 300);

    for (int repeat = 1; repeat < 5; ++repeat)
    {
        const auto level = peakNear (response, repeat * spacing, 300);

        INFO ("repeat " << repeat << " level " << level << " against " << previous);
        REQUIRE (level > 0.0f);
        REQUIRE (level < previous);
        previous = level;
    }
}

TEST_CASE ("the echo dies away rather than running for ever", "[mixer][echo]")
{
    // Ten seconds after one click at full amount, there must be nothing left.
    const auto blocks = (int) (sampleRate * 10.0 / blockSize);
    const auto response = echoImpulseResponse (1.0f, 0.1, blocks);

    auto tail = 0.0f;

    for (size_t i = response.size() * 9 / 10; i < response.size(); ++i)
        tail = juce::jmax (tail, std::abs (response[i]));

    INFO ("tail level " << tail);
    REQUIRE (tail < 0.001f);
}

TEST_CASE ("the echo time is clamped to something playable", "[mixer][echo]")
{
    opendj::Mixer mixer;
    mixer.prepare (sampleRate, blockSize);

    mixer.setChannelEchoTime (0, 1000.0);
    REQUIRE (mixer.getChannelEchoTime (0) <= 4.0);

    mixer.setChannelEchoTime (0, 0.0);
    REQUIRE (mixer.getChannelEchoTime (0) >= 0.02);

    // An out of range channel answers rather than writing past the end.
    mixer.setChannelEchoTime (7, 1.0);
    REQUIRE (mixer.getChannelEcho (7) == 0.0f);
}

TEST_CASE ("the echo on one channel leaves the other alone", "[mixer][echo]")
{
    opendj::Mixer mixer;
    juce::AudioBuffer<float> deckA (2, blockSize), deckB (2, blockSize);
    juce::AudioBuffer<float> master (2, blockSize), cue (2, blockSize);

    mixer.prepare (sampleRate, blockSize);
    mixer.setMasterGain (1.0f);
    mixer.setChannelFader (0, 1.0f);
    mixer.setChannelFader (1, 1.0f);
    mixer.setCrossfaderCurve (opendj::Mixer::CrossfaderCurve::linear);
    mixer.setCrossfaderPosition (1.0f);          // hard over on B
    mixer.setChannelEcho (0, 1.0f);              // and the echo is on A
    mixer.setChannelEchoTime (0, 0.1);

    for (int b = 0; b < 40 + blocksToSettle; ++b)
    {
        deckA.clear();
        deckB.clear();

        if (b == blocksToSettle)
            for (int ch = 0; ch < 2; ++ch)
                deckA.setSample (ch, 0, 0.5f);

        std::array<juce::AudioBuffer<float>*, opendj::Mixer::numChannels> decks { &deckA, &deckB };
        mixer.processBlock (decks, master, cue);

        REQUIRE (master.getMagnitude (0, 0, blockSize) < 0.001f);
    }
}

//==============================================================================
// Reverb
//==============================================================================

namespace
{
    /** A click through one channel with the reverb at `amount`, returning the
        master output so the tail can be measured rather than assumed. */
    std::vector<float> reverbImpulseResponse (float amount, int blocks)
    {
        opendj::Mixer mixer;
        juce::AudioBuffer<float> deckA (2, blockSize), deckB (2, blockSize);
        juce::AudioBuffer<float> master (2, blockSize), cue (2, blockSize);

        mixer.prepare (sampleRate, blockSize);
        mixer.setMasterGain (1.0f);
        mixer.setChannelFader (0, 1.0f);
        mixer.setCrossfaderCurve (opendj::Mixer::CrossfaderCurve::linear);
        mixer.setCrossfaderPosition (-1.0f);
        mixer.setChannelReverb (0, amount);

        for (int b = 0; b <= blocksToSettle; ++b)
        {
            deckA.clear();
            deckB.clear();
            std::array<juce::AudioBuffer<float>*, opendj::Mixer::numChannels> decks { &deckA, &deckB };
            mixer.processBlock (decks, master, cue);
        }

        std::vector<float> out;

        for (int b = 0; b < blocks; ++b)
        {
            deckA.clear();
            deckB.clear();

            if (b == 0)
                for (int ch = 0; ch < 2; ++ch)
                    deckA.setSample (ch, 0, 0.5f);

            std::array<juce::AudioBuffer<float>*, opendj::Mixer::numChannels> decks { &deckA, &deckB };
            mixer.processBlock (decks, master, cue);

            for (int i = 0; i < blockSize; ++i)
                out.push_back (master.getSample (0, i));
        }

        return out;
    }
}

TEST_CASE ("the reverb is silent until it is turned up", "[mixer][reverb]")
{
    const auto dry = reverbImpulseResponse (0.0f, 60);

    // The click arrives and nothing rings on after it.
    REQUIRE (peakNear (dry, 0, 300) > 0.01f);

    auto tail = 0.0f;

    for (size_t i = dry.size() / 4; i < dry.size(); ++i)
        tail = juce::jmax (tail, std::abs (dry[i]));

    INFO ("tail with the knob at zero: " << tail);
    REQUIRE (tail < 0.0005f);
}

TEST_CASE ("the reverb rings on after the sound stops", "[mixer][reverb]")
{
    const auto wet = reverbImpulseResponse (1.0f, 60);

    // A quarter of a second later there is still something there, which is the
    // whole point, and it is not simply the click smeared: it is later than any
    // filter delay could account for.
    const auto quarterSecond = (int) (sampleRate * 0.25);
    const auto ringing = peakNear (wet, quarterSecond, 2000);

    INFO ("level a quarter of a second after the click: " << ringing);
    REQUIRE (ringing > 0.0005f);
}

TEST_CASE ("the reverb decays rather than sustaining", "[mixer][reverb]")
{
    const auto wet = reverbImpulseResponse (1.0f, 400);        // about four seconds

    const auto early = peakNear (wet, (int) (sampleRate * 0.2), 4000);
    const auto late  = peakNear (wet, (int) (sampleRate * 3.5), 4000);

    INFO ("early " << early << ", late " << late);
    REQUIRE (early > late);
    REQUIRE (late < 0.001f);
}

TEST_CASE ("turning the reverb down does not cut the tail off", "[mixer][reverb]")
{
    // The knob controls how much of the tail is heard, not whether the reverb
    // is running. Cutting a tail dead is the one thing a reverb must not do.
    opendj::Mixer mixer;
    juce::AudioBuffer<float> deckA (2, blockSize), deckB (2, blockSize);
    juce::AudioBuffer<float> master (2, blockSize), cue (2, blockSize);

    mixer.prepare (sampleRate, blockSize);
    mixer.setMasterGain (1.0f);
    mixer.setChannelFader (0, 1.0f);
    mixer.setCrossfaderCurve (opendj::Mixer::CrossfaderCurve::linear);
    mixer.setCrossfaderPosition (-1.0f);
    mixer.setChannelReverb (0, 1.0f);

    std::array<juce::AudioBuffer<float>*, opendj::Mixer::numChannels> decks { &deckA, &deckB };

    for (int b = 0; b <= blocksToSettle; ++b)
    {
        deckA.clear(); deckB.clear();
        mixer.processBlock (decks, master, cue);
    }

    // Click, let it ring, then close the knob.
    deckA.clear(); deckB.clear();
    for (int ch = 0; ch < 2; ++ch) deckA.setSample (ch, 0, 0.5f);
    mixer.processBlock (decks, master, cue);

    for (int b = 0; b < 10; ++b)
    {
        deckA.clear(); deckB.clear();
        mixer.processBlock (decks, master, cue);
    }

    mixer.setChannelReverb (0, 0.0f);

    // Immediately after, the tail is fading rather than gone: the smoothing
    // takes it down over milliseconds, not in one block.
    deckA.clear(); deckB.clear();
    mixer.processBlock (decks, master, cue);

    const auto justAfter = master.getMagnitude (0, 0, blockSize);
    INFO ("level in the block after the knob closed: " << justAfter);
    REQUIRE (justAfter > 0.0f);
}

TEST_CASE ("the reverb on one channel leaves the other alone", "[mixer][reverb]")
{
    opendj::Mixer mixer;
    juce::AudioBuffer<float> deckA (2, blockSize), deckB (2, blockSize);
    juce::AudioBuffer<float> master (2, blockSize), cue (2, blockSize);

    mixer.prepare (sampleRate, blockSize);
    mixer.setMasterGain (1.0f);
    mixer.setChannelFader (0, 1.0f);
    mixer.setChannelFader (1, 1.0f);
    mixer.setCrossfaderCurve (opendj::Mixer::CrossfaderCurve::linear);
    mixer.setCrossfaderPosition (1.0f);        // hard over on B
    mixer.setChannelReverb (0, 1.0f);          // and the reverb is on A

    std::array<juce::AudioBuffer<float>*, opendj::Mixer::numChannels> decks { &deckA, &deckB };

    for (int b = 0; b < 60 + blocksToSettle; ++b)
    {
        deckA.clear();
        deckB.clear();

        if (b == blocksToSettle)
            for (int ch = 0; ch < 2; ++ch)
                deckA.setSample (ch, 0, 0.5f);

        mixer.processBlock (decks, master, cue);
        REQUIRE (master.getMagnitude (0, 0, blockSize) < 0.001f);
    }
}

TEST_CASE ("an out of range channel is answered, not written past", "[mixer][reverb]")
{
    opendj::Mixer mixer;
    mixer.prepare (sampleRate, blockSize);

    mixer.setChannelReverb (9, 1.0f);
    REQUIRE (mixer.getChannelReverb (9) == 0.0f);
    REQUIRE (mixer.getChannelReverb (-1) == 0.0f);
}

//==============================================================================
// Four channels, and which of them the crossfader reaches.

namespace
{
    /** The level one channel reaches the master at, with the crossfader parked
        where the test wants it. */
    float measureChannelGain (int channel, float crossfaderPosition,
                              opendj::Mixer::CrossfaderAssign assign)
    {
        opendj::Mixer mixer;
        mixer.prepare (sampleRate, blockSize);
        mixer.setMasterGain (1.0f);
        mixer.setChannelFader (channel, 1.0f);
        mixer.setChannelCrossfaderAssign (channel, assign);
        mixer.setCrossfaderCurve (opendj::Mixer::CrossfaderCurve::linear);
        mixer.setCrossfaderPosition (crossfaderPosition);

        juce::AudioBuffer<float> source (2, blockSize), master (2, blockSize), cue (2, blockSize);

        std::array<juce::AudioBuffer<float>*, opendj::Mixer::numChannels> decks {};
        decks[(size_t) channel] = &source;

        double phase = 0.0;
        auto peak = 0.0f;

        for (int block = 0; block <= blocksToSettle; ++block)
        {
            fillSine (source, 1000.0, phase, 0.5f);
            mixer.processBlock (decks, master, cue);
            peak = master.getMagnitude (0, 0, blockSize);
        }

        return peak / 0.5f;
    }
}

TEST_CASE ("the mixer has a strip for every deck", "[mixer]")
{
    REQUIRE (opendj::Mixer::numChannels == 4);
    REQUIRE (opendj::AudioEngine::numDecks == opendj::Mixer::numChannels);
}

TEST_CASE ("C and D ignore the crossfader unless they are told not to", "[mixer][crossfader]")
{
    // The default for the third and fourth channels, which is what makes them
    // useful as an addition to a mix rather than one side of it.
    REQUIRE_THAT (measureChannelGain (2, -1.0f, opendj::Mixer::CrossfaderAssign::thru),
                  WithinAbs (1.0f, 0.02f));
    REQUIRE_THAT (measureChannelGain (2, 1.0f, opendj::Mixer::CrossfaderAssign::thru),
                  WithinAbs (1.0f, 0.02f));
}

TEST_CASE ("any channel can be put on either side of the crossfader", "[mixer][crossfader]")
{
    // Channel C assigned to the A side is silent at the far end of the throw
    // and at full level at the near one, exactly as channel A would be.
    REQUIRE_THAT (measureChannelGain (2, -1.0f, opendj::Mixer::CrossfaderAssign::a),
                  WithinAbs (1.0f, 0.02f));
    REQUIRE_THAT (measureChannelGain (2, 1.0f, opendj::Mixer::CrossfaderAssign::a),
                  WithinAbs (0.0f, 0.002f));

    REQUIRE_THAT (measureChannelGain (3, 1.0f, opendj::Mixer::CrossfaderAssign::b),
                  WithinAbs (1.0f, 0.02f));
    REQUIRE_THAT (measureChannelGain (3, -1.0f, opendj::Mixer::CrossfaderAssign::b),
                  WithinAbs (0.0f, 0.002f));
}

TEST_CASE ("the assignment survives being read back", "[mixer][crossfader]")
{
    opendj::Mixer mixer;

    // The two the crossfader was built for start on it; the two that were added
    // start beside it.
    REQUIRE (mixer.getChannelCrossfaderAssign (0) == opendj::Mixer::CrossfaderAssign::a);
    REQUIRE (mixer.getChannelCrossfaderAssign (1) == opendj::Mixer::CrossfaderAssign::b);
    REQUIRE (mixer.getChannelCrossfaderAssign (2) == opendj::Mixer::CrossfaderAssign::thru);
    REQUIRE (mixer.getChannelCrossfaderAssign (3) == opendj::Mixer::CrossfaderAssign::thru);

    mixer.setChannelCrossfaderAssign (3, opendj::Mixer::CrossfaderAssign::a);
    REQUIRE (mixer.getChannelCrossfaderAssign (3) == opendj::Mixer::CrossfaderAssign::a);

    // And a channel that does not exist is answered rather than crashed into.
    mixer.setChannelCrossfaderAssign (99, opendj::Mixer::CrossfaderAssign::b);
    REQUIRE (mixer.getChannelCrossfaderAssign (99) == opendj::Mixer::CrossfaderAssign::thru);
}

TEST_CASE ("all four channels reach the master at once", "[mixer]")
{
    opendj::Mixer mixer;
    mixer.prepare (sampleRate, blockSize);
    mixer.setMasterGain (1.0f);
    mixer.setCrossfaderCurve (opendj::Mixer::CrossfaderCurve::linear);
    mixer.setCrossfaderPosition (0.0f);

    std::array<juce::AudioBuffer<float>, opendj::Mixer::numChannels> sources;
    std::array<juce::AudioBuffer<float>*, opendj::Mixer::numChannels> decks {};

    for (size_t c = 0; c < sources.size(); ++c)
    {
        sources[c].setSize (2, blockSize);
        decks[c] = &sources[c];
        mixer.setChannelFader ((int) c, 1.0f);

        // Off the crossfader, so this measures four channels and not the curve.
        mixer.setChannelCrossfaderAssign ((int) c, opendj::Mixer::CrossfaderAssign::thru);
    }

    juce::AudioBuffer<float> master (2, blockSize), cue (2, blockSize);
    std::array<double, opendj::Mixer::numChannels> phases {};
    auto peak = 0.0f;

    for (int block = 0; block <= blocksToSettle; ++block)
    {
        for (size_t c = 0; c < sources.size(); ++c)
            fillSine (sources[c], 1000.0, phases[c], 0.2f);

        mixer.processBlock (decks, master, cue);
        peak = master.getMagnitude (0, 0, blockSize);
    }

    // Four identical tones at 0.2 sum to 0.8, which the soft clipper leaves
    // alone. Two channels working and two ignored would read half of it.
    REQUIRE_THAT (peak, WithinAbs (0.8f, 0.03f));
}

//==============================================================================
// Which audio backend gets reached for first. Pinned down because a build that
// quietly went back to preferring DirectSound would sound broken on every
// Windows machine and no test would fail.

TEST_CASE ("the low latency backend is preferred on Windows", "[engine][device]")
{
    using opendj::AudioEngine;

    const auto lowLatency = AudioEngine::preferenceForDeviceType ("Windows Audio (Low Latency Mode)");
    const auto shared = AudioEngine::preferenceForDeviceType ("Windows Audio");
    const auto exclusive = AudioEngine::preferenceForDeviceType ("Windows Audio (Exclusive Mode)");
    const auto directSound = AudioEngine::preferenceForDeviceType ("DirectSound");

    // Lower is better, so every one of these is "beats".
    REQUIRE (lowLatency < shared);
    REQUIRE (lowLatency < exclusive);
    REQUIRE (lowLatency < directSound);
    REQUIRE (shared < directSound);
    REQUIRE (exclusive < directSound);
}

TEST_CASE ("DirectSound is the last resort", "[engine][device]")
{
    using opendj::AudioEngine;

    const auto directSound = AudioEngine::preferenceForDeviceType ("DirectSound");

    for (const auto* other : { "ASIO", "JACK", "CoreAudio", "ALSA",
                               "Windows Audio", "Windows Audio (Low Latency Mode)",
                               "something nobody has heard of" })
    {
        INFO (other);
        REQUIRE (AudioEngine::preferenceForDeviceType (other) < directSound);
    }
}

TEST_CASE ("a driver written for the job outranks everything", "[engine][device]")
{
    using opendj::AudioEngine;

    const auto lowLatency = AudioEngine::preferenceForDeviceType ("Windows Audio (Low Latency Mode)");

    for (const auto* dedicated : { "ASIO", "JACK", "CoreAudio" })
    {
        INFO (dedicated);
        REQUIRE (AudioEngine::preferenceForDeviceType (dedicated) <= lowLatency);
    }

    // An unknown backend sits between the ones worth having and DirectSound,
    // so a platform nobody has thought about here still gets tried.
    const auto unknown = AudioEngine::preferenceForDeviceType ("Some Future Backend");
    REQUIRE (unknown > lowLatency);
    REQUIRE (unknown < AudioEngine::preferenceForDeviceType ("DirectSound"));
}
