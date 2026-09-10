/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Deck.h"

#include <cmath>
#include <memory>

using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double deviceSampleRate = 48000.0;
    constexpr int blockSize = 512;
    constexpr double fixtureSeconds = 8.0;

    /** A deck plus a real decoded file on disk, since Deck deliberately goes
        through the format readers rather than accepting raw buffers. */
    struct Fixture
    {
        Fixture()
        {
            formatManager.registerBasicFormats();
            writeFixtureFile();

            deck = std::make_unique<opendj::Deck> (0, formatManager);
            deck->prepare (deviceSampleRate, blockSize);

            REQUIRE (deck->loadFile (temporaryFile.getFile()));

            buffer.setSize (2, blockSize);
        }

        void writeFixtureFile()
        {
            const double fileSampleRate = 44100.0;
            const auto numSamples = static_cast<int> (fixtureSeconds * fileSampleRate);

            juce::AudioBuffer<float> content (2, numSamples);
            double phase = 0.0;
            const auto delta = juce::MathConstants<double>::twoPi * 440.0 / fileSampleRate;

            for (int i = 0; i < numSamples; ++i)
            {
                const auto value = static_cast<float> (std::sin (phase)) * 0.5f;
                phase += delta;
                content.setSample (0, i, value);
                content.setSample (1, i, value);
            }

            juce::WavAudioFormat wav;
            auto stream = std::make_unique<juce::FileOutputStream> (temporaryFile.getFile());
            REQUIRE (stream->openedOk());

            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (stream.release(), fileSampleRate, 2, 16, {}, 0));
            REQUIRE (writer != nullptr);
            REQUIRE (writer->writeFromAudioSampleBuffer (content, 0, numSamples));
        }

        /** Advances the deck by the given number of blocks. */
        void run (int numBlocks)
        {
            for (int i = 0; i < numBlocks; ++i)
                deck->processBlock (buffer);
        }

        double secondsPerBlock() const { return blockSize / deviceSampleRate; }

        juce::AudioFormatManager formatManager;
        juce::TemporaryFile temporaryFile { ".wav" };
        std::unique_ptr<opendj::Deck> deck;
        juce::AudioBuffer<float> buffer;
    };
}

TEST_CASE ("a loaded deck reports its length and starts at zero", "[deck]")
{
    Fixture fixture;

    REQUIRE (fixture.deck->isLoaded());
    REQUIRE_THAT (fixture.deck->getLengthSeconds(), WithinAbs (fixtureSeconds, 0.01));
    REQUIRE_THAT (fixture.deck->getPositionSeconds(), WithinAbs (0.0, 0.001));
    REQUIRE_FALSE (fixture.deck->isPlaying());
}

TEST_CASE ("a stopped deck holds position and emits silence", "[deck]")
{
    Fixture fixture;
    fixture.run (10);

    REQUIRE_THAT (fixture.deck->getPositionSeconds(), WithinAbs (0.0, 0.001));
    REQUIRE (fixture.buffer.getMagnitude (0, 0, blockSize) < 0.0001f);
}

TEST_CASE ("playing advances position in real time", "[deck][transport]")
{
    Fixture fixture;
    fixture.deck->play();

    const int blocks = 100;
    fixture.run (blocks);

    REQUIRE_THAT (fixture.deck->getPositionSeconds(),
                  WithinAbs (blocks * fixture.secondsPerBlock(), 0.005));
    REQUIRE (fixture.buffer.getMagnitude (0, 0, blockSize) > 0.4f);
}

TEST_CASE ("tempo scales how fast the track is consumed", "[deck][tempo]")
{
    Fixture fixture;
    fixture.deck->setTempoRatio (1.08);   // the top of an 8 percent fader
    fixture.deck->play();

    const int blocks = 100;
    fixture.run (blocks);

    REQUIRE_THAT (fixture.deck->getPositionSeconds(),
                  WithinAbs (blocks * fixture.secondsPerBlock() * 1.08, 0.005));
}

TEST_CASE ("play and pause do not click", "[deck][transport]")
{
    Fixture fixture;
    fixture.deck->play();
    fixture.run (50);

    fixture.deck->pause();
    fixture.deck->processBlock (fixture.buffer);

    // The transport gain ramps over 5 ms, so the first paused block must decay
    // rather than jump to zero between one sample and the next.
    float largestStep = 0.0f;

    for (int i = 1; i < blockSize; ++i)
        largestStep = juce::jmax (largestStep,
                                  std::abs (fixture.buffer.getSample (0, i)
                                            - fixture.buffer.getSample (0, i - 1)));

    // A 440 Hz sine at 0.5 amplitude steps by about 0.03 between samples; a
    // click would be an order of magnitude larger than that.
    REQUIRE (largestStep < 0.1f);
}

TEST_CASE ("cue sets a point, previews from it, and returns on release", "[deck][cue]")
{
    Fixture fixture;

    // Park the deck a few seconds in, stopped.
    fixture.deck->seekToSeconds (3.0);
    fixture.run (1);
    REQUIRE_THAT (fixture.deck->getPositionSeconds(), WithinAbs (3.0, 0.01));

    // Pressing cue while stopped drops the cue point here and previews.
    fixture.deck->cuePressed();
    REQUIRE_THAT (fixture.deck->getCueSeconds(), WithinAbs (3.0, 0.01));
    REQUIRE (fixture.deck->isPlaying());

    fixture.run (20);
    REQUIRE (fixture.deck->getPositionSeconds() > 3.1);

    // Releasing it snaps back to the cue point and stops. The return happens
    // once the fade-out has finished, so give it a few blocks.
    fixture.deck->cueReleased();
    fixture.run (3);

    REQUIRE_FALSE (fixture.deck->isPlaying());
    REQUIRE_THAT (fixture.deck->getPositionSeconds(), WithinAbs (3.0, 0.01));
}

TEST_CASE ("cue while playing drops back to the cue point and stops", "[deck][cue]")
{
    Fixture fixture;

    fixture.deck->seekToSeconds (2.0);
    fixture.run (1);
    fixture.deck->cuePressed();     // sets the cue point at 2.0
    fixture.deck->cueReleased();

    fixture.deck->play();
    fixture.run (100);
    REQUIRE (fixture.deck->getPositionSeconds() > 3.0);

    fixture.deck->cuePressed();
    fixture.run (3);

    REQUIRE_FALSE (fixture.deck->isPlaying());
    REQUIRE_THAT (fixture.deck->getPositionSeconds(), WithinAbs (2.0, 0.01));
}

TEST_CASE ("playback stops at the end rather than looping", "[deck][transport]")
{
    Fixture fixture;

    fixture.deck->seekToSeconds (fixtureSeconds - 0.05);
    fixture.deck->play();
    fixture.run (20);

    REQUIRE_FALSE (fixture.deck->isPlaying());
    REQUIRE_THAT (fixture.deck->getPositionSeconds(), WithinAbs (fixtureSeconds, 0.01));
}

TEST_CASE ("retired tracks are only freed once the audio thread has moved on", "[deck][lifetime]")
{
    Fixture fixture;

    fixture.deck->play();
    fixture.run (5);

    // Loading again retires the first track while the deck is live.
    REQUIRE (fixture.deck->loadFile (fixture.temporaryFile.getFile()));
    fixture.deck->cleanUp();

    fixture.run (5);
    fixture.deck->cleanUp();

    REQUIRE (fixture.deck->isLoaded());
    REQUIRE_FALSE (fixture.deck->isPlaying());   // loading always stops the deck
}
