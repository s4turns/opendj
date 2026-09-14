/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/MicInput.h"

#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;
using opendj::MicInput;

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 480;   // ten milliseconds

    /** The music and the voice at steady levels, so a measured level says what
        the mic did to them rather than where a waveform happened to be. */
    struct Harness
    {
        Harness() { mic.prepare (sampleRate); }

        /** One block. Answers whether a separate recording mix was built. */
        bool run (float music, float voice)
        {
            fill (music);
            input.assign ((size_t) blockSize, voice);

            const float* channels[] = { input.data() };
            return mic.processBlock (channels, 1, room, recording, blockSize);
        }

        /** As many blocks as it takes to cover `seconds`, answering the last. */
        bool runFor (double seconds, float music, float voice)
        {
            auto separate = false;
            const auto blocks = (int) std::ceil (seconds * sampleRate / blockSize);

            for (int b = 0; b < blocks; ++b)
                separate = run (music, voice);

            return separate;
        }

        void fill (float music)
        {
            recording.clear();

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockSize; ++i)
                    room.setSample (ch, i, music);
        }

        float roomAt (int i) const   { return room.getSample (0, i); }
        float lastRoom() const       { return room.getSample (0, blockSize - 1); }
        float lastRecording() const  { return recording.getSample (0, blockSize - 1); }

        MicInput mic;
        juce::AudioBuffer<float> room { 2, blockSize };
        juce::AudioBuffer<float> recording { 2, blockSize };
        std::vector<float> input = std::vector<float> ((size_t) blockSize, 0.0f);
    };
}

TEST_CASE ("a mic that is off adds nothing, but its meter still reads", "[mic]")
{
    Harness h;

    REQUIRE (! h.runFor (0.1, 0.2f, 0.3f));
    REQUIRE (h.lastRoom() == 0.2f);

    // A level can be set before anybody hears it.
    REQUIRE_THAT (h.mic.getAndResetPeak(), WithinAbs (0.3f, 1.0e-4f));
    REQUIRE (h.mic.getAndResetPeak() == 0.0f);
}

TEST_CASE ("a mic that is on reaches the room at its level", "[mic]")
{
    Harness h;
    h.mic.setEnabled (true);

    REQUIRE (! h.runFor (0.1, 0.2f, 0.3f));
    REQUIRE_THAT (h.lastRoom(), WithinAbs (0.5f, 1.0e-4f));
}

TEST_CASE ("a mic kept out of the speakers still reaches the recording", "[mic]")
{
    Harness h;
    h.mic.setEnabled (true);
    h.mic.setRouting (MicInput::Routing::recordingOnly);

    REQUIRE (h.runFor (0.1, 0.2f, 0.3f));
    REQUIRE (h.lastRoom() == 0.2f);
    REQUIRE_THAT (h.lastRecording(), WithinAbs (0.5f, 1.0e-4f));
}

TEST_CASE ("switching the mic on fades it in rather than stepping", "[mic]")
{
    Harness h;
    h.mic.setEnabled (true);

    h.run (0.0f, 0.5f);

    // Ten milliseconds from nothing to full, so the first sample is nearly
    // silent and the last of the block is all there.
    REQUIRE (std::abs (h.roomAt (0)) < 0.01f);
    REQUIRE (h.roomAt (blockSize / 2) > 0.2f);
    REQUIRE (h.roomAt (blockSize / 2) < 0.3f);
    REQUIRE_THAT (h.lastRoom(), WithinAbs (0.5f, 1.0e-3f));
}

TEST_CASE ("talkover drops the music by ten decibels and gives it back", "[mic]")
{
    Harness h;
    h.mic.setEnabled (true);
    h.mic.setTalkover (true);

    const auto ducked = 0.5f * juce::Decibels::decibelsToGain (MicInput::talkoverDecibels);

    h.runFor (0.2, 0.5f, 0.0f);
    REQUIRE_THAT (h.lastRoom(), WithinAbs (ducked, 1.0e-3f));

    // The music comes back when the mic goes off, over half a second.
    h.mic.setEnabled (false);

    h.runFor (0.1, 0.5f, 0.0f);
    REQUIRE (h.lastRoom() > ducked + 0.01f);
    REQUIRE (h.lastRoom() < 0.49f);

    h.runFor (1.0, 0.5f, 0.0f);
    REQUIRE (h.lastRoom() == 0.5f);
}

TEST_CASE ("talkover does nothing while the mic is off", "[mic]")
{
    Harness h;
    h.mic.setTalkover (true);

    h.runFor (0.2, 0.5f, 0.0f);
    REQUIRE (h.lastRoom() == 0.5f);
}

TEST_CASE ("the level reaches the voice, smoothed", "[mic]")
{
    Harness h;
    h.mic.setEnabled (true);
    h.mic.setGain (2.0f);

    h.runFor (0.1, 0.0f, 0.25f);
    REQUIRE_THAT (h.lastRoom(), WithinAbs (0.5f, 1.0e-4f));

    // Out of range is held to the range.
    h.mic.setGain (5.0f);
    REQUIRE (h.mic.getGain() == 2.0f);
}

TEST_CASE ("a shout on top of loud music is rounded off, not torn", "[mic]")
{
    Harness h;
    h.mic.setEnabled (true);

    h.runFor (0.1, 0.8f, 0.8f);

    REQUIRE (h.lastRoom() > 0.95f);
    REQUIRE (h.lastRoom() <= 1.0f);
}

TEST_CASE ("no input is silence, not a crash", "[mic]")
{
    Harness h;
    h.mic.setEnabled (true);

    h.fill (0.3f);
    REQUIRE (! h.mic.processBlock (nullptr, 0, h.room, h.recording, blockSize));
    REQUIRE (h.lastRoom() == 0.3f);

    const float* nothing[] = { nullptr, nullptr };
    h.fill (0.3f);
    REQUIRE (! h.mic.processBlock (nothing, 2, h.room, h.recording, blockSize));
    REQUIRE (h.lastRoom() == 0.3f);
}

TEST_CASE ("a mic on either half of an input pair arrives at full level", "[mic]")
{
    Harness h;
    h.mic.setEnabled (true);

    std::vector<float> silence ((size_t) blockSize, 0.0f);
    std::vector<float> voice ((size_t) blockSize, 0.3f);

    for (int b = 0; b < 10; ++b)
    {
        h.fill (0.0f);
        const float* secondHalf[] = { silence.data(), voice.data() };
        h.mic.processBlock (secondHalf, 2, h.room, h.recording, blockSize);
    }

    REQUIRE_THAT (h.lastRoom(), WithinAbs (0.3f, 1.0e-4f));
}
