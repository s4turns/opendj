/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Sampler.h"

#include <cmath>

using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    /** A sound of a fixed level, so a measured level says what happened to it
        rather than what the waveform was doing at that moment. */
    std::unique_ptr<opendj::DecodedAudio> makeSound (double seconds, float level = 1.0f,
                                                     double rate = sampleRate)
    {
        auto sound = std::make_unique<opendj::DecodedAudio>();
        sound->sampleRate = rate;
        sound->audio.setSize (2, (int) std::lround (seconds * rate));

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < sound->audio.getNumSamples(); ++i)
                sound->audio.setSample (ch, i, level);

        return sound;
    }

    struct Harness
    {
        Harness() { sampler.prepare (sampleRate); }

        void load (int slot, double seconds, float level = 1.0f, double rate = sampleRate)
        {
            juce::String reason;
            REQUIRE (sampler.installSlot (slot, makeSound (seconds, level, rate), "test", &reason));
            REQUIRE (reason.isEmpty());
        }

        /** Runs one block and answers the loudest thing that reached master. */
        float runBlock (int numSamples = blockSize)
        {
            master.setSize (2, numSamples, false, false, true);
            cue.setSize (2, numSamples, false, false, true);
            master.clear();
            cue.clear();

            sampler.processBlock (master, cue, numSamples);

            return master.getMagnitude (0, numSamples);
        }

        float cueLevel() const { return cue.getMagnitude (0, cue.getNumSamples()); }

        float channelLevel (int channel) const
        {
            return master.getMagnitude (channel, 0, master.getNumSamples());
        }

        /** Runs blocks until one of them is loud, and answers how many that
            took. Somewhere for a test to say "and then it started". */
        int blocksUntilSound (int limit)
        {
            for (int i = 0; i < limit; ++i)
                if (runBlock() > 0.01f)
                    return i;

            return -1;
        }

        opendj::Sampler sampler;
        juce::AudioBuffer<float> master { 2, blockSize };
        juce::AudioBuffer<float> cue { 2, blockSize };
    };
}

TEST_CASE ("An empty sampler is silent", "[sampler]")
{
    Harness h;

    REQUIRE (! h.sampler.isSlotLoaded (0));
    REQUIRE (h.runBlock() == 0.0f);
}

TEST_CASE ("A loaded slot is silent until it is triggered", "[sampler]")
{
    Harness h;
    h.load (0, 0.5);

    REQUIRE (h.sampler.isSlotLoaded (0));
    REQUIRE (! h.sampler.isSlotPlaying (0));
    REQUIRE (h.runBlock() == 0.0f);

    h.sampler.trigger (0);
    REQUIRE (h.runBlock() > 0.5f);
    REQUIRE (h.sampler.isSlotPlaying (0));
}

TEST_CASE ("A one shot stops itself at the end of the sound", "[sampler]")
{
    Harness h;
    h.load (0, 0.05);                       // about five blocks at 512
    h.sampler.trigger (0);

    for (int i = 0; i < 20; ++i)
        h.runBlock();

    REQUIRE (! h.sampler.isSlotPlaying (0));
    REQUIRE (h.runBlock() == 0.0f);
}

TEST_CASE ("A looping slot keeps going past the end", "[sampler]")
{
    Harness h;
    h.load (0, 0.05);
    h.sampler.setSlotLooping (0, true);
    h.sampler.trigger (0);

    for (int i = 0; i < 40; ++i)
        h.runBlock();

    REQUIRE (h.sampler.isSlotPlaying (0));
    REQUIRE (h.runBlock() > 0.5f);

    h.sampler.stop (0);

    for (int i = 0; i < 4; ++i)
        h.runBlock();

    REQUIRE (! h.sampler.isSlotPlaying (0));
}

TEST_CASE ("A trigger starts the sound again from the beginning", "[sampler]")
{
    Harness h;
    h.load (0, 2.0);
    h.sampler.trigger (0);

    for (int i = 0; i < 10; ++i)
        h.runBlock();

    const auto before = h.sampler.getSlotPositionSeconds (0);
    REQUIRE (before > 0.05);

    h.sampler.trigger (0);
    h.runBlock();

    // Back near the start, rather than carrying on from where it was.
    REQUIRE (h.sampler.getSlotPositionSeconds (0) < before);
}

TEST_CASE ("Stopping a slot fades rather than cutting", "[sampler]")
{
    Harness h;
    h.load (0, 2.0);
    h.sampler.trigger (0);
    h.runBlock();

    h.sampler.stop (0);

    // The fade is four milliseconds, so the block it lands in still carries
    // audio. A cut would leave this one silent and a step in the output.
    REQUIRE (h.runBlock() > 0.01f);
}

TEST_CASE ("The slot gain and the sampler gain both scale the output", "[sampler]")
{
    Harness h;
    h.load (0, 2.0, 0.8f);
    h.sampler.setGain (1.0f);
    h.sampler.setSlotGain (0, 1.0f);
    h.sampler.trigger (0);

    for (int i = 0; i < 3; ++i)
        h.runBlock();

    const auto full = h.runBlock();
    REQUIRE_THAT (full, WithinAbs (0.8f, 0.02f));

    h.sampler.setSlotGain (0, 0.5f);
    REQUIRE_THAT (h.runBlock(), WithinAbs (full * 0.5f, 0.02f));

    h.sampler.setSlotGain (0, 1.0f);
    h.sampler.setGain (0.25f);
    REQUIRE_THAT (h.runBlock(), WithinAbs (full * 0.25f, 0.02f));
}

TEST_CASE ("The sampler reaches the headphones only when cueing is on", "[sampler]")
{
    Harness h;
    h.load (0, 2.0);
    h.sampler.trigger (0);

    for (int i = 0; i < 3; ++i)
        h.runBlock();

    REQUIRE (h.cueLevel() == 0.0f);

    h.sampler.setCueEnabled (true);
    REQUIRE (h.runBlock() > 0.5f);
    REQUIRE (h.cueLevel() > 0.5f);
}

TEST_CASE ("Two slots play at once", "[sampler]")
{
    Harness h;
    h.load (0, 2.0, 0.4f);
    h.load (1, 2.0, 0.4f);
    h.sampler.setGain (1.0f);

    h.sampler.trigger (0);

    for (int i = 0; i < 3; ++i)
        h.runBlock();

    const auto one = h.runBlock();

    h.sampler.trigger (1);

    for (int i = 0; i < 3; ++i)
        h.runBlock();

    REQUIRE (h.runBlock() > one * 1.5f);
}

TEST_CASE ("Clearing a slot stops it and empties it", "[sampler]")
{
    Harness h;
    h.load (0, 2.0);
    h.sampler.trigger (0);
    h.runBlock();

    h.sampler.clearSlot (0);

    REQUIRE (! h.sampler.isSlotLoaded (0));
    REQUIRE (h.runBlock() == 0.0f);

    // The displaced sound is held until the audio thread has moved past it, so
    // this is the sweep that actually frees it.
    h.runBlock();
    h.sampler.cleanUp();
}

TEST_CASE ("A sound at a different rate still plays for its own length", "[sampler]")
{
    Harness h;
    h.load (0, 1.0, 1.0f, 44100.0);         // one second at 44.1 out of a 48k device
    h.sampler.trigger (0);

    auto blocks = 0;

    while (h.sampler.isSlotPlaying (0) && blocks < 200)
    {
        h.runBlock();
        ++blocks;
    }

    // A second of audio is 48000 device samples, which is just under 94 blocks.
    // Playing it at the wrong rate would land near 86 or near 102.
    REQUIRE (blocks > 90);
    REQUIRE (blocks < 98);
}

TEST_CASE ("A slot that was never loaded refuses to be triggered", "[sampler]")
{
    Harness h;

    h.sampler.trigger (0);
    REQUIRE (! h.sampler.isSlotPlaying (0));
    REQUIRE (h.runBlock() == 0.0f);
}

TEST_CASE ("A slot number outside the sampler is answered safely", "[sampler]")
{
    Harness h;

    h.sampler.trigger (-1);
    h.sampler.trigger (opendj::Sampler::numSlots);
    h.sampler.stop (99);
    h.sampler.clearSlot (99);
    h.sampler.setSlotGain (99, 1.0f);

    REQUIRE (! h.sampler.isSlotLoaded (99));
    REQUIRE (! h.sampler.isSlotPlaying (-1));
    REQUIRE (h.sampler.getSlotGain (99) == 0.0f);
    REQUIRE (h.sampler.getSlotLengthSeconds (99) == 0.0);
    REQUIRE (h.sampler.getSlotName (99).isEmpty());

    juce::String reason;
    REQUIRE (! h.sampler.installSlot (99, makeSound (0.1), "x", &reason));
    REQUIRE (reason.isNotEmpty());
}

TEST_CASE ("A file too long to be a sample is refused", "[sampler]")
{
    Harness h;
    juce::String reason;

    REQUIRE (! h.sampler.installSlot (0, makeSound (61.0, 0.01f), "long", &reason));
    REQUIRE (reason.contains ("deck"));
    REQUIRE (! h.sampler.isSlotLoaded (0));
}

TEST_CASE ("One slot does not bleed into the other channel", "[sampler]")
{
    Harness h;
    h.load (0, 2.0);
    h.sampler.trigger (0);

    for (int i = 0; i < 3; ++i)
        h.runBlock();

    // A mono sound is laid across both, which is the point of the check: both
    // channels carry it, and neither carries twice as much as the other.
    REQUIRE_THAT (h.channelLevel (0), WithinAbs (h.channelLevel (1), 0.001f));
}

TEST_CASE ("The sampler adds to what the mixer already put in the buffer", "[sampler]")
{
    Harness h;
    h.load (0, 2.0, 0.25f);
    h.sampler.setGain (1.0f);
    h.sampler.trigger (0);

    for (int i = 0; i < 3; ++i)
        h.runBlock();

    // The mix is already there when the sampler runs, so this has to add to it
    // rather than replace it.
    h.master.clear();
    h.cue.clear();

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < blockSize; ++i)
            h.master.setSample (ch, i, 0.5f);

    h.sampler.processBlock (h.master, h.cue, blockSize);

    REQUIRE (h.master.getMagnitude (0, blockSize) > 0.7f);
}
