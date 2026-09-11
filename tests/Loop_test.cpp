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
    constexpr double fixtureSeconds = 30.0;
    constexpr double fixtureBpm = 120.0;          // two beats a second, so a beat is 0.5s

    /** A deck holding a click track at a known tempo, so the beat grid the
        analyser finds is the one the loop lengths are measured against. */
    struct Fixture
    {
        Fixture()
        {
            formatManager.registerBasicFormats();
            writeFixture();

            deck = std::make_unique<opendj::Deck> (0, formatManager);
            deck->prepare (deviceSampleRate, blockSize);

            REQUIRE (deck->loadFile (temporaryFile.getFile()));
            REQUIRE (deck->getAnalysis() != nullptr);

            buffer.setSize (2, blockSize);
        }

        void writeFixture()
        {
            const double fileSampleRate = 44100.0;
            const auto numSamples = static_cast<int> (fixtureSeconds * fileSampleRate);
            const auto samplesPerBeat = 60.0 / fixtureBpm * fileSampleRate;

            juce::AudioBuffer<float> content (2, numSamples);
            content.clear();

            // fileSampleRate is used inside, so it has to be captured: MSVC lets
            // a const local through without one, GCC does not.
            const auto addHit = [&content, numSamples, fileSampleRate] (double at, double frequency,
                                                                        double decay, float amplitude)
            {
                const auto start = static_cast<int> (at);

                for (int i = 0; i < static_cast<int> (decay * 4.0); ++i)
                {
                    const auto index = start + i;

                    if (index < 0 || index >= numSamples)
                        break;

                    const auto envelope = std::exp (-i / decay);
                    const auto phase = juce::MathConstants<double>::twoPi * frequency * i / fileSampleRate;
                    const auto value = static_cast<float> (std::sin (phase) * envelope) * amplitude;

                    content.addSample (0, index, value);
                    content.addSample (1, index, value);
                }
            };

            for (double beat = 0.0; beat * samplesPerBeat < numSamples; beat += 1.0)
            {
                addHit (beat * samplesPerBeat, 55.0, fileSampleRate * 0.06, 0.8f);
                addHit ((beat + 0.5) * samplesPerBeat, 6000.0, fileSampleRate * 0.01, 0.3f);
            }

            juce::WavAudioFormat wav;
            auto stream = std::make_unique<juce::FileOutputStream> (temporaryFile.getFile());
            REQUIRE (stream->openedOk());

            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (stream.release(), fileSampleRate, 2, 16, {}, 0));
            REQUIRE (writer != nullptr);
            REQUIRE (writer->writeFromAudioSampleBuffer (content, 0, numSamples));
        }

        void run (int numBlocks)
        {
            for (int i = 0; i < numBlocks; ++i)
                deck->processBlock (buffer);
        }

        double secondsPerBlock() const { return blockSize / deviceSampleRate; }

        /** Blocks needed to cover a stretch of the track at normal speed. */
        int blocksFor (double seconds) const
        {
            return static_cast<int> (std::ceil (seconds / secondsPerBlock()));
        }

        juce::AudioFormatManager formatManager;
        juce::TemporaryFile temporaryFile { ".wav" };
        std::unique_ptr<opendj::Deck> deck;
        juce::AudioBuffer<float> buffer;
    };
}

//==============================================================================
// Setting a loop
//==============================================================================

TEST_CASE ("a deck starts with no loop", "[loop]")
{
    Fixture fixture;

    REQUIRE_FALSE (fixture.deck->hasLoop());
    REQUIRE_FALSE (fixture.deck->isLoopEnabled());
    REQUIRE_FALSE (fixture.deck->isLoopRolling());
}

TEST_CASE ("loop in and out set a loop by hand", "[loop]")
{
    Fixture fixture;

    fixture.deck->seekToSeconds (4.0);
    fixture.run (1);
    fixture.deck->setLoopIn();

    // Loop in alone must not start looping: the end is not known yet.
    REQUIRE_FALSE (fixture.deck->isLoopEnabled());

    fixture.deck->seekToSeconds (6.0);
    fixture.run (1);
    fixture.deck->setLoopOut();

    REQUIRE (fixture.deck->hasLoop());
    REQUIRE (fixture.deck->isLoopEnabled());
    REQUIRE_THAT (fixture.deck->getLoopStartSeconds(), WithinAbs (4.0, 0.02));
    REQUIRE_THAT (fixture.deck->getLoopEndSeconds(), WithinAbs (6.0, 0.02));

    // Set by hand, so it has no length in beats.
    REQUIRE (fixture.deck->getLoopBeats() == 0.0);
}

TEST_CASE ("a loop out before the loop in is refused", "[loop]")
{
    Fixture fixture;

    fixture.deck->seekToSeconds (8.0);
    fixture.run (1);
    fixture.deck->setLoopIn();

    fixture.deck->seekToSeconds (2.0);
    fixture.run (1);
    fixture.deck->setLoopOut();

    // A backwards loop is not a loop, and enabling it would trap the deck.
    REQUIRE_FALSE (fixture.deck->isLoopEnabled());
}

TEST_CASE ("an automatic loop is a whole number of beats long", "[loop][beats]")
{
    Fixture fixture;
    const auto analysis = fixture.deck->getAnalysis();

    REQUIRE (analysis->hasTempo());
    const auto beat = analysis->secondsPerBeat();

    fixture.deck->seekToSeconds (10.0);
    fixture.run (1);

    REQUIRE (fixture.deck->setLoopBeats (4.0));

    const auto length = fixture.deck->getLoopEndSeconds() - fixture.deck->getLoopStartSeconds();

    INFO ("beat is " << beat << "s, loop is " << length << "s");
    REQUIRE_THAT (length, WithinAbs (4.0 * beat, 0.01));
    REQUIRE (fixture.deck->getLoopBeats() == 4.0);
    REQUIRE (fixture.deck->isLoopEnabled());
}

TEST_CASE ("an automatic loop starts on the beat behind the playhead", "[loop][beats]")
{
    Fixture fixture;
    const auto analysis = fixture.deck->getAnalysis();
    const auto beat = analysis->secondsPerBeat();

    // Park a third of the way through a beat, which is what a finger does.
    const auto beatTime = analysis->nearestBeatSeconds (10.0);
    fixture.deck->seekToSeconds (beatTime + beat * 0.33);
    fixture.run (1);

    REQUIRE (fixture.deck->setLoopBeats (1.0));

    const auto start = fixture.deck->getLoopStartSeconds();

    // Behind, not nearest: a loop that starts late is late for every bar it
    // plays, which is the mistake the button exists to prevent.
    INFO ("beat at " << beatTime << ", loop starts at " << start);
    REQUIRE (start <= beatTime + 0.001);
    REQUIRE_THAT (start, WithinAbs (beatTime, 0.02));
}

TEST_CASE ("an automatic loop needs a beat grid", "[loop][beats]")
{
    // Silence gives the analyser no tempo, and a beat-length loop without one
    // would be a guess dressed up as a measurement.
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    juce::TemporaryFile file { ".wav" };
    {
        juce::AudioBuffer<float> silence (2, static_cast<int> (44100.0 * 5.0));
        silence.clear();

        juce::WavAudioFormat wav;
        auto stream = std::make_unique<juce::FileOutputStream> (file.getFile());
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (stream.release(), 44100.0, 2, 16, {}, 0));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (silence, 0, silence.getNumSamples());
    }

    opendj::Deck deck (0, formats);
    deck.prepare (deviceSampleRate, blockSize);
    REQUIRE (deck.loadFile (file.getFile()));

    REQUIRE_FALSE (deck.setLoopBeats (4.0));
    REQUIRE_FALSE (deck.isLoopEnabled());
}

//==============================================================================
// Playing a loop
//==============================================================================

TEST_CASE ("playback stays inside an enabled loop", "[loop][transport]")
{
    Fixture fixture;

    fixture.deck->seekToSeconds (10.0);
    fixture.run (1);
    REQUIRE (fixture.deck->setLoopBeats (2.0));

    const auto start = fixture.deck->getLoopStartSeconds();
    const auto end = fixture.deck->getLoopEndSeconds();

    fixture.deck->play();

    // Run for several times the length of the loop.
    for (int i = 0; i < fixture.blocksFor ((end - start) * 6.0); ++i)
    {
        fixture.run (1);
        const auto position = fixture.deck->getPositionSeconds();

        INFO ("position " << position << " should be within " << start << " to " << end);
        REQUIRE (position >= start - 0.05);
        REQUIRE (position <= end + 0.05);
    }

    REQUIRE (fixture.deck->isPlaying());
}

TEST_CASE ("a loop wraps rather than stopping at the end of it", "[loop][transport]")
{
    Fixture fixture;

    fixture.deck->seekToSeconds (10.0);
    fixture.run (1);
    REQUIRE (fixture.deck->setLoopBeats (1.0));

    const auto start = fixture.deck->getLoopStartSeconds();
    fixture.deck->seekToSeconds (fixture.deck->getLoopEndSeconds() - 0.01);
    fixture.deck->play();
    fixture.run (3);

    // It came back to near the start rather than carrying on past the end.
    REQUIRE_THAT (fixture.deck->getPositionSeconds(), WithinAbs (start, 0.1));
    REQUIRE (fixture.deck->isPlaying());
}

TEST_CASE ("disabling a loop lets the track run on", "[loop][transport]")
{
    Fixture fixture;

    fixture.deck->seekToSeconds (10.0);
    fixture.run (1);
    REQUIRE (fixture.deck->setLoopBeats (1.0));

    const auto end = fixture.deck->getLoopEndSeconds();

    fixture.deck->play();
    fixture.run (fixture.blocksFor (1.0));
    fixture.deck->setLoopEnabled (false);
    fixture.run (fixture.blocksFor (2.0));

    REQUIRE (fixture.deck->getPositionSeconds() > end + 0.5);

    // The loop is remembered even while it is off, so it can be turned back on.
    REQUIRE (fixture.deck->hasLoop());
}

TEST_CASE ("reloop jumps back and starts it again", "[loop][transport]")
{
    Fixture fixture;

    fixture.deck->seekToSeconds (10.0);
    fixture.run (1);
    REQUIRE (fixture.deck->setLoopBeats (2.0));

    const auto start = fixture.deck->getLoopStartSeconds();

    fixture.deck->setLoopEnabled (false);
    fixture.deck->play();
    fixture.run (fixture.blocksFor (3.0));
    REQUIRE (fixture.deck->getPositionSeconds() > start + 2.0);

    fixture.deck->reloop();
    fixture.run (2);

    REQUIRE (fixture.deck->isLoopEnabled());
    REQUIRE_THAT (fixture.deck->getPositionSeconds(), WithinAbs (start, 0.1));
}

TEST_CASE ("a loop keeps working with key lock on", "[loop][keylock]")
{
    // The stretcher reads through its own head. A loop that wrapped only the
    // audible one would show the deck looping while it played straight on.
    Fixture fixture;

    fixture.deck->seekToSeconds (10.0);
    fixture.run (1);
    REQUIRE (fixture.deck->setLoopBeats (2.0));

    const auto start = fixture.deck->getLoopStartSeconds();
    const auto end = fixture.deck->getLoopEndSeconds();

    fixture.deck->setKeyLock (true);
    fixture.deck->setTempoRatio (1.06);
    fixture.deck->play();
    fixture.run (fixture.blocksFor ((end - start) * 4.0));

    const auto position = fixture.deck->getPositionSeconds();

    INFO ("position " << position << " against loop " << start << " to " << end);
    REQUIRE (position >= start - 0.1);
    REQUIRE (position <= end + 0.1);
}

//==============================================================================
// Halving and doubling
//==============================================================================

TEST_CASE ("halving and doubling keep the loop start still", "[loop][beats]")
{
    Fixture fixture;

    fixture.deck->seekToSeconds (10.0);
    fixture.run (1);
    REQUIRE (fixture.deck->setLoopBeats (4.0));

    const auto start = fixture.deck->getLoopStartSeconds();
    const auto length = fixture.deck->getLoopEndSeconds() - start;

    fixture.deck->halveLoop();
    REQUIRE_THAT (fixture.deck->getLoopStartSeconds(), WithinAbs (start, 0.001));
    REQUIRE_THAT (fixture.deck->getLoopEndSeconds() - start, WithinAbs (length * 0.5, 0.001));
    REQUIRE (fixture.deck->getLoopBeats() == 2.0);

    fixture.deck->doubleLoop();
    fixture.deck->doubleLoop();
    REQUIRE_THAT (fixture.deck->getLoopStartSeconds(), WithinAbs (start, 0.001));
    REQUIRE_THAT (fixture.deck->getLoopEndSeconds() - start, WithinAbs (length * 2.0, 0.001));
    REQUIRE (fixture.deck->getLoopBeats() == 8.0);
}

TEST_CASE ("a loop will not be halved into a tone", "[loop][beats]")
{
    Fixture fixture;

    fixture.deck->seekToSeconds (10.0);
    fixture.run (1);
    REQUIRE (fixture.deck->setLoopBeats (1.0));

    // Twelve halvings would take half a second down to a tenth of a millisecond.
    for (int i = 0; i < 12; ++i)
        fixture.deck->halveLoop();

    const auto length = fixture.deck->getLoopEndSeconds() - fixture.deck->getLoopStartSeconds();

    INFO ("shortest loop reached: " << length << "s");
    REQUIRE (length >= 0.02);
}

//==============================================================================
// Rolls
//==============================================================================

TEST_CASE ("a roll returns to where the track would have been", "[loop][roll]")
{
    Fixture fixture;

    fixture.deck->seekToSeconds (10.0);
    fixture.run (1);
    fixture.deck->play();
    fixture.run (2);

    const auto before = fixture.deck->getPositionSeconds();

    REQUIRE (fixture.deck->beginLoopRoll (1.0));
    REQUIRE (fixture.deck->isLoopRolling());

    const auto rollBlocks = fixture.blocksFor (2.0);
    fixture.run (rollBlocks);

    // While rolling it is inside the loop, which is behind where it would be.
    REQUIRE (fixture.deck->getPositionSeconds() < before + 1.5);

    fixture.deck->endLoopRoll();
    fixture.run (1);

    // Letting go lands where the music got to, which is the whole point: a roll
    // can be dropped in mid-phrase without losing the mix.
    const auto expected = before + (rollBlocks + 1) * fixture.secondsPerBlock();

    INFO ("landed at " << fixture.deck->getPositionSeconds() << ", expected near " << expected);
    REQUIRE_THAT (fixture.deck->getPositionSeconds(), WithinAbs (expected, 0.1));

    // And the loop it made goes with it.
    REQUIRE_FALSE (fixture.deck->isLoopRolling());
    REQUIRE_FALSE (fixture.deck->isLoopEnabled());
}

TEST_CASE ("a roll repeats while it is held", "[loop][roll]")
{
    Fixture fixture;

    fixture.deck->seekToSeconds (10.0);
    fixture.run (1);
    fixture.deck->play();

    REQUIRE (fixture.deck->beginLoopRoll (1.0));

    const auto start = fixture.deck->getLoopStartSeconds();
    const auto end = fixture.deck->getLoopEndSeconds();

    for (int i = 0; i < fixture.blocksFor ((end - start) * 4.0); ++i)
    {
        fixture.run (1);
        REQUIRE (fixture.deck->getPositionSeconds() >= start - 0.05);
        REQUIRE (fixture.deck->getPositionSeconds() <= end + 0.05);
    }
}

TEST_CASE ("a second roll is not started on top of the first", "[loop][roll]")
{
    Fixture fixture;

    fixture.deck->seekToSeconds (10.0);
    fixture.run (1);
    fixture.deck->play();

    REQUIRE (fixture.deck->beginLoopRoll (1.0));
    const auto start = fixture.deck->getLoopStartSeconds();

    fixture.run (4);

    // Whatever the pads do, the return position must not be reset half way
    // through, or letting go would land in the wrong place.
    REQUIRE_FALSE (fixture.deck->beginLoopRoll (2.0));
    REQUIRE_THAT (fixture.deck->getLoopStartSeconds(), WithinAbs (start, 0.001));
}

TEST_CASE ("loading a track leaves no loop behind", "[loop]")
{
    Fixture fixture;

    fixture.deck->seekToSeconds (10.0);
    fixture.run (1);
    REQUIRE (fixture.deck->setLoopBeats (4.0));
    REQUIRE (fixture.deck->isLoopEnabled());

    REQUIRE (fixture.deck->loadFile (fixture.temporaryFile.getFile()));

    REQUIRE_FALSE (fixture.deck->hasLoop());
    REQUIRE_FALSE (fixture.deck->isLoopEnabled());
    REQUIRE_FALSE (fixture.deck->isLoopRolling());
}

TEST_CASE ("a loop length is sticky", "[loop][sticky]")
{
    // Pressing a length starts that loop and leaves it running; pressing the
    // same length again stops it. Every loop section on every piece of DJ gear
    // behaves this way, and a controller has no click-and-hold to tell apart.
    Fixture fixture;

    REQUIRE (fixture.deck->getAnalysis() != nullptr);
    REQUIRE (fixture.deck->getAnalysis()->hasTempo());

    REQUIRE (fixture.deck->toggleLoopBeats (4.0));
    REQUIRE (fixture.deck->isLoopEnabled());
    REQUIRE_THAT (fixture.deck->getLoopBeats(), WithinAbs (4.0, 0.001));

    // It survives being played through, rather than ending on its own.
    fixture.deck->play();
    fixture.run (50);
    REQUIRE (fixture.deck->isLoopEnabled());

    // The same length again turns it off.
    REQUIRE_FALSE (fixture.deck->toggleLoopBeats (4.0));
    REQUIRE_FALSE (fixture.deck->isLoopEnabled());
}

TEST_CASE ("another loop length changes the loop rather than stopping it", "[loop][sticky]")
{
    Fixture fixture;

    REQUIRE (fixture.deck->toggleLoopBeats (8.0));
    REQUIRE (fixture.deck->isLoopEnabled());

    // Reaching for another length mid-loop means change to it, not stop.
    REQUIRE (fixture.deck->toggleLoopBeats (2.0));
    REQUIRE (fixture.deck->isLoopEnabled());
    REQUIRE_THAT (fixture.deck->getLoopBeats(), WithinAbs (2.0, 0.001));

    // And that new length is now the one that stops it.
    REQUIRE_FALSE (fixture.deck->toggleLoopBeats (2.0));
    REQUIRE_FALSE (fixture.deck->isLoopEnabled());
}
