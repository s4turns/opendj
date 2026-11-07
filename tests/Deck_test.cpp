/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Deck.h"

#include <cmath>
#include <memory>
#include <vector>

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

        /** Advances the deck and keeps the left channel of everything it produced. */
        std::vector<float> capture (int numBlocks)
        {
            std::vector<float> samples;
            samples.reserve ((size_t) (numBlocks * blockSize));

            for (int i = 0; i < numBlocks; ++i)
            {
                deck->processBlock (buffer);
                const auto* left = buffer.getReadPointer (0);
                samples.insert (samples.end(), left, left + blockSize);
            }

            return samples;
        }

        double secondsPerBlock() const { return blockSize / deviceSampleRate; }

        juce::AudioFormatManager formatManager;
        juce::TemporaryFile temporaryFile { ".wav" };
        std::unique_ptr<opendj::Deck> deck;
        juce::AudioBuffer<float> buffer;
    };

    /** The frequency of a tone, from how often it crosses zero going up. Crude,
        but exact enough on a sine to tell 440 Hz from 484. */
    double measureFrequency (const std::vector<float>& samples, double sampleRate)
    {
        auto crossings = 0;

        for (size_t i = 1; i < samples.size(); ++i)
            if (samples[i - 1] < 0.0f && samples[i] >= 0.0f)
                ++crossings;

        return crossings / (static_cast<double> (samples.size()) / sampleRate);
    }
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

TEST_CASE ("a hand on the platter drives playback", "[deck][jog]")
{
    Fixture fixture;
    fixture.deck->setJogTicksPerRevolution (512);
    fixture.deck->seekToSeconds (2.0);
    fixture.run (1);

    // A record at 33 1/3 rpm turns once in 1.8 seconds, so half a turn of the
    // platter should move half of that. The head chases the hand over a couple
    // of blocks rather than teleporting, so let it arrive before measuring.
    fixture.deck->setJogTouched (true);
    fixture.deck->addJogTicks (256.0);
    fixture.run (40);

    REQUIRE_THAT (fixture.deck->getPositionSeconds(), WithinAbs (2.9, 0.01));
}

TEST_CASE ("the platter runs the track backwards", "[deck][jog]")
{
    Fixture fixture;
    fixture.deck->seekToSeconds (4.0);
    fixture.run (1);

    fixture.deck->setJogTouched (true);
    fixture.deck->addJogTicks (-256.0);
    fixture.run (40);

    REQUIRE_THAT (fixture.deck->getPositionSeconds(), WithinAbs (3.1, 0.01));
}

TEST_CASE ("a still hand on the platter holds the track still", "[deck][jog]")
{
    Fixture fixture;
    fixture.deck->seekToSeconds (2.0);
    fixture.deck->play();
    fixture.run (5);

    const auto beforeTouch = fixture.deck->getPositionSeconds();

    // Touching a playing deck stops it dead, the way a hand on vinyl does.
    fixture.deck->setJogTouched (true);
    fixture.run (10);

    REQUIRE_THAT (fixture.deck->getPositionSeconds(), WithinAbs (beforeTouch, 0.001));
}

TEST_CASE ("scratching a stopped deck is audible", "[deck][jog]")
{
    Fixture fixture;
    fixture.deck->seekToSeconds (2.0);
    fixture.run (2);

    REQUIRE (fixture.buffer.getMagnitude (0, 0, blockSize) < 0.0001f);

    // Keep the platter moving: a still hand is silence by design, so the audio
    // only appears while the ticks keep coming.
    fixture.deck->setJogTouched (true);

    auto loudest = 0.0f;

    for (int i = 0; i < 8; ++i)
    {
        fixture.deck->addJogTicks (64.0);
        fixture.run (1);
        loudest = juce::jmax (loudest, fixture.buffer.getMagnitude (0, 0, blockSize));
    }

    REQUIRE (loudest > 0.1f);
}

TEST_CASE ("letting go of the platter returns to the tempo fader", "[deck][jog]")
{
    Fixture fixture;
    fixture.deck->seekToSeconds (2.0);
    fixture.deck->play();
    fixture.run (2);

    fixture.deck->setJogTouched (true);
    fixture.run (5);

    fixture.deck->setJogTouched (false);
    const auto atRelease = fixture.deck->getPositionSeconds();

    const int blocks = 50;
    fixture.run (blocks);

    REQUIRE_THAT (fixture.deck->getPositionSeconds(),
                  WithinAbs (atRelease + blocks * fixture.secondsPerBlock(), 0.005));
}

TEST_CASE ("a nudge off the platter bends the pitch and then lets go", "[deck][jog]")
{
    Fixture fixture;
    fixture.deck->play();
    fixture.run (2);

    const auto beforeNudge = fixture.deck->getPositionSeconds();
    fixture.deck->addJogTicks (100.0);
    fixture.run (10);

    const auto nudged = fixture.deck->getPositionSeconds() - beforeNudge;
    const auto unnudged = 10 * fixture.secondsPerBlock();

    // The nudge pushes it ahead, but only by a little and only briefly.
    REQUIRE (nudged > unnudged);
    REQUIRE (nudged < unnudged * 1.1);

    // Give the bend time to decay, then confirm the deck is back at exactly the
    // rate the tempo fader asks for.
    fixture.run (100);
    const auto settled = fixture.deck->getPositionSeconds();
    fixture.run (50);

    REQUIRE_THAT (fixture.deck->getPositionSeconds(),
                  WithinAbs (settled + 50 * fixture.secondsPerBlock(), 0.002));
}

TEST_CASE ("a hot cue pad sets an empty slot and jumps to a set one", "[deck][hotcue]")
{
    Fixture fixture;

    REQUIRE_FALSE (fixture.deck->hasHotCue (0));

    fixture.deck->seekToSeconds (3.5);
    fixture.run (1);
    fixture.deck->hotCuePressed (0);

    REQUIRE (fixture.deck->hasHotCue (0));
    REQUIRE_THAT (fixture.deck->getHotCueSeconds (0), WithinAbs (3.5, 0.01));

    fixture.deck->seekToSeconds (6.0);
    fixture.run (1);

    fixture.deck->hotCuePressed (0);
    fixture.run (1);
    REQUIRE_THAT (fixture.deck->getPositionSeconds(), WithinAbs (3.5, 0.01));

    fixture.deck->clearHotCue (0);
    REQUIRE_FALSE (fixture.deck->hasHotCue (0));
}

TEST_CASE ("loading a track clears the hot cues from the last one", "[deck][hotcue]")
{
    Fixture fixture;

    fixture.deck->seekToSeconds (1.0);
    fixture.run (1);
    fixture.deck->hotCuePressed (3);
    REQUIRE (fixture.deck->hasHotCue (3));

    REQUIRE (fixture.deck->loadFile (fixture.temporaryFile.getFile()));
    REQUIRE_FALSE (fixture.deck->hasHotCue (3));
}

TEST_CASE ("a track loads with the hot cues the library kept for it", "[deck][hotcue]")
{
    Fixture fixture;

    opendj::KnownTrack known;
    known.hotCues[0] = 1.25;
    known.hotCues[5] = 6.5;

    REQUIRE (fixture.deck->loadFile (fixture.temporaryFile.getFile(), &known));

    REQUIRE_THAT (fixture.deck->getHotCueSeconds (0), WithinAbs (1.25, 1.0e-9));
    REQUIRE_THAT (fixture.deck->getHotCueSeconds (5), WithinAbs (6.5, 1.0e-9));
    REQUIRE_FALSE (fixture.deck->hasHotCue (1));

    fixture.deck->hotCuePressed (5);
    fixture.run (1);
    REQUIRE_THAT (fixture.deck->getPositionSeconds(), WithinAbs (6.5, 0.01));
}

TEST_CASE ("setting or clearing a hot cue reports every pad, and jumping does not", "[deck][hotcue]")
{
    Fixture fixture;

    std::vector<opendj::HotCues> reported;
    juce::File reportedFile;
    fixture.deck->onHotCuesChanged = [&] (const juce::File& file, const opendj::HotCues& cues)
    {
        reportedFile = file;
        reported.push_back (cues);
    };

    fixture.deck->seekToSeconds (2.0);
    fixture.run (1);
    fixture.deck->hotCuePressed (2);

    REQUIRE (reported.size() == 1);
    REQUIRE (reportedFile == fixture.temporaryFile.getFile());
    REQUIRE_THAT (reported.back()[2], WithinAbs (2.0, 0.01));
    REQUIRE (reported.back()[0] < 0.0);

    fixture.deck->hotCuePressed (2);   // a jump, which changes nothing worth keeping
    REQUIRE (reported.size() == 1);

    fixture.deck->clearHotCue (2);
    REQUIRE (reported.size() == 2);
    REQUIRE (reported.back()[2] < 0.0);

    fixture.deck->clearHotCue (2);     // already empty
    REQUIRE (reported.size() == 2);
}

TEST_CASE ("without key lock a faster tempo raises the pitch", "[deck][keylock]")
{
    Fixture fixture;
    fixture.deck->setTempoRatio (1.10);
    fixture.deck->play();
    fixture.run (20);   // past the fade in

    const auto tone = fixture.capture (100);
    REQUIRE_THAT (measureFrequency (tone, deviceSampleRate), WithinAbs (440.0 * 1.10, 5.0));
}

TEST_CASE ("key lock keeps the pitch when the tempo changes", "[deck][keylock]")
{
    Fixture fixture;
    fixture.deck->setKeyLock (true);
    fixture.deck->setTempoRatio (1.10);
    fixture.deck->play();
    fixture.run (40);   // fade in, and the stretcher's own start-up

    const auto tone = fixture.capture (200);
    REQUIRE (fixture.buffer.getMagnitude (0, 0, blockSize) > 0.3f);
    REQUIRE_THAT (measureFrequency (tone, deviceSampleRate), WithinAbs (440.0, 5.0));

    // Slowing down is the other half of the fader.
    fixture.deck->setTempoRatio (0.92);
    fixture.run (40);
    const auto slower = fixture.capture (200);
    REQUIRE_THAT (measureFrequency (slower, deviceSampleRate), WithinAbs (440.0, 5.0));
}

TEST_CASE ("key lock still consumes the track at the tempo rate", "[deck][keylock]")
{
    Fixture fixture;
    fixture.deck->setKeyLock (true);
    fixture.deck->setTempoRatio (1.10);
    fixture.deck->play();

    const int blocks = 100;
    fixture.run (blocks);

    REQUIRE_THAT (fixture.deck->getPositionSeconds(),
                  WithinAbs (blocks * fixture.secondsPerBlock() * 1.10, 0.005));
}

TEST_CASE ("key lock at the recorded speed is transparent", "[deck][keylock]")
{
    Fixture fixture;
    fixture.deck->setKeyLock (true);
    fixture.deck->play();
    fixture.run (20);

    const auto tone = fixture.capture (100);
    REQUIRE_THAT (measureFrequency (tone, deviceSampleRate), WithinAbs (440.0, 3.0));
    REQUIRE_THAT (fixture.deck->getPositionSeconds(), WithinAbs (120 * fixture.secondsPerBlock(), 0.005));
}

TEST_CASE ("key lock is bypassed while a hand is on the platter", "[deck][keylock][jog]")
{
    Fixture fixture;
    fixture.deck->setKeyLock (true);
    fixture.deck->setTempoRatio (1.10);
    fixture.deck->play();
    fixture.run (40);

    // A hand moving the platter at exactly record speed plays the tone at its
    // recorded pitch through the plain path, and the deck follows the hand.
    fixture.deck->setJogTouched (true);
    const auto ticksPerBlock = 512.0 * fixture.secondsPerBlock() / 1.8;

    for (int i = 0; i < 20; ++i)
    {
        fixture.deck->addJogTicks (ticksPerBlock);
        fixture.run (1);
    }

    const auto before = fixture.deck->getPositionSeconds();
    std::vector<float> tone;

    for (int i = 0; i < 100; ++i)
    {
        fixture.deck->addJogTicks (ticksPerBlock);
        const auto block = fixture.capture (1);
        tone.insert (tone.end(), block.begin(), block.end());
    }

    REQUIRE_THAT (measureFrequency (tone, deviceSampleRate), WithinAbs (440.0, 5.0));
    REQUIRE_THAT (fixture.deck->getPositionSeconds() - before,
                  WithinAbs (100 * fixture.secondsPerBlock(), 0.01));

    // Letting go hands back to the stretcher at the fader's tempo, still in key.
    fixture.deck->setJogTouched (false);
    fixture.run (40);
    const auto after = fixture.capture (200);
    REQUIRE_THAT (measureFrequency (after, deviceSampleRate), WithinAbs (440.0, 5.0));
}

TEST_CASE ("a nudge means the same at any platter resolution", "[deck][jog]")
{
    // The same movement — a fifth of a turn — on two wheels that count very
    // differently must bend the pitch by the same amount, or a mapping that
    // reports absolute position would fling the track away.
    const auto bendAfterFifthOfATurn = [] (int ticksPerRevolution)
    {
        Fixture fixture;
        fixture.deck->setJogTicksPerRevolution (ticksPerRevolution);
        fixture.deck->play();
        fixture.run (20);

        const auto before = fixture.deck->getPositionSeconds();
        fixture.deck->addJogTicks (ticksPerRevolution / 5.0);
        fixture.run (1);

        // How far the block moved, against how far it would have moved unbent.
        const auto moved = fixture.deck->getPositionSeconds() - before;
        return moved / fixture.secondsPerBlock();
    };

    const auto coarse = bendAfterFifthOfATurn (512);
    const auto fine = bendAfterFifthOfATurn (16384);

    INFO ("512 ticks/rev gave " << coarse << ", 16384 gave " << fine);
    REQUIRE_THAT (fine, WithinAbs (coarse, 0.01));

    // And it is a nudge, not a leap: a few percent, not a few hundred.
    REQUIRE (coarse > 1.0);
    REQUIRE (coarse < 1.15);
}

TEST_CASE ("a scratch keeps moving between uneven bursts of ticks", "[deck][jog][scratch]")
{
    // A controller does not report evenly. During the slow part of a scratch,
    // which is the turnaround, several blocks pass with nothing and then two
    // arrive at once. The head has to keep moving through the gaps or the
    // scratch is heard as a stutter.
    Fixture fixture;
    fixture.deck->setJogTicksPerRevolution (800);
    fixture.deck->setJogTouched (true);

    const auto ticksPerBurst = 12.0;
    auto stalledBlocks = 0;
    auto previous = fixture.deck->getPositionSeconds();

    for (int block = 0; block < 60; ++block)
    {
        if (block % 3 == 0)                       // ticks arrive every third block
            fixture.deck->addJogTicks (ticksPerBurst);

        fixture.run (1);

        const auto now = fixture.deck->getPositionSeconds();

        if (block > 3 && std::abs (now - previous) < 1.0e-6)
            ++stalledBlocks;

        previous = now;
    }

    INFO ("blocks with no movement: " << stalledBlocks);
    REQUIRE (stalledBlocks == 0);
}

TEST_CASE ("a scratch still arrives where the hand put it", "[deck][jog][scratch]")
{
    // Chasing the hand must not lose ground: after the platter stops, the head
    // has to settle exactly where the movement asked for, not short of it.
    Fixture fixture;
    fixture.deck->setJogTicksPerRevolution (800);
    fixture.deck->setJogTouched (true);

    const auto start = fixture.deck->getPositionSeconds();

    // Half a turn, which is 0.9 seconds of a record at 33 1/3 rpm.
    fixture.deck->addJogTicks (400.0);
    fixture.run (60);                              // long enough to settle

    REQUIRE_THAT (fixture.deck->getPositionSeconds() - start, WithinAbs (0.9, 0.01));
}

TEST_CASE ("letting go of a scratch does not jump the head", "[deck][jog][scratch]")
{
    Fixture fixture;
    fixture.deck->setJogTicksPerRevolution (800);
    fixture.deck->play();
    fixture.run (20);

    fixture.deck->setJogTouched (true);
    fixture.deck->addJogTicks (200.0);
    fixture.run (40);

    const auto scratched = fixture.deck->getPositionSeconds();

    fixture.deck->setJogTouched (false);
    fixture.run (1);

    // One block of ordinary playback, not a leap back to where the hand was.
    REQUIRE_THAT (fixture.deck->getPositionSeconds() - scratched,
                  WithinAbs (fixture.secondsPerBlock(), 0.002));
}

namespace
{
    /** A separation whose four stems are tones an octave apart, so which of them
        survived a gain change can be read straight off the output. */
    std::shared_ptr<opendj::SeparatedTrack> makeSeparation (int numSamples, double rate)
    {
        auto separation = std::make_shared<opendj::SeparatedTrack>();
        separation->sampleRate = rate;

        const double frequency[opendj::numStems] { 110.0, 220.0, 440.0, 880.0 };

        for (int stem = 0; stem < opendj::numStems; ++stem)
        {
            auto& buffer = separation->stems[(size_t) stem];
            buffer.setSize (2, numSamples);

            const auto delta = juce::MathConstants<double>::twoPi * frequency[stem] / rate;
            double phase = 0.0;

            for (int i = 0; i < numSamples; ++i)
            {
                const auto value = static_cast<float> (std::sin (phase)) * 0.2f;
                phase += delta;
                buffer.setSample (0, i, value);
                buffer.setSample (1, i, value);
            }
        }

        return separation;
    }
}

TEST_CASE ("a separation is refused unless it matches the loaded track", "[deck][stems]")
{
    Fixture fixture;
    auto separation = makeSeparation (44100, 44100.0);

    // A separation finishing after the deck moved on belongs to another record.
    fixture.deck->setSeparation (separation, juce::File ("/some/other/track.mp3"));
    REQUIRE_FALSE (fixture.deck->hasStems());

    fixture.deck->setSeparation (separation, fixture.deck->getLoadedFile());
    REQUIRE (fixture.deck->hasStems());
}

TEST_CASE ("a malformed separation is refused", "[deck][stems]")
{
    Fixture fixture;

    auto ragged = makeSeparation (44100, 44100.0);
    ragged->stems[2].setSize (2, 100);        // one stem a different length

    fixture.deck->setSeparation (ragged, fixture.deck->getLoadedFile());
    REQUIRE_FALSE (fixture.deck->hasStems());

    auto empty = std::make_shared<opendj::SeparatedTrack>();
    fixture.deck->setSeparation (empty, fixture.deck->getLoadedFile());
    REQUIRE_FALSE (fixture.deck->hasStems());
}

TEST_CASE ("stems at unity sound like the track they came from", "[deck][stems]")
{
    // Every stem at 1 must leave the output untouched: the deck should still be
    // playing the recorded mix, not a reconstruction of it.
    Fixture withoutStems;
    withoutStems.deck->play();
    withoutStems.run (30);
    const auto plain = withoutStems.capture (20);

    Fixture withStems;
    withStems.deck->setSeparation (makeSeparation (44100 * 8, 44100.0),
                                   withStems.deck->getLoadedFile());
    withStems.deck->play();
    withStems.run (30);
    const auto stems = withStems.capture (20);

    REQUIRE (plain.size() == stems.size());

    auto worst = 0.0f;

    for (size_t i = 0; i < plain.size(); ++i)
        worst = juce::jmax (worst, std::abs (plain[i] - stems[i]));

    INFO ("largest difference: " << worst);
    REQUIRE (worst < 1.0e-6f);
}

TEST_CASE ("silencing a stem removes it and leaves the others", "[deck][stems]")
{
    Fixture fixture;
    fixture.deck->setSeparation (makeSeparation (44100 * 8, 44100.0),
                                 fixture.deck->getLoadedFile());
    fixture.deck->play();

    // Everything but the 440 Hz stem turned off, so that is what should be left.
    fixture.deck->setStemGain (opendj::Stem::drums, 0.0f);
    fixture.deck->setStemGain (opendj::Stem::bass, 0.0f);
    fixture.deck->setStemGain (opendj::Stem::vocals, 0.0f);
    fixture.run (40);

    const auto tone = fixture.capture (100);
    REQUIRE_THAT (measureFrequency (tone, deviceSampleRate), WithinAbs (440.0, 5.0));

    // And with that one gone too, silence.
    fixture.deck->setStemGain (opendj::Stem::other, 0.0f);
    fixture.run (40);
    fixture.deck->processBlock (fixture.buffer);
    REQUIRE (fixture.buffer.getMagnitude (0, 0, blockSize) < 0.001f);
}

TEST_CASE ("a stem gain change does not click", "[deck][stems]")
{
    Fixture fixture;
    fixture.deck->setSeparation (makeSeparation (44100 * 8, 44100.0),
                                 fixture.deck->getLoadedFile());
    fixture.deck->play();
    fixture.run (30);

    fixture.deck->setStemGain (opendj::Stem::vocals, 0.0f);

    const auto samples = fixture.capture (4);
    auto biggestStep = 0.0f;

    for (size_t i = 1; i < samples.size(); ++i)
        biggestStep = juce::jmax (biggestStep, std::abs (samples[i] - samples[i - 1]));

    // A step change would show as a jump far larger than one sample of a tone.
    INFO ("largest sample-to-sample step: " << biggestStep);
    REQUIRE (biggestStep < 0.05f);
}

TEST_CASE ("loading another track drops the stems with it", "[deck][stems]")
{
    Fixture fixture;
    fixture.deck->setSeparation (makeSeparation (44100 * 8, 44100.0),
                                 fixture.deck->getLoadedFile());
    REQUIRE (fixture.deck->hasStems());

    REQUIRE (fixture.deck->loadFile (fixture.temporaryFile.getFile()));
    REQUIRE_FALSE (fixture.deck->hasStems());
}

//==============================================================================
// Brake, backspin, reverse and censor
//==============================================================================

namespace
{
    int blocksFor (double seconds) { return (int) std::ceil (seconds * deviceSampleRate / blockSize); }

    /** Plays from the given position until the playhead has really got there. */
    void playFrom (Fixture& fixture, double seconds)
    {
        fixture.deck->seekToSeconds (seconds);
        fixture.deck->play();
        fixture.run (4);
    }

    float largestStepIn (const std::vector<float>& samples)
    {
        auto largest = 0.0f;

        for (size_t i = 1; i < samples.size(); ++i)
            largest = juce::jmax (largest, std::abs (samples[i] - samples[i - 1]));

        return largest;
    }
}

TEST_CASE ("a brake lowers the pitch and stops the deck", "[deck][performance]")
{
    Fixture fixture;
    playFrom (fixture, 1.0);

    const auto start = fixture.deck->getPositionSeconds();
    fixture.deck->brake();
    fixture.run (1);
    REQUIRE (fixture.deck->isSpinningDown());

    // Counting crossings shows the pitch itself fell, not only that the
    // position slowed down. A linear ramp halves it by half way.
    const auto quarter = blocksFor (opendj::Deck::brakeSeconds * 0.25);
    const auto early = measureFrequency (fixture.capture (quarter), deviceSampleRate);
    fixture.run (quarter);
    const auto late = measureFrequency (fixture.capture (quarter), deviceSampleRate);

    INFO ("early " << early << " Hz, late " << late << " Hz");
    REQUIRE (early > 330.0);
    REQUIRE (early < 440.0);
    REQUIRE (late < early * 0.6);

    fixture.run (quarter + 4);

    REQUIRE_FALSE (fixture.deck->isSpinningDown());
    REQUIRE_FALSE (fixture.deck->isPlaying());

    // From full speed to nothing at an even rate covers half the distance.
    REQUIRE_THAT (fixture.deck->getPositionSeconds() - start, WithinAbs (opendj::Deck::brakeSeconds * 0.5, 0.02));
}

TEST_CASE ("a brake ends in silence, with no chirp from the ordinary fade", "[deck][performance]")
{
    Fixture fixture;
    playFrom (fixture, 1.0);
    fixture.deck->brake();

    fixture.run (blocksFor (opendj::Deck::brakeSeconds * 0.5));
    const auto tail = fixture.capture (blocksFor (opendj::Deck::brakeSeconds * 0.6));

    // Without the fade the last stretch is a DC level that stops dead, and
    // without keeping the level at zero the fade-out plays at full speed.
    REQUIRE (largestStepIn (tail) < 0.03f);
    REQUIRE (std::abs (tail.back()) < 1.0e-6f);

    fixture.run (2);
    REQUIRE (fixture.buffer.getMagnitude (0, 0, blockSize) < 1.0e-6f);
}

TEST_CASE ("a brake falls in pitch with key lock on", "[deck][performance][keylock]")
{
    Fixture fixture;
    fixture.deck->setKeyLock (true);
    fixture.deck->setTempoRatio (1.08);
    playFrom (fixture, 1.0);
    fixture.run (blocksFor (0.5));

    fixture.deck->brake();
    fixture.run (blocksFor (opendj::Deck::brakeSeconds * 0.55));

    // The stretcher would hold this at 440 had it stayed in the path.
    const auto late = measureFrequency (fixture.capture (blocksFor (0.1)), deviceSampleRate);

    INFO ("late " << late << " Hz");
    REQUIRE (late < 300.0);
}

TEST_CASE ("a stopped deck cannot be braked or spun back", "[deck][performance]")
{
    Fixture fixture;

    fixture.deck->brake();
    fixture.run (2);
    REQUIRE_FALSE (fixture.deck->isSpinningDown());

    fixture.deck->backspin();
    fixture.run (2);
    REQUIRE_FALSE (fixture.deck->isSpinningDown());
    REQUIRE_THAT (fixture.deck->getPositionSeconds(), WithinAbs (0.0, 1.0e-6));
}

TEST_CASE ("play catches a brake, without a click", "[deck][performance]")
{
    Fixture fixture;
    playFrom (fixture, 1.0);
    fixture.deck->brake();

    // Far enough in that the level is on its way down.
    fixture.run (blocksFor (opendj::Deck::brakeSeconds * 0.95));
    REQUIRE (fixture.deck->isSpinningDown());

    fixture.deck->play();
    const auto after = fixture.capture (10);

    REQUIRE_FALSE (fixture.deck->isSpinningDown());
    REQUIRE (fixture.deck->isPlaying());
    REQUIRE (largestStepIn (after) < 0.1f);

    // Back at the tempo: a whole block's worth of position for a block.
    const auto before = fixture.deck->getPositionSeconds();
    fixture.run (10);
    REQUIRE_THAT (fixture.deck->getPositionSeconds() - before, WithinAbs (10 * fixture.secondsPerBlock(), 0.001));
}

TEST_CASE ("with slip on, a brake lands back on time and keeps playing", "[deck][performance][slip]")
{
    Fixture fixture;
    playFrom (fixture, 1.0);
    fixture.deck->setSlipEnabled (true);

    const auto start = fixture.deck->getPositionSeconds();
    fixture.deck->brake();

    const auto blocks = blocksFor (opendj::Deck::brakeSeconds + 0.1);
    fixture.run (blocks);

    REQUIRE_FALSE (fixture.deck->isSpinningDown());
    REQUIRE (fixture.deck->isPlaying());
    REQUIRE_THAT (fixture.deck->getPositionSeconds() - start, WithinAbs (blocks * fixture.secondsPerBlock(), 0.03));

    // And it can be heard again.
    fixture.run (4);
    REQUIRE (fixture.buffer.getMagnitude (0, 0, blockSize) > 0.4f);
}

TEST_CASE ("a backspin runs backwards fast, then stops", "[deck][performance]")
{
    Fixture fixture;
    playFrom (fixture, 5.0);

    const auto start = fixture.deck->getPositionSeconds();
    fixture.deck->backspin();
    fixture.run (blocksFor (0.1));

    // Close to the full speed for a tenth of a second, the wrong way.
    REQUIRE_THAT (fixture.deck->getPositionSeconds() - start, WithinAbs (-0.1 * opendj::Deck::backspinSpeed, 0.08));

    fixture.run (blocksFor (opendj::Deck::backspinSeconds));

    REQUIRE_FALSE (fixture.deck->isPlaying());
    REQUIRE_THAT (fixture.deck->getPositionSeconds() - start,
                  WithinAbs (-opendj::Deck::backspinSpeed * opendj::Deck::backspinSeconds * 0.5, 0.05));
}

TEST_CASE ("reverse plays backwards at the tempo and at the same pitch", "[deck][performance]")
{
    Fixture fixture;
    playFrom (fixture, 4.0);

    const auto start = fixture.deck->getPositionSeconds();
    fixture.deck->setReverse (true);

    const auto blocks = blocksFor (1.0);
    const auto samples = fixture.capture (blocks);

    REQUIRE_THAT (fixture.deck->getPositionSeconds() - start, WithinAbs (-blocks * fixture.secondsPerBlock(), 0.005));
    REQUIRE_THAT (measureFrequency (samples, deviceSampleRate), WithinAbs (440.0, 3.0));

    // Forwards again from wherever it has reached.
    fixture.deck->toggleReverse();
    const auto turned = fixture.deck->getPositionSeconds();
    fixture.run (blocks);
    REQUIRE_THAT (fixture.deck->getPositionSeconds() - turned, WithinAbs (blocks * fixture.secondsPerBlock(), 0.005));
}

TEST_CASE ("reverse off the front of the track stops the deck and turns itself off", "[deck][performance]")
{
    Fixture fixture;
    playFrom (fixture, 0.2);
    fixture.deck->setReverse (true);

    fixture.run (blocksFor (0.5));

    REQUIRE_FALSE (fixture.deck->isPlaying());
    REQUIRE_FALSE (fixture.deck->isReverseEnabled());
    REQUIRE_THAT (fixture.deck->getPositionSeconds(), WithinAbs (0.0, 1.0e-6));

    // Play now means play, rather than stopping again straight away.
    fixture.deck->play();
    fixture.run (blocksFor (0.1));
    REQUIRE (fixture.deck->isPlaying());
    REQUIRE (fixture.deck->getPositionSeconds() > 0.05);
}

TEST_CASE ("with slip on, reverse lands where the track would have reached", "[deck][performance][slip]")
{
    Fixture fixture;
    playFrom (fixture, 3.0);
    fixture.deck->setSlipEnabled (true);

    const auto start = fixture.deck->getPositionSeconds();
    fixture.deck->setReverse (true);

    const auto blocks = blocksFor (0.5);
    fixture.run (blocks);
    fixture.deck->setReverse (false);
    fixture.run (1);

    REQUIRE_THAT (fixture.deck->getPositionSeconds() - start,
                  WithinAbs ((blocks + 1) * fixture.secondsPerBlock(), 0.01));
}

TEST_CASE ("censor runs backwards while held and lands back on time", "[deck][performance]")
{
    Fixture fixture;
    playFrom (fixture, 3.0);

    const auto start = fixture.deck->getPositionSeconds();
    fixture.deck->setCensor (true);

    const auto blocks = blocksFor (0.5);
    fixture.run (blocks);

    REQUIRE_THAT (fixture.deck->getPositionSeconds() - start, WithinAbs (-blocks * fixture.secondsPerBlock(), 0.005));
    REQUIRE (fixture.deck->getSlipPositionSeconds() > start);

    // Slip is off, and the deck lands on time anyway: that is the feature.
    fixture.deck->setCensor (false);
    fixture.run (1);

    REQUIRE_THAT (fixture.deck->getPositionSeconds() - start,
                  WithinAbs ((blocks + 1) * fixture.secondsPerBlock(), 0.01));
    REQUIRE (fixture.deck->getSlipPositionSeconds() < 0.0);
}

TEST_CASE ("letting go of a censor does not tick", "[deck][performance]")
{
    Fixture fixture;
    playFrom (fixture, 3.0);

    fixture.deck->setCensor (true);
    fixture.run (blocksFor (0.3));
    fixture.deck->setCensor (false);

    // Landing is a jump to a different part of the waveform. A 440 Hz sine at
    // half level moves by about 0.03 a sample; a jump is many times that.
    const auto around = fixture.capture (6);

    REQUIRE (largestStepIn (around) < 0.05f);
}

TEST_CASE ("a censor held against the start of the track does not stop the deck", "[deck][performance]")
{
    Fixture fixture;
    playFrom (fixture, 0.1);

    const auto start = fixture.deck->getPositionSeconds();
    fixture.deck->setCensor (true);

    const auto blocks = blocksFor (0.5);
    fixture.run (blocks);
    REQUIRE (fixture.deck->isPlaying());

    fixture.deck->setCensor (false);
    fixture.run (1);

    REQUIRE (fixture.deck->isPlaying());
    REQUIRE_THAT (fixture.deck->getPositionSeconds() - start,
                  WithinAbs ((blocks + 1) * fixture.secondsPerBlock(), 0.01));
}

TEST_CASE ("pausing during a censor leaves nothing to jump back to", "[deck][performance]")
{
    Fixture fixture;
    playFrom (fixture, 3.0);

    fixture.deck->setCensor (true);
    fixture.run (blocksFor (0.5));

    fixture.deck->pause();
    fixture.run (blocksFor (0.1));
    fixture.deck->setCensor (false);

    const auto stoppedAt = fixture.deck->getPositionSeconds();

    fixture.deck->play();
    fixture.run (2);

    REQUIRE_THAT (fixture.deck->getPositionSeconds() - stoppedAt, WithinAbs (2 * fixture.secondsPerBlock(), 0.005));
}

TEST_CASE ("a hand on the platter ends a brake", "[deck][performance]")
{
    Fixture fixture;
    playFrom (fixture, 1.0);
    fixture.deck->brake();
    fixture.run (blocksFor (0.2));
    REQUIRE (fixture.deck->isSpinningDown());

    fixture.deck->setJogTouched (true);
    fixture.run (2);

    REQUIRE_FALSE (fixture.deck->isSpinningDown());
    REQUIRE (fixture.deck->isPlaying());
}

TEST_CASE ("loading a track clears reverse and censor", "[deck][performance]")
{
    Fixture fixture;
    fixture.deck->setReverse (true);
    fixture.deck->setCensor (true);

    REQUIRE (fixture.deck->loadFile (fixture.temporaryFile.getFile()));

    REQUIRE_FALSE (fixture.deck->isReverseEnabled());
    REQUIRE_FALSE (fixture.deck->isCensorHeld());
}

//==============================================================================
// Key shift
//==============================================================================

TEST_CASE ("a key shift moves the pitch by the semitones asked for", "[deck][key]")
{
    Fixture fixture;
    playFrom (fixture, 1.0);

    const auto measureNow = [&fixture]
    {
        fixture.run (blocksFor (0.3));     // past whatever the change itself leaves behind
        return measureFrequency (fixture.capture (blocksFor (0.5)), deviceSampleRate);
    };

    REQUIRE_THAT (measureNow(), WithinAbs (440.0, 3.0));

    fixture.deck->setKeyShift (2);
    REQUIRE_THAT (measureNow(), WithinAbs (440.0 * std::pow (2.0, 2.0 / 12.0), 5.0));

    fixture.deck->setKeyShift (-3);
    REQUIRE_THAT (measureNow(), WithinAbs (440.0 * std::pow (2.0, -3.0 / 12.0), 5.0));

    fixture.deck->setKeyShift (0);
    REQUIRE_THAT (measureNow(), WithinAbs (440.0, 3.0));
}

TEST_CASE ("a key shift leaves the tempo alone", "[deck][key]")
{
    Fixture fixture;
    fixture.deck->setKeyShift (5);
    playFrom (fixture, 1.0);

    const auto start = fixture.deck->getPositionSeconds();
    const auto blocks = blocksFor (1.0);
    fixture.run (blocks);

    REQUIRE_THAT (fixture.deck->getPositionSeconds() - start, WithinAbs (blocks * fixture.secondsPerBlock(), 0.005));
    REQUIRE (fixture.buffer.getMagnitude (0, 0, blockSize) > 0.2f);
}

TEST_CASE ("with key lock off, the tempo moves the pitch on top of a key shift", "[deck][key]")
{
    Fixture fixture;
    fixture.deck->setKeyShift (2);
    fixture.deck->setTempoRatio (1.08);
    playFrom (fixture, 1.0);
    fixture.run (blocksFor (0.3));

    const auto expected = 440.0 * 1.08 * std::pow (2.0, 2.0 / 12.0);
    REQUIRE_THAT (measureFrequency (fixture.capture (blocksFor (0.5)), deviceSampleRate), WithinAbs (expected, 6.0));
}

TEST_CASE ("with key lock on, only the key shift moves the pitch", "[deck][key][keylock]")
{
    Fixture fixture;
    fixture.deck->setKeyLock (true);
    fixture.deck->setKeyShift (2);
    fixture.deck->setTempoRatio (1.08);
    playFrom (fixture, 1.0);
    fixture.run (blocksFor (0.3));

    const auto expected = 440.0 * std::pow (2.0, 2.0 / 12.0);
    REQUIRE_THAT (measureFrequency (fixture.capture (blocksFor (0.5)), deviceSampleRate), WithinAbs (expected, 5.0));
}

TEST_CASE ("key lock at the tempo the track was recorded at still costs nothing", "[deck][key][keylock]")
{
    // A shift of zero must not hold the stretcher in the path, or a deck at its
    // own tempo would pick up a delay nobody asked for.
    Fixture plain, shifted;
    plain.deck->setKeyLock (true);
    shifted.deck->setKeyLock (true);
    shifted.deck->setKeyShift (1);
    shifted.deck->setKeyShift (0);

    plain.deck->play();
    shifted.deck->play();

    for (int b = 0; b < 20; ++b)
    {
        plain.deck->processBlock (plain.buffer);
        shifted.deck->processBlock (shifted.buffer);

        for (int i = 0; i < blockSize; ++i)
            REQUIRE (plain.buffer.getSample (0, i) == shifted.buffer.getSample (0, i));
    }
}

TEST_CASE ("a shift cannot go past an octave, and a new track starts without one", "[deck][key]")
{
    Fixture fixture;

    fixture.deck->setKeyShift (40);
    REQUIRE (fixture.deck->getKeyShift() == opendj::Deck::maximumKeyShift);

    fixture.deck->setKeyShift (-40);
    REQUIRE (fixture.deck->getKeyShift() == -opendj::Deck::maximumKeyShift);

    REQUIRE (fixture.deck->loadFile (fixture.temporaryFile.getFile()));
    REQUIRE (fixture.deck->getKeyShift() == 0);
}

TEST_CASE ("the deck's key follows the shift and, without key lock, the tempo", "[deck][key]")
{
    Fixture fixture;

    opendj::KnownTrack known;
    known.analysed = true;
    known.bpm = 120.0;
    known.key = opendj::MusicalKey::fromString ("Am");
    REQUIRE (fixture.deck->loadFile (fixture.temporaryFile.getFile(), &known));

    REQUIRE (fixture.deck->getEffectiveKey().toString() == "Am");

    fixture.deck->setKeyShift (2);
    REQUIRE (fixture.deck->getEffectiveKey().toString() == "Bm");

    // Six percent is a semitone.
    fixture.deck->setTempoRatio (1.06);
    REQUIRE (fixture.deck->getEffectiveKey().toString() == "Cm");

    fixture.deck->setKeyLock (true);
    REQUIRE (fixture.deck->getEffectiveKey().toString() == "Bm");

    // Under half a semitone reads as no move at all.
    fixture.deck->setKeyLock (false);
    fixture.deck->setKeyShift (0);
    fixture.deck->setTempoRatio (1.02);
    REQUIRE (fixture.deck->getEffectiveKey().toString() == "Am");
}

TEST_CASE ("a track the library knows arrives with its key", "[deck][key]")
{
    Fixture fixture;

    opendj::KnownTrack known;
    known.analysed = true;
    known.bpm = 120.0;
    known.key = opendj::MusicalKey::fromString ("F#m");
    REQUIRE (fixture.deck->loadFile (fixture.temporaryFile.getFile(), &known));

    REQUIRE (fixture.deck->getAnalysis()->key.toString() == "F#m");
}

TEST_CASE ("a key in the tags beats detection on a track's first load", "[deck][key]")
{
    Fixture fixture;

    opendj::KnownTrack known;
    known.analysed = false;
    known.key = opendj::MusicalKey::fromString ("D#");
    REQUIRE (fixture.deck->loadFile (fixture.temporaryFile.getFile(), &known));

    REQUIRE (fixture.deck->getAnalysis()->key.toString() == "D#");
}

TEST_CASE ("with no key in the tags, a first load detects one as before", "[deck][key]")
{
    Fixture fixture;

    opendj::KnownTrack known;
    known.analysed = false;
    REQUIRE (fixture.deck->loadFile (fixture.temporaryFile.getFile(), &known));

    const auto detected = fixture.deck->getAnalysis()->key;
    REQUIRE (fixture.deck->loadFile (fixture.temporaryFile.getFile()));
    REQUIRE (fixture.deck->getAnalysis()->key.toString() == detected.toString());
}
