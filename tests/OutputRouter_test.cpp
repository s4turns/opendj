/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/OutputRouter.h"

#include <array>
#include <vector>

using Catch::Matchers::WithinAbs;
using opendj::OutputMode;

namespace
{
    constexpr int numSamples = 64;
    constexpr float canary = -9999.0f;

    /** Two busses with a different, constant value in every channel, so which
        signal landed where is a matter of reading one number. */
    struct Busses
    {
        Busses()
        {
            master.setSize (2, numSamples);
            cue.setSize (2, numSamples);

            for (int i = 0; i < numSamples; ++i)
            {
                master.setSample (0, i, 0.10f);
                master.setSample (1, i, 0.20f);
                cue.setSample (0, i, 0.30f);
                cue.setSample (1, i, 0.40f);
            }
        }

        juce::AudioBuffer<float> master, cue;
    };

    /** Device outputs, longer than the block, filled with a canary so anything
        written past the end is visible. */
    struct Outputs
    {
        explicit Outputs (int channels) : storage ((size_t) channels)
        {
            for (auto& channel : storage)
                channel.assign ((size_t) numSamples * 2, canary);

            for (auto& channel : storage)
                pointers.push_back (channel.data());
        }

        float at (int channel, int sample = 0) const
        {
            return storage[(size_t) channel][(size_t) sample];
        }

        /** True when nothing was written after the block that was asked for. */
        bool tailIsIntact() const
        {
            for (const auto& channel : storage)
                for (size_t i = (size_t) numSamples; i < channel.size(); ++i)
                    if (channel[i] != canary)
                        return false;

            return true;
        }

        float* const* data() { return pointers.data(); }
        int size() const { return (int) pointers.size(); }

        std::vector<std::vector<float>> storage;
        std::vector<float*> pointers;
    };
}

TEST_CASE ("separate pairs put master on 1-2 and cue on 3-4", "[routing]")
{
    Busses busses;
    Outputs outputs (4);

    routeOutputs (busses.master, busses.cue, outputs.data(), outputs.size(),
                  numSamples, OutputMode::separatePairs);

    REQUIRE_THAT (outputs.at (0), WithinAbs (0.10f, 1.0e-6f));
    REQUIRE_THAT (outputs.at (1), WithinAbs (0.20f, 1.0e-6f));
    REQUIRE_THAT (outputs.at (2), WithinAbs (0.30f, 1.0e-6f));
    REQUIRE_THAT (outputs.at (3), WithinAbs (0.40f, 1.0e-6f));
    REQUIRE (outputs.tailIsIntact());
}

TEST_CASE ("separate pairs on a stereo device drop the cue bus", "[routing]")
{
    Busses busses;
    Outputs outputs (2);

    routeOutputs (busses.master, busses.cue, outputs.data(), outputs.size(),
                  numSamples, OutputMode::separatePairs);

    // Master, in stereo, and the headphone feed nowhere near the room.
    REQUIRE_THAT (outputs.at (0), WithinAbs (0.10f, 1.0e-6f));
    REQUIRE_THAT (outputs.at (1), WithinAbs (0.20f, 1.0e-6f));
    REQUIRE (outputs.tailIsIntact());
}

TEST_CASE ("a split puts master on one output and cue on the other", "[routing]")
{
    Busses busses;
    Outputs outputs (2);

    routeOutputs (busses.master, busses.cue, outputs.data(), outputs.size(),
                  numSamples, OutputMode::splitStereo);

    // Each bus folded to mono: the average of its two sides, not their sum, so
    // a loud stereo mix does not clip on the way down.
    REQUIRE_THAT (outputs.at (0), WithinAbs (0.15f, 1.0e-6f));
    REQUIRE_THAT (outputs.at (1), WithinAbs (0.35f, 1.0e-6f));
    REQUIRE (outputs.tailIsIntact());
}

TEST_CASE ("a split uses only the first two outputs", "[routing]")
{
    Busses busses;
    Outputs outputs (4);

    routeOutputs (busses.master, busses.cue, outputs.data(), outputs.size(),
                  numSamples, OutputMode::splitStereo);

    REQUIRE_THAT (outputs.at (0), WithinAbs (0.15f, 1.0e-6f));
    REQUIRE_THAT (outputs.at (1), WithinAbs (0.35f, 1.0e-6f));

    // A split is a statement about a cable. The other outputs are left alone
    // rather than quietly carrying a second copy of something.
    REQUIRE (outputs.at (2) == canary);
    REQUIRE (outputs.at (3) == canary);
}

TEST_CASE ("a single output gets the master and nothing else", "[routing]")
{
    Busses busses;

    for (const auto mode : { OutputMode::separatePairs, OutputMode::splitStereo })
    {
        Outputs outputs (1);
        routeOutputs (busses.master, busses.cue, outputs.data(), outputs.size(),
                      numSamples, mode);

        // Separate pairs copy the left channel; a split folds to mono. Either
        // way the cue bus is not on it.
        const auto expected = mode == OutputMode::splitStereo ? 0.15f : 0.10f;
        REQUIRE_THAT (outputs.at (0), WithinAbs (expected, 1.0e-6f));
        REQUIRE (outputs.tailIsIntact());
    }
}

TEST_CASE ("an inactive output channel is skipped", "[routing]")
{
    Busses busses;
    Outputs outputs (4);

    // JUCE passes null for an output the device is not using.
    outputs.pointers[1] = nullptr;
    outputs.pointers[3] = nullptr;

    routeOutputs (busses.master, busses.cue, outputs.data(), outputs.size(),
                  numSamples, OutputMode::separatePairs);

    REQUIRE_THAT (outputs.at (0), WithinAbs (0.10f, 1.0e-6f));
    REQUIRE_THAT (outputs.at (2), WithinAbs (0.30f, 1.0e-6f));
}

TEST_CASE ("nothing is written for an empty block or no outputs", "[routing]")
{
    Busses busses;
    Outputs outputs (4);

    routeOutputs (busses.master, busses.cue, outputs.data(), outputs.size(),
                  0, OutputMode::separatePairs);
    routeOutputs (busses.master, busses.cue, outputs.data(), 0,
                  numSamples, OutputMode::separatePairs);
    routeOutputs (busses.master, busses.cue, nullptr, 4,
                  numSamples, OutputMode::separatePairs);

    for (int ch = 0; ch < 4; ++ch)
        REQUIRE (outputs.at (ch) == canary);
}

TEST_CASE ("a block longer than the busses is trimmed to fit", "[routing]")
{
    Busses busses;
    Outputs outputs (4);

    // Asked for more than the busses hold, which must trim rather than read
    // past the end of them.
    routeOutputs (busses.master, busses.cue, outputs.data(), outputs.size(),
                  numSamples * 2, OutputMode::separatePairs);

    REQUIRE_THAT (outputs.at (0), WithinAbs (0.10f, 1.0e-6f));
    REQUIRE (outputs.tailIsIntact());
}

TEST_CASE ("whether the cue bus can be heard depends on the mode", "[routing]")
{
    REQUIRE (cueIsAudible (OutputMode::separatePairs, 4));
    REQUIRE (! cueIsAudible (OutputMode::separatePairs, 2));
    REQUIRE (! cueIsAudible (OutputMode::separatePairs, 0));

    // The whole point of a split: a plain stereo interface can pre-listen.
    REQUIRE (cueIsAudible (OutputMode::splitStereo, 2));
    REQUIRE (cueIsAudible (OutputMode::splitStereo, 4));
    REQUIRE (! cueIsAudible (OutputMode::splitStereo, 1));
}

TEST_CASE ("an output mode survives being written down", "[routing][settings]")
{
    for (const auto mode : { OutputMode::separatePairs, OutputMode::splitStereo })
        REQUIRE (opendj::outputModeFromString (toString (mode)) == mode);

    // Anything unrecognised reads as separate pairs, which never puts a mono
    // master into one speaker without being asked.
    REQUIRE (opendj::outputModeFromString ("") == OutputMode::separatePairs);
    REQUIRE (opendj::outputModeFromString ("nonsense") == OutputMode::separatePairs);
}
