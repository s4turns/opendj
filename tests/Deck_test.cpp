/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Deck.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <thread>
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

            writeTrack (content, fileSampleRate, 16);
        }

        /** Puts this audio in the fixture's file, in place of whatever was there. */
        void writeTrack (const juce::AudioBuffer<float>& content, double fileSampleRate, int bitDepth)
        {
            temporaryFile.getFile().deleteFile();

            juce::WavAudioFormat wav;
            auto stream = std::make_unique<juce::FileOutputStream> (temporaryFile.getFile());
            REQUIRE (stream->openedOk());

            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (stream.release(), fileSampleRate, 2, bitDepth, {}, 0));
            REQUIRE (writer != nullptr);
            REQUIRE (writer->writeFromAudioSampleBuffer (content, 0, content.getNumSamples()));
        }

        /** Advances the deck by the given number of blocks. */
        void run (int numBlocks)
        {
            for (int i = 0; i < numBlocks; ++i)
                deck->processBlock (buffer);
        }

        /** Advances the deck and keeps the left channel of everything it produced. */
        std::vector<float> capture (int numBlocks, int channel = 0)
        {
            std::vector<float> samples;
            samples.reserve ((size_t) (numBlocks * blockSize));

            for (int i = 0; i < numBlocks; ++i)
            {
                deck->processBlock (buffer);
                const auto* heard = buffer.getReadPointer (channel);
                samples.insert (samples.end(), heard, heard + blockSize);
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
    std::shared_ptr<opendj::SeparatedTrack> makeSeparation (int numSamples, double rate, float scale = 1.0f)
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
                const auto value = static_cast<float> (std::sin (phase)) * 0.2f * scale;
                phase += delta;
                buffer.setSample (0, i, value);
                buffer.setSample (1, i, value);
            }
        }

        return separation;
    }

    /** Loads a track whose mix really is the sum of makeSeparation's stems, and
        hands the deck the stems. The fixture's own track is a lone 440 Hz tone,
        so with that one the deck's mix and stems are two different signals, and
        since the deck plays the mix at unity, all four tones are only there to be
        measured with this.

        Scale the stems to make them miss the mix by a known amount, which is what
        a real model's do by an amount nobody can predict. The file is floating
        point, so that with no scaling the two agree to the last bit or so. */
    std::shared_ptr<opendj::SeparatedTrack> loadTrackOfStems (Fixture& fixture, float stemScale = 1.0f)
    {
        constexpr double rate = 44100.0;
        const auto numSamples = static_cast<int> (fixtureSeconds * rate);
        const auto exact = makeSeparation (numSamples, rate);

        juce::AudioBuffer<float> mix (2, numSamples);
        mix.clear();

        for (const auto& stem : exact->stems)
            for (int ch = 0; ch < 2; ++ch)
                mix.addFrom (ch, 0, stem, ch, 0, numSamples);

        fixture.writeTrack (mix, rate, 32);
        REQUIRE (fixture.deck->loadFile (fixture.temporaryFile.getFile()));

        auto stems = makeSeparation (numSamples, rate, stemScale);
        fixture.deck->setSeparation (stems, fixture.deck->getLoadedFile());
        REQUIRE (fixture.deck->hasStems());
        return stems;
    }

    /** How much of one tone is in a signal, as its amplitude. Windowed, because
        the stems are an octave apart and a tone's edges would otherwise leak into
        the measurement of its neighbours. */
    double toneLevel (const std::vector<float>& samples, double frequency)
    {
        const auto delta = juce::MathConstants<double>::twoPi * frequency / deviceSampleRate;
        const auto windowStep = juce::MathConstants<double>::twoPi / static_cast<double> (samples.size());
        double real = 0.0, imaginary = 0.0, windowSum = 0.0;

        for (size_t i = 0; i < samples.size(); ++i)
        {
            const auto window = 0.5 - 0.5 * std::cos (windowStep * static_cast<double> (i));
            const auto phase = delta * static_cast<double> (i);
            real += window * samples[i] * std::cos (phase);
            imaginary += window * samples[i] * std::sin (phase);
            windowSum += window;
        }

        return 2.0 * std::sqrt (real * real + imaginary * imaginary) / windowSum;
    }

    /** The same over a run of short windows, for a tone through the stretcher.
        It holds a pure tone's level well but not its phase, which jumps now and
        then and would cancel itself out in one long window. */
    double shortWindowToneLevel (const std::vector<float>& samples, double frequency)
    {
        constexpr size_t windowSize = 8192;
        double sum = 0.0;
        auto windows = 0;

        for (size_t start = 0; start + windowSize <= samples.size(); start += windowSize, ++windows)
            sum += toneLevel (std::vector<float> (samples.begin() + (std::ptrdiff_t) start,
                                                  samples.begin() + (std::ptrdiff_t) (start + windowSize)),
                              frequency);

        return sum / juce::jmax (1, windows);
    }

    constexpr double stemTones[opendj::numStems] { 110.0, 220.0, 440.0, 880.0 };
    constexpr double stemToneLevel = 0.2;

    /** Requires each stem's tone at the level given, in the order the stems run. */
    void requireStemLevels (const std::vector<float>& samples, double drums, double bass, double other, double vocals)
    {
        const double expected[opendj::numStems] { drums, bass, other, vocals };

        for (int stem = 0; stem < opendj::numStems; ++stem)
        {
            INFO (opendj::toString (static_cast<opendj::Stem> (stem)));
            REQUIRE_THAT (toneLevel (samples, stemTones[stem]), WithinAbs (expected[stem], 0.003));
        }
    }

    bool sameSamples (const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
    }

    /** The largest change of slope from one sample to the next. */
    float biggestBend (const std::vector<float>& samples)
    {
        auto biggest = 0.0f;

        for (size_t i = 2; i < samples.size(); ++i)
            biggest = juce::jmax (biggest, std::abs (samples[i] - 2.0f * samples[i - 1] + samples[i - 2]));

        return biggest;
    }

    float biggestDifferenceStep (const std::vector<float>& a, const std::vector<float>& b)
    {
        auto biggest = 0.0f;

        for (size_t i = 1; i < a.size(); ++i)
            biggest = juce::jmax (biggest, std::abs ((a[i] - b[i]) - (a[i - 1] - b[i - 1])));

        return biggest;
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
    // On a mix the stems really sum to. On any other, a difference between the
    // two is real and this would measure that instead.
    Fixture fixture;
    loadTrackOfStems (fixture);
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

TEST_CASE ("a track whose mix is its stems really has all four of them in it", "[deck][stems]")
{
    Fixture fixture;
    loadTrackOfStems (fixture);
    fixture.deck->play();
    fixture.run (20);

    const auto heard = fixture.capture (100);
    requireStemLevels (heard, stemToneLevel, stemToneLevel, stemToneLevel, stemToneLevel);
}

TEST_CASE ("a stem shorter than the track is silence past its end", "[deck][stems]")
{
    // A stem cache from an older copy of a file passes as well formed, its stems
    // all being the same length as each other. The buffers keep their channels
    // side by side, so reading the left past its end would play the right.
    Fixture fixture;
    constexpr auto stemSamples = static_cast<int> (fixtureSeconds * 44100.0) / 2;

    const auto shortStems = makeSeparation (stemSamples, 44100.0);
    fixture.deck->setSeparation (shortStems, fixture.deck->getLoadedFile());
    REQUIRE (fixture.deck->hasStems());

    const auto stemsEndSeconds = stemSamples / 44100.0;

    fixture.deck->setStemGain (opendj::Stem::vocals, 0.5f);
    fixture.deck->seekToSeconds (stemsEndSeconds - 0.25);
    fixture.deck->play();
    fixture.run (5);

    // Still inside, the other stems playing.
    REQUIRE (fixture.deck->getPositionSeconds() < stemsEndSeconds);
    fixture.deck->processBlock (fixture.buffer);
    REQUIRE (fixture.buffer.getMagnitude (0, blockSize) > 0.05f);

    while (fixture.deck->getPositionSeconds() < stemsEndSeconds + 0.001)
        fixture.deck->processBlock (fixture.buffer);

    REQUIRE (fixture.deck->isPlaying());

    const auto past = fixture.capture (50);

    for (const auto sample : past)
        REQUIRE (std::abs (sample) <= 0.0f);
}

//==============================================================================
// Stem pads
//==============================================================================

TEST_CASE ("each stem pad leaves exactly its stems", "[deck][stems][pads]")
{
    const auto pad = GENERATE (0, 1, 2, 3);
    INFO ("pad " << pad);

    // Written out rather than read from the table being tested.
    const double expected[opendj::numStemPads][opendj::numStems]
    {
        { stemToneLevel, stemToneLevel, stemToneLevel, 0.0 },
        { 0.0, 0.0, 0.0, stemToneLevel },
        { stemToneLevel, 0.0, 0.0, 0.0 },
        { 0.0, stemToneLevel, stemToneLevel, stemToneLevel }
    };

    Fixture fixture;
    loadTrackOfStems (fixture);
    fixture.deck->play();
    fixture.run (10);

    fixture.deck->toggleStemPad (pad);
    REQUIRE (fixture.deck->getStemPad() == pad);
    fixture.run (10);

    const auto& levels = expected[pad];
    requireStemLevels (fixture.capture (100), levels[0], levels[1], levels[2], levels[3]);
}

TEST_CASE ("a pad multiplies with the knobs and leaves them where they are", "[deck][stems][pads]")
{
    Fixture fixture;
    loadTrackOfStems (fixture);
    fixture.deck->setStemGain (opendj::Stem::vocals, 0.5f);
    fixture.deck->play();

    fixture.deck->toggleStemPad (1);
    fixture.run (10);
    requireStemLevels (fixture.capture (100), 0.0, 0.0, 0.0, stemToneLevel * 0.5);

    fixture.deck->toggleStemPad (1);
    REQUIRE (fixture.deck->getStemPad() == -1);
    REQUIRE_THAT (fixture.deck->getStemGain (opendj::Stem::vocals), WithinAbs (0.5, 1.0e-6));
    REQUIRE_THAT (fixture.deck->getStemGain (opendj::Stem::drums), WithinAbs (1.0, 1.0e-6));

    fixture.run (10);
    requireStemLevels (fixture.capture (100), stemToneLevel, stemToneLevel, stemToneLevel, stemToneLevel * 0.5);
}

TEST_CASE ("a knob moved under a pad is heard as the product of the two", "[deck][stems][pads]")
{
    // This is what automix does: it rewrites the vocal knob every tick of a
    // handoff, and neither of them should undo the other.
    Fixture fixture;
    loadTrackOfStems (fixture);
    fixture.deck->play();

    fixture.deck->toggleStemPad (1);
    fixture.deck->setStemGain (opendj::Stem::vocals, 0.25f);
    fixture.run (10);
    requireStemLevels (fixture.capture (100), 0.0, 0.0, 0.0, stemToneLevel * 0.25);

    // With the vocals masked, restoring the knob must not bring them back.
    fixture.deck->toggleStemPad (0);
    fixture.deck->setStemGain (opendj::Stem::vocals, 1.0f);
    fixture.run (10);
    requireStemLevels (fixture.capture (100), stemToneLevel, stemToneLevel, stemToneLevel, 0.0);
}

TEST_CASE ("a new pad replaces the last, and a hold returns to the latched one", "[deck][stems][pads]")
{
    Fixture fixture;
    loadTrackOfStems (fixture);
    auto& deck = *fixture.deck;

    deck.toggleStemPad (0);
    deck.toggleStemPad (1);
    REQUIRE (deck.getStemPad() == 1);

    deck.holdStemPad (2, true);
    REQUIRE (deck.getStemPad() == 2);

    // A second hand. The first letting go is not the end of the second.
    deck.holdStemPad (3, true);
    REQUIRE (deck.getStemPad() == 3);
    deck.holdStemPad (2, false);
    REQUIRE (deck.getStemPad() == 3);

    deck.holdStemPad (3, false);
    REQUIRE (deck.getStemPad() == 1);

    deck.toggleStemPad (1);
    REQUIRE (deck.getStemPad() == -1);
}

TEST_CASE ("a pad press with no stems, or of no such pad, does nothing", "[deck][stems][pads]")
{
    Fixture pressed, untouched;

    pressed.deck->toggleStemPad (0);
    pressed.deck->holdStemPad (1, true);
    REQUIRE (pressed.deck->getStemPad() == -1);

    pressed.deck->play();
    untouched.deck->play();
    REQUIRE (sameSamples (pressed.capture (30), untouched.capture (30)));

    // And it was not remembered for when stems turn up.
    pressed.deck->setSeparation (makeSeparation (44100 * 8, 44100.0), pressed.deck->getLoadedFile());
    REQUIRE (pressed.deck->getStemPad() == -1);

    for (const auto slot : { -1, 4, 100 })
    {
        pressed.deck->toggleStemPad (slot);
        pressed.deck->holdStemPad (slot, true);
        REQUIRE (pressed.deck->getStemPad() == -1);
    }
}

TEST_CASE ("stems at unity with no pad are the mix to the last bit", "[deck][stems][pads]")
{
    // Direct and through the stretcher: stems that are there and not being used
    // must cost nothing and change nothing. They are scaled so that any of them
    // getting into the output would show.
    const auto keyShift = GENERATE (0, 2);
    INFO ("key shift " << keyShift);

    Fixture withStems, withoutStems;
    loadTrackOfStems (withStems, 0.8f);
    loadTrackOfStems (withoutStems);
    withoutStems.deck->setSeparation (nullptr, withoutStems.deck->getLoadedFile());
    REQUIRE_FALSE (withoutStems.deck->hasStems());

    withStems.deck->setKeyShift (keyShift);
    withoutStems.deck->setKeyShift (keyShift);
    withStems.deck->play();
    withoutStems.deck->play();

    REQUIRE (sameSamples (withStems.capture (60), withoutStems.capture (60)));
}

TEST_CASE ("letting go of a pad goes back to the mix itself", "[deck][stems][pads]")
{
    Fixture withStems, control;
    loadTrackOfStems (withStems, 0.8f);
    loadTrackOfStems (control);
    withStems.deck->play();
    control.deck->play();
    withStems.run (10);
    control.run (10);

    withStems.deck->holdStemPad (3, true);
    withStems.run (20);
    control.run (20);
    withStems.deck->holdStemPad (3, false);

    // One block for the gains and another for the crossfade.
    withStems.run (3);
    control.run (3);

    REQUIRE (sameSamples (withStems.capture (10), control.capture (10)));
}

TEST_CASE ("a pad and a turned down knob do not follow onto the next track", "[deck][stems][pads]")
{
    Fixture fixture;
    loadTrackOfStems (fixture);

    fixture.deck->toggleStemPad (1);
    fixture.deck->holdStemPad (2, true);
    fixture.deck->setStemGain (opendj::Stem::drums, 0.0f);
    fixture.deck->play();
    fixture.run (10);

    // The stems come back with the track, which is the point: nothing left over
    // from the last one may apply to them.
    loadTrackOfStems (fixture);

    REQUIRE (fixture.deck->getStemPad() == -1);
    REQUIRE_THAT (fixture.deck->getStemGain (opendj::Stem::drums), WithinAbs (1.0, 1.0e-6));

    // Nor is the hold left to come back when it lets go.
    fixture.deck->holdStemPad (2, false);
    REQUIRE (fixture.deck->getStemPad() == -1);

    fixture.deck->play();
    fixture.run (10);
    requireStemLevels (fixture.capture (100), stemToneLevel, stemToneLevel, stemToneLevel, stemToneLevel);
}

TEST_CASE ("a track loaded under a pad starts clean even if it never passes through silence", "[deck][stems][pads]")
{
    // Pressing play straight after a load means the deck never falls silent
    // between the two, which is the only other place the fades are put right.
    Fixture fixture, control;
    loadTrackOfStems (fixture);
    fixture.deck->toggleStemPad (1);
    fixture.deck->play();
    fixture.run (10);

    loadTrackOfStems (fixture, 0.8f);
    fixture.deck->play();
    const auto first = fixture.capture (1);

    // A deck that was already running and has just been sent to the start.
    loadTrackOfStems (control, 0.8f);
    control.deck->play();
    control.run (5);
    control.deck->seekToSeconds (0.0);
    const auto expected = control.capture (1);

    REQUIRE (sameSamples (first, expected));
}

TEST_CASE ("a pad pressed on a stopped deck is in place for the first sample", "[deck][stems][pads]")
{
    // The vocals are silent here, so anything heard under the acapella pad can
    // only be the mix, fading out over the start of the track.
    const auto keyShift = GENERATE (0, 2);
    INFO ("key shift " << keyShift);

    Fixture fixture;
    const auto numSamples = static_cast<int> (fixtureSeconds * 44100.0);
    const auto stems = makeSeparation (numSamples, 44100.0);
    stems->stems[(size_t) opendj::Stem::vocals].clear();
    fixture.deck->setSeparation (stems, fixture.deck->getLoadedFile());
    fixture.deck->setKeyShift (keyShift);
    fixture.run (2);

    fixture.deck->toggleStemPad (1);
    fixture.run (2);
    fixture.deck->play();

    for (const auto sample : fixture.capture (2))
        REQUIRE (std::abs (sample) < 1.0e-6f);
}

TEST_CASE ("moving between the mix and the stems does not step", "[deck][stems][pads]")
{
    // Stems a model made never quite sum to the mix, and a held pad crosses from
    // one to the other twice. Here they miss by a fifth. The size of a step
    // depends on where in the wave it lands, so the pad goes down and up at a run
    // of different places.
    const auto keyShift = GENERATE (0, 2);
    INFO ("key shift " << keyShift);

    Fixture withStems, control;
    loadTrackOfStems (withStems, 0.8f);
    loadTrackOfStems (control);
    withStems.deck->setKeyShift (keyShift);
    control.deck->setKeyShift (keyShift);
    withStems.deck->play();
    control.deck->play();
    withStems.run (40);
    control.run (40);

    REQUIRE (sameSamples (withStems.capture (2), control.capture (2)));

    std::vector<float> heard, undisturbed;

    const auto capturePair = [&] (int blocks)
    {
        const auto a = withStems.capture (blocks);
        const auto b = control.capture (blocks);
        heard.insert (heard.end(), a.begin(), a.end());
        undisturbed.insert (undisturbed.end(), b.begin(), b.end());
    };

    for (int round = 0; round < 12; ++round)
    {
        withStems.deck->holdStemPad (3, true);
        capturePair (11 + round % 3);
        withStems.deck->holdStemPad (3, false);
        capturePair (13 + round % 2);
    }

    // The pad did something, or the rest of this would mean nothing.
    auto largestDifference = 0.0f;

    for (size_t i = 0; i < heard.size(); ++i)
        largestDifference = juce::jmax (largestDifference, std::abs (heard[i] - undisturbed[i]));

    REQUIRE (largestDifference > 0.1f);

    // The tones bend slowly from one sample to the next, and a step does not, so
    // the bend shows one up wherever in the wave it landed.
    const auto bend = biggestBend (heard);
    INFO ("largest bend " << bend << ", against " << biggestBend (undisturbed) << " undisturbed");
    REQUIRE (bend < 0.01f);

    // On the direct path the undisturbed deck can be taken away to leave just
    // what the pad did. Not through the stretcher: any change to what goes in
    // sends its phases somewhere else for good, so the two soon differ by whole
    // tones, step or no step.
    if (keyShift == 0)
    {
        const auto step = biggestDifferenceStep (heard, undisturbed);
        INFO ("largest step against the undisturbed deck: " << step);
        REQUIRE (step < 0.02f);
    }
}

TEST_CASE ("a stem pad works through the stretcher", "[deck][stems][pads]")
{
    const auto shifted = std::pow (2.0, 2.0 / 12.0);

    Fixture fixture;
    loadTrackOfStems (fixture);
    fixture.deck->setKeyShift (2);
    fixture.deck->play();
    fixture.run (20);

    fixture.deck->toggleStemPad (2);
    fixture.run (40);

    const auto heard = fixture.capture (100);

    INFO ("drums " << shortWindowToneLevel (heard, 110.0 * shifted)
          << ", bass " << shortWindowToneLevel (heard, 220.0 * shifted)
          << ", other " << shortWindowToneLevel (heard, 440.0 * shifted)
          << ", vocals " << shortWindowToneLevel (heard, 880.0 * shifted));
    REQUIRE (shortWindowToneLevel (heard, 110.0 * shifted) > 0.05);
    REQUIRE (shortWindowToneLevel (heard, 220.0 * shifted) < 0.01);
    REQUIRE (shortWindowToneLevel (heard, 440.0 * shifted) < 0.01);
    REQUIRE (shortWindowToneLevel (heard, 880.0 * shifted) < 0.01);

    // Letting go is back to the mix, which is told from the stems by scaling
    // them down, and the fade has to have been let finish for that to be so.
    Fixture scaled;
    loadTrackOfStems (scaled, 0.25f);
    scaled.deck->setKeyShift (2);
    scaled.deck->play();
    scaled.deck->holdStemPad (2, true);
    scaled.run (40);
    scaled.deck->holdStemPad (2, false);
    scaled.run (60);

    const auto back = scaled.capture (100);
    INFO ("bass after letting go " << shortWindowToneLevel (back, 220.0 * shifted));
    REQUIRE (shortWindowToneLevel (back, 220.0 * shifted) > 0.1);
}

TEST_CASE ("pad presses from two threads at once are none of them lost", "[deck][stems][pads]")
{
    // An even number of presses in all has to end where it started. A read and a
    // write would drop some, and which way up it finishes is then a coin toss,
    // so it gets a few tosses.
    Fixture fixture;
    fixture.deck->setSeparation (makeSeparation (1024, 44100.0), fixture.deck->getLoadedFile());
    auto& deck = *fixture.deck;

    for (int round = 0; round < 10; ++round)
    {
        const auto presser = [&deck]
        {
            for (int i = 0; i < 100001; ++i)
                deck.toggleStemPad (1);
        };

        std::thread other (presser);
        presser();
        other.join();

        REQUIRE (deck.getStemPad() == -1);
    }
}

TEST_CASE ("stems finishing as the track goes do not land on what replaced it", "[deck][stems]")
{
    Fixture fixture;

    juce::AudioBuffer<float> briefly (2, 4410);
    briefly.clear();
    fixture.writeTrack (briefly, 44100.0, 16);
    fixture.deck->unload();

    const auto file = fixture.temporaryFile.getFile();
    const auto stems = makeSeparation (4410, 44100.0);
    auto& deck = *fixture.deck;

    std::atomic<bool> finished { false };

    // The three things that used to be able to walk over each other, with an
    // audio thread running under them so that retired stems really do get freed.
    std::thread worker ([&] { while (! finished) deck.setSeparation (stems, file); });
    std::thread cleaner ([&] { while (! finished) deck.cleanUp(); });
    std::thread audio ([&]
    {
        juce::AudioBuffer<float> block (2, 64);

        while (! finished)
            deck.processBlock (block);
    });

    auto leftBehind = 0;

    for (int i = 0; i < 300; ++i)
    {
        if (! deck.loadFile (file))
            break;

        deck.unload();

        // Long enough for a call that got past the check before the unload to
        // have finished, which is what would leave stems on an empty deck.
        for (int wait = 0; wait < 200; ++wait)
            std::this_thread::yield();

        if (deck.hasStems())
            ++leftBehind;
    }

    finished = true;
    worker.join();
    cleaner.join();
    audio.join();

    REQUIRE (leftBehind == 0);
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

//==============================================================================
// Auto gain
//==============================================================================

namespace
{
    /** A deck playing a tone written at the given level, its own file and all. */
    struct LevelledDeck
    {
        explicit LevelledDeck (float amplitude)
        {
            formatManager.registerBasicFormats();

            constexpr double rate = 48000.0;
            juce::AudioBuffer<float> content (2, (int) (6.0 * rate));
            const auto delta = juce::MathConstants<double>::twoPi * 997.0 / rate;

            for (int i = 0; i < content.getNumSamples(); ++i)
            {
                const auto value = (float) std::sin (delta * i) * amplitude;
                content.setSample (0, i, value);
                content.setSample (1, i, value);
            }

            juce::WavAudioFormat wav;
            auto stream = std::make_unique<juce::FileOutputStream> (file.getFile());
            REQUIRE (stream->openedOk());
            std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.release(), rate, 2, 24, {}, 0));
            REQUIRE (writer != nullptr);
            REQUIRE (writer->writeFromAudioSampleBuffer (content, 0, content.getNumSamples()));
            writer.reset();

            deck = std::make_unique<opendj::Deck> (0, formatManager);
            deck->prepare (deviceSampleRate, blockSize);
            REQUIRE (deck->loadFile (file.getFile()));
            buffer.setSize (2, blockSize);
        }

        float levelAfterSettling()
        {
            for (int b = 0; b < 40; ++b)
                deck->processBlock (buffer);

            return buffer.getRMSLevel (0, 0, blockSize);
        }

        juce::AudioFormatManager formatManager;
        juce::TemporaryFile file { ".wav" };
        std::unique_ptr<opendj::Deck> deck;
        juce::AudioBuffer<float> buffer;
    };
}

TEST_CASE ("auto gain brings tracks of different levels to the same level", "[deck][autogain]")
{
    // Ten dB apart as written.
    LevelledDeck loud (0.5f), quiet (0.158f);
    loud.deck->play();
    quiet.deck->play();

    const auto loudBefore = loud.levelAfterSettling();
    const auto quietBefore = quiet.levelAfterSettling();
    REQUIRE_THAT (juce::Decibels::gainToDecibels (loudBefore / quietBefore), WithinAbs (10.0, 0.3));

    loud.deck->setAutoGainEnabled (true);
    quiet.deck->setAutoGainEnabled (true);

    const auto loudAfter = loud.levelAfterSettling();
    const auto quietAfter = quiet.levelAfterSettling();

    INFO ("loud " << loudAfter << ", quiet " << quietAfter);
    REQUIRE_THAT (juce::Decibels::gainToDecibels (loudAfter / quietAfter), WithinAbs (0.0, 0.3));
}

TEST_CASE ("auto gain levels to the target itself", "[deck][autogain]")
{
    LevelledDeck deck (0.5f);
    deck.deck->setAutoGainEnabled (true);
    deck.deck->play();

    // A stereo sine's RMS is 3 dB under its peak, and -14 LUFS at 997 Hz has its
    // peak at -14 dBFS.
    const auto expected = juce::Decibels::decibelsToGain (-14.0f) / std::sqrt (2.0f);
    REQUIRE_THAT (deck.levelAfterSettling(), WithinAbs (expected, expected * 0.03f));
}

TEST_CASE ("with auto gain off the output is what it always was", "[deck][autogain]")
{
    LevelledDeck plain (0.5f), levelled (0.5f);
    REQUIRE (plain.deck->getTrackAutoGain() < 0.9f);

    plain.deck->play();
    levelled.deck->setAutoGainEnabled (true);
    levelled.deck->setAutoGainEnabled (false);
    levelled.deck->play();

    for (int b = 0; b < 40; ++b)
    {
        plain.deck->processBlock (plain.buffer);
        levelled.deck->processBlock (levelled.buffer);

        for (int i = 0; i < blockSize; ++i)
            REQUIRE (plain.buffer.getSample (0, i) == levelled.buffer.getSample (0, i));
    }
}

TEST_CASE ("the trim stays the DJ's, on top of auto gain", "[deck][autogain]")
{
    LevelledDeck deck (0.5f);
    deck.deck->setAutoGainEnabled (true);
    deck.deck->play();

    const auto level = deck.levelAfterSettling();
    REQUIRE (deck.deck->getTrim() == 1.0f);

    deck.deck->setTrim (0.5f);
    REQUIRE_THAT (deck.levelAfterSettling(), WithinAbs (level * 0.5f, level * 0.02f));
}

TEST_CASE ("turning auto gain on under a playing track does not click", "[deck][autogain]")
{
    LevelledDeck deck (0.5f);
    deck.deck->play();
    deck.levelAfterSettling();

    deck.deck->setAutoGainEnabled (true);

    // The tone's own largest step between samples.
    const auto toneStep = 0.5f * (float) (juce::MathConstants<double>::twoPi * 997.0 / deviceSampleRate);
    auto previous = deck.buffer.getSample (0, blockSize - 1);
    auto largest = 0.0f;

    for (int b = 0; b < 10; ++b)
    {
        deck.deck->processBlock (deck.buffer);

        for (int i = 0; i < blockSize; ++i)
        {
            largest = juce::jmax (largest, std::abs (deck.buffer.getSample (0, i) - previous));
            previous = deck.buffer.getSample (0, i);
        }
    }

    REQUIRE (largest < toneStep * 1.2f);
}

TEST_CASE ("a quiet track is lifted only as far as its peak allows", "[deck][autogain]")
{
    // With plenty of headroom, it gets all it asks for: this tone reads -20 LUFS.
    LevelledDeck roomy (0.1f);
    REQUIRE_THAT (roomy.deck->getTrackAutoGain(), WithinAbs (juce::Decibels::decibelsToGain (6.0f), 0.05f));

    // The library says this is 16 dB under the target, and the file peaks at 0.5,
    // which leaves 6 dB. A file that quiet with peaks that high is all transients.
    LevelledDeck peaky (0.5f);

    opendj::KnownTrack known;
    known.analysed = true;
    known.bpm = 120.0;
    known.loudnessLufs = -30.0;

    REQUIRE (peaky.deck->loadFile (peaky.file.getFile(), &known));
    REQUIRE_THAT (peaky.deck->getTrackAutoGain(), WithinAbs (2.0, 0.01));

    // And what comes out does not go over full scale.
    peaky.deck->setAutoGainEnabled (true);
    peaky.deck->play();
    peaky.levelAfterSettling();
    REQUIRE (peaky.buffer.getMagnitude (0, 0, blockSize) <= 1.01f);
}

TEST_CASE ("a known loudness is used rather than measured again", "[deck][autogain]")
{
    LevelledDeck deck (0.5f);

    opendj::KnownTrack known;
    known.analysed = true;
    known.bpm = 120.0;
    known.loudnessLufs = -14.0;

    REQUIRE (deck.deck->loadFile (deck.file.getFile(), &known));
    REQUIRE_THAT (deck.deck->getAnalysis()->loudnessLufs, WithinAbs (-14.0, 1.0e-9));
    REQUIRE_THAT (deck.deck->getTrackAutoGain(), WithinAbs (1.0, 1.0e-5));
}

TEST_CASE ("a track analysed before loudness existed is measured as it loads", "[deck][autogain]")
{
    LevelledDeck deck (0.5f);

    opendj::KnownTrack known;
    known.analysed = true;
    known.bpm = 120.0;

    REQUIRE (deck.deck->loadFile (deck.file.getFile(), &known));
    REQUIRE_THAT (deck.deck->getAnalysis()->loudnessLufs, WithinAbs (-6.0, 0.2));
}

//==============================================================================
// Stem effect
//==============================================================================

namespace
{
    constexpr double fxBeat = 0.4;      // deliberately not the deck's default
    constexpr int fxOneBeat = 2;        // the place of one beat in the divisions

    /** Loads a track and its stems, drawn a sample at a time by `draw (stem, seconds)`,
        with the mix their exact sum. For a test that needs a burst to time, which
        the four steady tones of makeSeparation do not have. */
    template <typename Drawing>
    std::shared_ptr<opendj::SeparatedTrack> loadDrawnStems (Fixture& fixture, double seconds, Drawing draw)
    {
        constexpr double rate = 44100.0;
        const auto numSamples = static_cast<int> (seconds * rate);

        auto stems = std::make_shared<opendj::SeparatedTrack>();
        stems->sampleRate = rate;

        juce::AudioBuffer<float> mix (2, numSamples);
        mix.clear();

        for (int stem = 0; stem < opendj::numStems; ++stem)
        {
            auto& buffer = stems->stems[(size_t) stem];
            buffer.setSize (2, numSamples);

            for (int i = 0; i < numSamples; ++i)
            {
                const auto value = draw (stem, i / rate);
                buffer.setSample (0, i, value);
                buffer.setSample (1, i, value);
            }

            for (int ch = 0; ch < 2; ++ch)
                mix.addFrom (ch, 0, buffer, ch, 0, numSamples);
        }

        fixture.writeTrack (mix, rate, 32);
        REQUIRE (fixture.deck->loadFile (fixture.temporaryFile.getFile()));

        fixture.deck->setSeparation (stems, fixture.deck->getLoadedFile());
        REQUIRE (fixture.deck->hasStems());
        return stems;
    }

    float sineAt (double frequency, double seconds, float level = 0.2f)
    {
        return level * static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * frequency * seconds));
    }

    constexpr double burstSeconds = 1.0, burstLength = 0.05;

    /** A short smooth burst of a tone, with nothing either side of it. */
    float burstAt (double frequency, double seconds)
    {
        if (seconds < burstSeconds || seconds >= burstSeconds + burstLength)
            return 0.0f;

        const auto window = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (seconds - burstSeconds) / burstLength);
        return static_cast<float> (window) * sineAt (frequency, seconds);
    }

    /** Vocals with one burst, and the other three stems steady tones to watch. */
    float vocalBurstOverTones (int stem, double seconds)
    {
        return stem == (int) opendj::Stem::vocals ? burstAt (880.0, seconds)
                                                  : sineAt (stemTones[stem], seconds);
    }

    /** Vocals with one burst, and nothing else, so its echoes can be timed
        through the stretcher, which would otherwise smear the tones over them. */
    float vocalBurstAlone (int stem, double seconds)
    {
        return stem == (int) opendj::Stem::vocals ? burstAt (880.0, seconds) : 0.0f;
    }

    float vocalToneAlone (int stem, double seconds)
    {
        return stem == (int) opendj::Stem::vocals ? sineAt (880.0, seconds) : 0.0f;
    }

    void setUpStemFx (opendj::Deck& deck, opendj::StemEffect::Type type, float amount,
                      opendj::Stem target = opendj::Stem::vocals)
    {
        deck.setStemFxTarget (target);
        deck.setStemFxType (type);
        deck.setStemFxBeats (fxOneBeat);
        deck.setStemFxBeatSeconds (fxBeat);
        deck.setStemFxAmount (amount);
    }

    std::vector<float> difference (const std::vector<float>& a, const std::vector<float>& b)
    {
        REQUIRE (a.size() == b.size());
        std::vector<float> out (a.size());

        for (size_t i = 0; i < a.size(); ++i)
            out[i] = a[i] - b[i];

        return out;
    }

    /** A stretch of a capture, by time from its start. */
    std::vector<float> slice (const std::vector<float>& samples, double from, double to)
    {
        const auto first = juce::jlimit<size_t> (0, samples.size(), (size_t) (from * deviceSampleRate));
        const auto last = juce::jlimit<size_t> (first, samples.size(), (size_t) (to * deviceSampleRate));
        return { samples.begin() + (std::ptrdiff_t) first, samples.begin() + (std::ptrdiff_t) last };
    }

    float peakOf (const std::vector<float>& samples)
    {
        auto peak = 0.0f;

        for (const auto sample : samples)
            peak = juce::jmax (peak, std::abs (sample));

        return peak;
    }

    double rmsOf (const std::vector<float>& samples)
    {
        double sum = 0.0;

        for (const auto sample : samples)
            sum += (double) sample * (double) sample;

        return std::sqrt (sum / (double) juce::jmax<size_t> (1, samples.size()));
    }

    /** How much there is in a band, as the level of the one tone that would be
        that much. Through the stretcher a low tone wanders a little either side of
        its frequency, by a different amount from one run of the input to the next,
        so that one bin at exactly that frequency can read anything from a fifth of
        the level to nearly all of it, on a dry deck too. The power stays in the
        band, so the band is what can be held to a decibel. */
    double bandLevel (const std::vector<float>& samples, double lowest, double highest)
    {
        constexpr size_t windowSize = 8192;
        constexpr double hannPowerPerTone = 1.5;    // the three bins a Hann window spreads a tone across
        const auto binSpacing = deviceSampleRate / (double) windowSize;

        double power = 0.0;
        auto windows = 0;

        for (size_t start = 0; start + windowSize <= samples.size(); start += windowSize, ++windows)
        {
            const std::vector<float> window (samples.begin() + (std::ptrdiff_t) start,
                                             samples.begin() + (std::ptrdiff_t) (start + windowSize));

            for (auto bin = std::ceil (lowest / binSpacing); bin * binSpacing <= highest; bin += 1.0)
            {
                const auto level = toneLevel (window, bin * binSpacing);
                power += level * level;
            }
        }

        return std::sqrt (power / hannPowerPerTone / juce::jmax (1, windows));
    }

    /** When the signal first gets above a level, in seconds, or -1 if it never does. */
    double firstAbove (const std::vector<float>& samples, float level)
    {
        for (size_t i = 0; i < samples.size(); ++i)
            if (std::abs (samples[i]) > level)
                return (double) i / deviceSampleRate;

        return -1.0;
    }

    /** The middle of a burst's energy, in seconds. A stretcher smears a burst's
        edges a good way either side but keeps them even, so this is steadier than
        looking for its start or its loudest sample. */
    double centreOfEnergy (const std::vector<float>& samples)
    {
        double weighted = 0.0, total = 0.0;

        for (size_t i = 0; i < samples.size(); ++i)
        {
            const auto energy = (double) samples[i] * (double) samples[i];
            weighted += energy * (double) i;
            total += energy;
        }

        return total > 0.0 ? weighted / total / deviceSampleRate : -1.0;
    }
}

TEST_CASE ("a stem effect never switched on leaves the deck to the last bit", "[deck][stems][stemfx]")
{
    const auto keyLocked = GENERATE (false, true);
    INFO ("key lock " << keyLocked);

    Fixture withChoices, control;
    loadTrackOfStems (withChoices, 0.8f);
    loadTrackOfStems (control, 0.8f);

    // Everything chosen but the switch, which is not to make any difference.
    setUpStemFx (*withChoices.deck, opendj::StemEffect::Type::reverb, 1.0f);

    for (auto* fixture : { &withChoices, &control })
    {
        fixture->deck->setKeyLock (keyLocked);
        fixture->deck->setTempoRatio (keyLocked ? 1.25 : 1.0);
        fixture->deck->play();
    }

    REQUIRE (sameSamples (withChoices.capture (100), control.capture (100)));
}

TEST_CASE ("a stem effect cannot be switched on with no stems", "[deck][stems][stemfx]")
{
    // Each way of switching it on, on a deck of its own: the second press on one
    // deck would put right whatever the first had got wrong.
    const auto setting = GENERATE (true, false);
    INFO ((setting ? "set on" : "toggled"));

    Fixture noStems, control;

    if (setting)
        noStems.deck->setStemFxEnabled (true);
    else
        noStems.deck->toggleStemFx();

    noStems.deck->play();
    control.deck->play();
    REQUIRE (sameSamples (noStems.capture (50), control.capture (50)));

    // And the switch has not been waiting all this time for the stems to arrive.
    noStems.deck->setSeparation (makeSeparation (44100 * 8, 44100.0), noStems.deck->getLoadedFile());
    REQUIRE_FALSE (noStems.deck->isStemFxEnabled());
}

TEST_CASE ("a vocal echo repeats at the beat and leaves the other stems alone", "[deck][stems][stemfx]")
{
    Fixture withEcho, control;
    loadDrawnStems (withEcho, 4.0, vocalBurstOverTones);
    loadDrawnStems (control, 4.0, vocalBurstOverTones);

    // Every knob at unity, so the deck is playing the mix and the stems are read
    // only for the effect. The echo must not depend on the mix path being off.
    setUpStemFx (*withEcho.deck, opendj::StemEffect::Type::echo, 0.5f);
    withEcho.deck->setStemFxEnabled (true);
    REQUIRE (withEcho.deck->isStemFxEnabled());

    withEcho.deck->play();
    control.deck->play();

    const auto blocks = blocksFor (3.0);
    const auto heard = withEcho.capture (blocks);
    const auto wet = difference (heard, control.capture (blocks));

    // Quiet until a beat after the burst began, and then the repeats are a beat
    // apart. Half level for the first, and less again for the second, at this
    // setting of the knob.
    REQUIRE (peakOf (slice (wet, 0.0, burstSeconds + fxBeat)) < 1.0e-5f);

    const auto first = slice (wet, burstSeconds + fxBeat, burstSeconds + 2.0 * fxBeat);
    const auto second = slice (wet, burstSeconds + 2.0 * fxBeat, burstSeconds + 3.0 * fxBeat);

    REQUIRE_THAT (peakOf (first), WithinAbs (0.2f * 0.5f, 0.01));
    REQUIRE_THAT (peakOf (second), WithinAbs (0.2f * 0.5f * 0.425f, 0.01));
    REQUIRE_THAT (centreOfEnergy (first), WithinAbs (burstLength * 0.5, 0.002));
    REQUIRE_THAT (centreOfEnergy (second), WithinAbs (burstLength * 0.5, 0.002));

    // All of what was added is the echo of the 880 Hz burst, none of it the other
    // stems, which come out at the level they went in.
    REQUIRE (toneLevel (slice (wet, burstSeconds + fxBeat, burstSeconds + fxBeat + burstLength), 880.0) > 0.02);

    for (int stem = 0; stem < 3; ++stem)
    {
        INFO (opendj::toString (static_cast<opendj::Stem> (stem)));
        REQUIRE (toneLevel (wet, stemTones[stem]) < 1.0e-5);
        REQUIRE_THAT (toneLevel (heard, stemTones[stem]), WithinAbs (stemToneLevel, 0.003));
    }
}

TEST_CASE ("a stem echo follows the target that was chosen", "[deck][stems][stemfx]")
{
    // Tones on everything, and the echo on the bass: all it can add is 220 Hz.
    Fixture withEcho, control;
    loadTrackOfStems (withEcho);
    loadTrackOfStems (control);

    setUpStemFx (*withEcho.deck, opendj::StemEffect::Type::echo, 0.5f, opendj::Stem::bass);
    withEcho.deck->setStemFxEnabled (true);
    withEcho.deck->play();
    control.deck->play();

    const auto wet = difference (withEcho.capture (blocksFor (2.0)), control.capture (blocksFor (2.0)));

    REQUIRE (toneLevel (wet, 220.0) > 0.05);
    REQUIRE (toneLevel (wet, 110.0) < 1.0e-3);
    REQUIRE (toneLevel (wet, 440.0) < 1.0e-3);
    REQUIRE (toneLevel (wet, 880.0) < 1.0e-3);
}

TEST_CASE ("a pad closes the way into a stem echo and the tail rings on", "[deck][stems][stemfx][pads]")
{
    const auto draw = [] (int stem, double seconds) { return sineAt (stemTones[stem], seconds); };

    Fixture withEcho, control;
    loadDrawnStems (withEcho, 8.0, draw);
    loadDrawnStems (control, 8.0, draw);

    setUpStemFx (*withEcho.deck, opendj::StemEffect::Type::echo, 0.5f);
    withEcho.deck->setStemFxEnabled (true);

    for (auto* fixture : { &withEcho, &control })
    {
        fixture->deck->play();
        fixture->run (blocksFor (1.0));
        fixture->deck->toggleStemPad (0);   // no vocals
        fixture->run (blocksFor (0.05));
    }

    // The dry vocal has gone, which is all the control has to say.
    REQUIRE (toneLevel (control.capture (blocksFor (0.2)), 880.0) < 1.0e-3);

    const auto ringing = withEcho.capture (blocksFor (0.2));
    INFO ("880 Hz in the tail: " << toneLevel (ringing, 880.0));
    REQUIRE (toneLevel (ringing, 880.0) > 0.03);

    // The others are still as they were, and the way in being shut, it dies.
    REQUIRE_THAT (toneLevel (ringing, 110.0), WithinAbs (stemToneLevel, 0.003));

    withEcho.run (blocksFor (5.0));
    REQUIRE (toneLevel (withEcho.capture (blocksFor (0.2)), 880.0) < 1.0e-3);
}

TEST_CASE ("a stem echo's repeats keep the time of the tempo", "[deck][stems][stemfx][keylock]")
{
    // With key lock the stretcher scales the spacing; without, the deck has to.
    // Either way the room should hear a beat at the tempo it is playing.
    const auto keyLocked = GENERATE (false, true);
    INFO ("key lock " << keyLocked);

    constexpr double tempo = 1.25;

    Fixture fixture;
    loadDrawnStems (fixture, 4.0, vocalBurstAlone);

    setUpStemFx (*fixture.deck, opendj::StemEffect::Type::echo, 0.5f);
    fixture.deck->setKeyLock (keyLocked);
    fixture.deck->setTempoRatio (tempo);
    fixture.deck->setStemFxEnabled (true);
    fixture.deck->play();

    const auto heard = fixture.capture (blocksFor (2.0));

    // The burst's place in the output depends on the stretcher's delay, so these
    // are found from the output, the burst and the repeat each by their middle.
    const auto around = [&heard] (double centre) { return slice (heard, centre - 0.14, centre + 0.14); };
    const auto expectedBurst = (burstSeconds + burstLength * 0.5) / tempo;
    const auto expectedSpacing = fxBeat / tempo;

    const auto burst = expectedBurst - 0.14 + centreOfEnergy (around (expectedBurst));
    const auto repeat = expectedBurst + expectedSpacing - 0.14 + centreOfEnergy (around (expectedBurst + expectedSpacing));

    INFO ("burst at " << burst << ", repeat at " << repeat);
    REQUIRE (peakOf (around (expectedBurst + expectedSpacing)) > 0.03f);
    REQUIRE_THAT (repeat - burst, WithinAbs (expectedSpacing, 0.01));
}

TEST_CASE ("a stem echo's repeats stay in key under key lock", "[deck][stems][stemfx][keylock]")
{
    Fixture fixture;
    loadDrawnStems (fixture, 8.0, vocalToneAlone);

    setUpStemFx (*fixture.deck, opendj::StemEffect::Type::echo, 0.5f);
    fixture.deck->setKeyLock (true);
    fixture.deck->setTempoRatio (1.25);
    fixture.deck->setStemFxEnabled (true);
    fixture.deck->play();
    fixture.run (blocksFor (1.0));

    fixture.deck->toggleStemPad (0);
    fixture.run (blocksFor (0.1));

    // Only the repeats left, and through the stretcher they are at the pitch the
    // stem was recorded at and not the 1100 Hz the tempo would have made them.
    const auto tail = fixture.capture (blocksFor (0.2));
    INFO ("880 Hz " << shortWindowToneLevel (tail, 880.0) << ", 1100 Hz " << shortWindowToneLevel (tail, 1100.0));
    REQUIRE (shortWindowToneLevel (tail, 880.0) > 0.02);
    REQUIRE (shortWindowToneLevel (tail, 1100.0) < shortWindowToneLevel (tail, 880.0) * 0.1);
}

TEST_CASE ("the stem low pass cuts the highs of its stem and only its stem", "[deck][stems][stemfx]")
{
    const auto keyLocked = GENERATE (false, true);
    INFO ("key lock " << keyLocked);

    // Both test tones in the vocals, so the one filter has to do both things, and
    // 220 Hz in the drums, which would lose level if they were filtered too.
    const auto draw = [] (int stem, double seconds)
    {
        if (stem == (int) opendj::Stem::vocals)
            return sineAt (110.0, seconds) + sineAt (880.0, seconds);

        return stem == (int) opendj::Stem::drums ? sineAt (220.0, seconds) : 0.0f;
    };

    Fixture filtered, control;
    loadDrawnStems (filtered, 6.0, draw);
    loadDrawnStems (control, 6.0, draw);

    setUpStemFx (*filtered.deck, opendj::StemEffect::Type::lowPass, 1.0f);
    filtered.deck->setStemFxEnabled (true);

    for (auto* fixture : { &filtered, &control })
    {
        fixture->deck->setKeyLock (keyLocked);
        fixture->deck->setTempoRatio (keyLocked ? 1.25 : 1.0);
        fixture->deck->play();
        fixture->run (blocksFor (0.5));
    }

    const auto heard = filtered.capture (blocksFor (2.0));
    const auto dry = control.capture (blocksFor (2.0));

    const auto decibelsIn = [&heard, &dry] (double lowest, double highest)
    {
        return juce::Decibels::gainToDecibels (bandLevel (heard, lowest, highest)
                                               / bandLevel (dry, lowest, highest));
    };

    const auto vocalTop = decibelsIn (700.0, 1100.0);
    const auto vocalBody = decibelsIn (70.0, 155.0);
    const auto drums = decibelsIn (180.0, 270.0);

    INFO ("880 Hz " << vocalTop << " dB, 110 Hz " << vocalBody << " dB, 220 Hz " << drums << " dB");

    // Both were there to begin with, or there would be nothing to compare with.
    REQUIRE_THAT (bandLevel (dry, 700.0, 1100.0), WithinAbs (0.2, 0.03));
    REQUIRE_THAT (bandLevel (dry, 70.0, 155.0), WithinAbs (0.2, 0.03));

    REQUIRE (vocalTop < -20.0);
    REQUIRE_THAT (vocalBody, WithinAbs (0.0, 1.0));
    REQUIRE_THAT (drums, WithinAbs (0.0, 1.0));
}

TEST_CASE ("a stem reverb rings after the stem has stopped", "[deck][stems][stemfx]")
{
    Fixture withReverb, control;
    loadDrawnStems (withReverb, 4.0, vocalBurstAlone);
    loadDrawnStems (control, 4.0, vocalBurstAlone);

    setUpStemFx (*withReverb.deck, opendj::StemEffect::Type::reverb, 1.0f);
    withReverb.deck->setStemFxEnabled (true);
    withReverb.deck->play();
    control.deck->play();

    const auto blocks = blocksFor (3.0);
    const auto wet = difference (withReverb.capture (blocks), control.capture (blocks));

    // The burst has gone by here, and what is left is the room.
    const auto end = burstSeconds + burstLength;
    REQUIRE (peakOf (slice (wet, 0.0, burstSeconds)) < 1.0e-5f);
    REQUIRE (rmsOf (slice (wet, end, end + 0.2)) > 1.0e-3);
    REQUIRE (rmsOf (slice (wet, end + 1.5, end + 1.7)) < rmsOf (slice (wet, end, end + 0.2)));
}

TEST_CASE ("the knob at zero is no stem effect at all", "[deck][stems][stemfx]")
{
    using Type = opendj::StemEffect::Type;
    const auto type = GENERATE (Type::echo, Type::reverb, Type::lowPass);
    INFO ("type " << (int) type);

    Fixture withEffect, control;
    loadTrackOfStems (withEffect);
    loadTrackOfStems (control);

    setUpStemFx (*withEffect.deck, type, 0.0f);
    withEffect.deck->setStemFxEnabled (true);
    withEffect.deck->play();
    control.deck->play();

    REQUIRE (sameSamples (withEffect.capture (100), control.capture (100)));
}

TEST_CASE ("after a stem effect's tail has died the deck is back to the last bit", "[deck][stems][stemfx]")
{
    using Type = opendj::StemEffect::Type;
    const auto type = GENERATE (Type::echo, Type::reverb, Type::lowPass);
    INFO ("type " << (int) type);

    Fixture withEffect, control;
    loadDrawnStems (withEffect, 16.0, vocalBurstOverTones);
    loadDrawnStems (control, 16.0, vocalBurstOverTones);

    setUpStemFx (*withEffect.deck, type, 0.5f);
    withEffect.deck->setStemFxEnabled (true);
    withEffect.deck->play();
    control.deck->play();

    // There has to have been something to come back from.
    const auto during = blocksFor (burstSeconds + 1.0);
    REQUIRE (peakOf (difference (withEffect.capture (during), control.capture (during))) > 0.01f);

    withEffect.deck->setStemFxEnabled (false);

    const auto dying = blocksFor (12.0);
    withEffect.run (dying);
    control.run (dying);

    REQUIRE (sameSamples (withEffect.capture (100), control.capture (100)));
}

TEST_CASE ("after a stem echo's tail has died under key lock the deck sounds as it did", "[deck][stems][stemfx][keylock]")
{
    // What the stretcher has been through stays in its phases for good, so this
    // cannot be compared to the bit. It has to be the same sound, with no echo in.
    const auto draw = [] (int stem, double seconds)
    {
        return stem == (int) opendj::Stem::vocals ? burstAt (880.0, seconds)
                                                  : (stem == (int) opendj::Stem::drums ? sineAt (110.0, seconds) : 0.0f);
    };

    Fixture withEffect, control;
    loadDrawnStems (withEffect, 16.0, draw);
    loadDrawnStems (control, 16.0, draw);

    setUpStemFx (*withEffect.deck, opendj::StemEffect::Type::echo, 0.5f);
    withEffect.deck->setStemFxEnabled (true);

    for (auto* fixture : { &withEffect, &control })
    {
        fixture->deck->setKeyLock (true);
        fixture->deck->setTempoRatio (1.25);
        fixture->deck->play();
    }

    const auto during = blocksFor (2.0);
    const auto repeatAt = (burstSeconds + burstLength * 0.5 + fxBeat) / 1.25;
    const auto wetRepeat = slice (withEffect.capture (during), repeatAt - 0.1, repeatAt + 0.1);
    const auto dryRepeat = slice (control.capture (during), repeatAt - 0.1, repeatAt + 0.1);

    INFO ("880 Hz where the repeat should be: " << toneLevel (wetRepeat, 880.0)
          << " against " << toneLevel (dryRepeat, 880.0));
    REQUIRE (toneLevel (wetRepeat, 880.0) > 10.0 * juce::jmax (1.0e-4, toneLevel (dryRepeat, 880.0)));

    withEffect.deck->setStemFxEnabled (false);

    const auto dying = blocksFor (8.0);
    withEffect.run (dying);
    control.run (dying);

    const auto heard = withEffect.capture (100);
    const auto dry = control.capture (100);

    INFO ("rms " << rmsOf (heard) << " against " << rmsOf (dry));
    REQUIRE_THAT (rmsOf (heard), WithinAbs (rmsOf (dry), 0.02 * rmsOf (dry)));
    REQUIRE_THAT (shortWindowToneLevel (heard, 110.0), WithinAbs (shortWindowToneLevel (dry, 110.0), 0.003));
    REQUIRE (shortWindowToneLevel (heard, 880.0) < 1.0e-3);
}

TEST_CASE ("a new track starts with no tail from the last", "[deck][stems][stemfx]")
{
    const auto wholeFade = GENERATE (false, true);
    INFO ("left to fall silent first " << wholeFade);

    Fixture fixture, control;
    loadTrackOfStems (fixture);
    setUpStemFx (*fixture.deck, opendj::StemEffect::Type::echo, 1.0f);
    fixture.deck->setStemFxEnabled (true);
    fixture.deck->play();
    fixture.run (blocksFor (1.0));

    // A load pauses the deck. Pressing play at once, inside the five millisecond
    // fade, means it never reaches the silence that would otherwise have cleared
    // the effect, so it is left to the load to ask for it.
    REQUIRE (fixture.deck->loadFile (fixture.temporaryFile.getFile()));
    REQUIRE_FALSE (fixture.deck->isStemFxEnabled());

    if (wholeFade)
        fixture.run (5);

    fixture.deck->play();
    const auto heard = fixture.capture (blocksFor (1.0));

    // A deck already running, sent to the start, is where the first plays from.
    REQUIRE (control.deck->loadFile (fixture.temporaryFile.getFile()));
    control.deck->play();

    if (! wholeFade)
    {
        control.run (5);
        control.deck->seekToSeconds (0.0);
    }

    REQUIRE (sameSamples (heard, control.capture (blocksFor (1.0))));

    // And the choice is not made again for the stems of the new one on arrival.
    fixture.deck->setSeparation (makeSeparation (44100 * 8, 44100.0), fixture.deck->getLoadedFile());
    REQUIRE_FALSE (fixture.deck->isStemFxEnabled());
}

TEST_CASE ("a tail does not wait through a pause to come back on the next play", "[deck][stems][stemfx]")
{
    Fixture uninterrupted, paused, control;
    loadDrawnStems (uninterrupted, 4.0, vocalBurstOverTones);
    loadDrawnStems (paused, 4.0, vocalBurstOverTones);
    loadDrawnStems (control, 4.0, vocalBurstOverTones);

    setUpStemFx (*uninterrupted.deck, opendj::StemEffect::Type::echo, 0.5f);
    setUpStemFx (*paused.deck, opendj::StemEffect::Type::echo, 0.5f);
    uninterrupted.deck->setStemFxEnabled (true);
    paused.deck->setStemFxEnabled (true);

    const auto toPause = blocksFor (burstSeconds + burstLength + 0.05);

    for (auto* fixture : { &uninterrupted, &paused, &control })
    {
        fixture->deck->play();
        fixture->run (toPause);
    }

    // The burst is in the line of both, and left alone it comes back.
    const auto after = blocksFor (1.0);
    const auto stillThere = uninterrupted.capture (after);

    for (auto* fixture : { &paused, &control })
    {
        fixture->deck->pause();
        fixture->run (10);
        fixture->deck->play();
    }

    const auto dry = control.capture (after);

    // Measured on the vocals alone, which are over by now but for the repeats:
    // the pause has put the control's tones out of phase with the deck left
    // playing, so a plain difference would be large with no echo at all.
    const auto firstRepeat = burstSeconds + fxBeat - toPause * (double) blockSize / deviceSampleRate;
    const auto repeatOf = [firstRepeat] (const std::vector<float>& samples)
    {
        return toneLevel (slice (samples, firstRepeat - 0.02, firstRepeat + burstLength + 0.02), 880.0);
    };

    INFO ("repeat left alone " << repeatOf (stillThere) << ", on the control " << repeatOf (dry));
    REQUIRE (repeatOf (stillThere) > 0.02);
    REQUIRE (repeatOf (dry) < 0.002);

    REQUIRE (sameSamples (paused.capture (after), dry));
}

TEST_CASE ("a stem effect delay stays in the line at the slowest tempo", "[deck][stems][stemfx]")
{
    using opendj::StemEffect;

    // The slowest beat a grid can be given, the longest division and the slowest
    // the deck can go.
    constexpr auto longestDivision = StemEffect::numDivisions - 1;
    const auto slowestBeat = 60.0 / opendj::Deck::minimumGridBpm;

    REQUIRE_THAT (StemEffect::delaySecondsFor (slowestBeat, longestDivision, 0.05, false),
                  WithinAbs (StemEffect::longestDelaySeconds, 1.0e-9));

    Fixture withEcho, control;
    loadDrawnStems (withEcho, 8.0, vocalToneAlone);
    loadDrawnStems (control, 8.0, vocalToneAlone);

    setUpStemFx (*withEcho.deck, StemEffect::Type::echo, 0.5f);
    withEcho.deck->setStemFxBeats (longestDivision);
    withEcho.deck->setStemFxBeatSeconds (slowestBeat);
    withEcho.deck->setStemFxEnabled (true);

    for (auto* fixture : { &withEcho, &control })
    {
        fixture->deck->setTempoRatio (0.05);
        fixture->deck->play();
    }

    const auto onFor = blocksFor (1.0);
    auto wet = difference (withEcho.capture (onFor), control.capture (onFor));

    // Off long before the first repeat has come round. It must still come: this
    // is the longest silence a tail can go through and not be over.
    withEcho.deck->setStemFxEnabled (false);

    const auto later = blocksFor (4.5);
    const auto rest = difference (withEcho.capture (later), control.capture (later));
    wet.insert (wet.end(), rest.begin(), rest.end());

    INFO ("first repeat at " << firstAbove (wet, 1.0e-4f));
    REQUIRE_THAT (firstAbove (wet, 1.0e-4f), WithinAbs (StemEffect::longestDelaySeconds, 0.05));
    REQUIRE (peakOf (slice (wet, 4.2, 5.0)) > 0.02f);
}

TEST_CASE ("the stem effect's lengths are what they say", "[deck][stems][stemfx]")
{
    using opendj::StemEffect;

    REQUIRE (StemEffect::getDivisionName (0) == "1/4");
    REQUIRE (StemEffect::getDivisionName (StemEffect::numDivisions - 1) == "2");

    // Half a second a beat. At the fader off the stretcher, at the record on it,
    // since the stretcher does the fader's share itself.
    REQUIRE_THAT (StemEffect::delaySecondsFor (0.5, 0, 1.0, false), WithinAbs (0.125, 1.0e-9));
    REQUIRE_THAT (StemEffect::delaySecondsFor (0.5, 2, 1.25, false), WithinAbs (0.4, 1.0e-9));
    REQUIRE_THAT (StemEffect::delaySecondsFor (0.5, 2, 1.25, true), WithinAbs (0.5, 1.0e-9));
    REQUIRE_THAT (StemEffect::delaySecondsFor (0.5, 3, 1.0, false), WithinAbs (1.0, 1.0e-9));
}

TEST_CASE ("a stem effect reads a short stem only as far as it goes", "[deck][stems][stemfx]")
{
    // As with the stems playing dry, the left of a short buffer runs straight
    // into its right, so that reading on past the end would feed the echo.
    Fixture fixture, control;
    constexpr auto stemSamples = 44100;

    fixture.deck->setSeparation (makeSeparation (stemSamples, 44100.0), fixture.deck->getLoadedFile());
    setUpStemFx (*fixture.deck, opendj::StemEffect::Type::echo, 0.5f);
    fixture.deck->setStemFxEnabled (true);

    fixture.deck->play();
    control.deck->play();

    const auto blocks = blocksFor (7.5);
    const auto wet = difference (fixture.capture (blocks), control.capture (blocks));

    // Repeats while there is a stem and for as long as they last, then nothing.
    REQUIRE (peakOf (slice (wet, 0.5, 1.0)) > 0.05f);
    REQUIRE (peakOf (slice (wet, 5.0, 7.5)) < 1.0e-4f);
}

TEST_CASE ("switching a stem effect or changing what it does is not heard as a click", "[deck][stems][stemfx]")
{
    using Type = opendj::StemEffect::Type;

    // Pressed at several places in the tone, so that some press lands where a cut
    // would be at its worst: a single one can land where it would not show.
    auto worst = 0.0f;

    for (int offset = 0; offset < 8; ++offset)
    {
        const auto draw = [] (int stem, double seconds) { return sineAt (stemTones[stem], seconds); };

        Fixture fixture, control;
        loadDrawnStems (fixture, 2.0, draw);
        loadDrawnStems (control, 2.0, draw);

        setUpStemFx (*fixture.deck, Type::lowPass, 1.0f);
        fixture.deck->play();
        control.deck->play();

        const auto settle = 20 + offset;
        fixture.run (settle);
        control.run (settle);

        const auto listen = [&fixture, &control, &worst]
        {
            const auto heard = difference (fixture.capture (10), control.capture (10));
            worst = juce::jmax (worst, biggestDifferenceStep (heard, std::vector<float> (heard.size(), 0.0f)));
            return heard;
        };

        // It really came on, or there is nothing here to switch. The knob is all
        // the way, so most of the vocals' 880 Hz is taken away.
        fixture.deck->setStemFxEnabled (true);
        const auto cut = toneLevel (slice (listen(), 0.05, 0.1), 880.0);
        INFO ("880 Hz taken away " << cut);
        REQUIRE (cut > 0.1);
        fixture.deck->setStemFxTarget (opendj::Stem::other);
        listen();
        fixture.deck->setStemFxType (Type::echo);
        listen();
        fixture.deck->setStemFxType (Type::lowPass);
        listen();
        fixture.deck->setStemFxEnabled (false);
        listen();
    }

    // The steepest an 880 Hz tone of this level can go by itself is 0.023 a
    // sample, and a fade over that is well under it. A cut could be 0.2.
    INFO ("biggest step " << worst);
    REQUIRE (worst < 0.03f);
}

TEST_CASE ("switching a stem effect off shuts the way in and lets the tail ring out", "[deck][stems][stemfx]")
{
    using Type = opendj::StemEffect::Type;
    const auto type = GENERATE (Type::echo, Type::reverb);
    const auto channel = GENERATE (0, 1);
    INFO ("type " << (int) type << ", channel " << channel);

    // A tone that never stops, so that a way in left open cannot go unnoticed: a
    // burst long gone by the time of the switch would have nothing left to send.
    Fixture withEffect, control;
    loadDrawnStems (withEffect, 16.0, vocalToneAlone);
    loadDrawnStems (control, 16.0, vocalToneAlone);

    setUpStemFx (*withEffect.deck, type, 0.5f);
    withEffect.deck->setStemFxEnabled (true);
    withEffect.deck->play();
    control.deck->play();

    const auto wetFor = [&withEffect, &control, channel] (double seconds)
    {
        const auto blocks = blocksFor (seconds);
        return difference (withEffect.capture (blocks, channel), control.capture (blocks, channel));
    };

    const auto ringingOn = wetFor (1.0);
    REQUIRE (toneLevel (slice (ringingOn, 0.8, 1.0), 880.0) > 0.03);

    withEffect.deck->setStemFxEnabled (false);

    // Still there straight after, and not cut off with the switch...
    REQUIRE (rmsOf (wetFor (0.1)) > 0.01);

    // ...and then gone, with the tone still going in underneath it.
    wetFor (4.0);
    REQUIRE (rmsOf (wetFor (0.5)) < 1.0e-3);

    wetFor (6.0);
    const auto blocks = blocksFor (1.0);
    REQUIRE (sameSamples (withEffect.capture (blocks, channel), control.capture (blocks, channel)));
}

TEST_CASE ("a stem effect keeps the left and the right apart", "[deck][stems][stemfx]")
{
    // The reverb is left out: it is a mono send into a stereo room, and is meant
    // to take from both.
    using Type = opendj::StemEffect::Type;
    const auto type = GENERATE (Type::echo, Type::lowPass);
    const auto channel = GENERATE (0, 1);
    INFO ("type " << (int) type << ", channel " << channel);

    constexpr double toneOf[2] { 880.0, 1320.0 };

    Fixture withEffect, control;
    loadDrawnStems (control, 4.0, vocalToneAlone);

    auto stems = loadDrawnStems (withEffect, 4.0, vocalToneAlone);
    auto& vocals = stems->stems[(size_t) opendj::Stem::vocals];

    for (int i = 0; i < vocals.getNumSamples(); ++i)
        vocals.setSample (1, i, sineAt (toneOf[1], i / 44100.0));

    setUpStemFx (*withEffect.deck, type, type == Type::echo ? 0.5f : 1.0f);
    withEffect.deck->setStemFxEnabled (true);
    withEffect.deck->play();
    control.deck->play();

    const auto blocks = blocksFor (2.0);
    const auto wet = slice (difference (withEffect.capture (blocks, channel), control.capture (blocks, channel)), 1.0, 2.0);

    INFO (toneOf[0] << " Hz " << toneLevel (wet, toneOf[0]) << ", " << toneOf[1] << " Hz " << toneLevel (wet, toneOf[1]));
    REQUIRE (toneLevel (wet, toneOf[channel]) > 0.05);
    REQUIRE (toneLevel (wet, toneOf[1 - channel]) < 1.0e-3);
}

TEST_CASE ("the deck's level goes over what a stem effect adds", "[deck][stems][stemfx]")
{
    const auto firstRepeatAt = [] (float trim)
    {
        Fixture withEcho, control;
        loadDrawnStems (withEcho, 4.0, vocalBurstAlone);
        loadDrawnStems (control, 4.0, vocalBurstAlone);

        setUpStemFx (*withEcho.deck, opendj::StemEffect::Type::echo, 0.5f);
        withEcho.deck->setStemFxEnabled (true);

        for (auto* fixture : { &withEcho, &control })
        {
            fixture->deck->setTrim (trim);
            fixture->deck->play();
        }

        const auto blocks = blocksFor (2.0);
        const auto wet = difference (withEcho.capture (blocks), control.capture (blocks));
        return (double) peakOf (slice (wet, burstSeconds + fxBeat, burstSeconds + 2.0 * fxBeat));
    };

    // A trim turned down would otherwise leave the echo louder than its source.
    const auto full = firstRepeatAt (1.0f);
    REQUIRE (full > 0.05);
    REQUIRE_THAT (firstRepeatAt (0.5f), WithinAbs (full * 0.5, full * 0.02));
}

TEST_CASE ("a stem effect that has finished stops being run", "[deck][stems][stemfx]")
{
    using opendj::StemEffect;

    constexpr double rate = 44100.0;
    constexpr int block = 512;

    const auto secondsUntilItStops = [] (StemEffect& effect, StemEffect::Settings settings)
    {
        float left = 0.0f, right = 0.0f;

        // A second of something going in, and then the switch goes off.
        settings.on = true;

        for (int i = 0; i < (int) rate; )
        {
            REQUIRE (effect.beginBlock (settings));

            for (int frame = 0; frame < block; ++frame, ++i)
                effect.tick (0.2f, 0.2f, left, right);
        }

        settings.on = false;
        auto heardAnything = false;
        juce::int64 frames = 0;

        while (effect.beginBlock (settings))
        {
            REQUIRE (frames < (juce::int64) (rate * 120.0));

            for (int frame = 0; frame < block; ++frame, ++frames)
            {
                effect.tick (0.0f, 0.0f, left, right);
                heardAnything = heardAnything || std::abs (left) > 1.0e-3f;
            }
        }

        REQUIRE_FALSE (effect.isRinging());
        return std::pair { (double) frames / rate, heardAnything };
    };

    StemEffect::Settings settings;
    settings.amount = 0.5f;
    settings.delaySeconds = 0.25;

    StemEffect effect;
    effect.prepare (rate);

    // A tail is run for as long as it lasts, and for longer than the longest
    // silence between two repeats, as it cannot tell that from the end.
    settings.type = StemEffect::Type::echo;
    const auto [echoSeconds, echoHeard] = secondsUntilItStops (effect, settings);
    REQUIRE (echoHeard);
    REQUIRE (echoSeconds > StemEffect::longestDelaySeconds);
    REQUIRE (echoSeconds < 15.0);

    // And what it stopped with is gone: a second start does not hear the first.
    settings.on = true;
    REQUIRE (effect.beginBlock (settings));

    for (int frame = 0; frame < block; ++frame)
    {
        float left = 1.0f, right = 1.0f;
        effect.tick (0.0f, 0.0f, left, right);
        REQUIRE (std::abs (left) < 1.0e-9f);
        REQUIRE (std::abs (right) < 1.0e-9f);
    }

    effect.reset();

    settings.type = StemEffect::Type::reverb;
    const auto [reverbSeconds, reverbHeard] = secondsUntilItStops (effect, settings);
    REQUIRE (reverbHeard);
    REQUIRE (reverbSeconds > StemEffect::longestDelaySeconds);
    REQUIRE (reverbSeconds < 30.0);

    // A filter has nothing to ring, so it is done as soon as it has faded out.
    settings.type = StemEffect::Type::lowPass;
    REQUIRE (secondsUntilItStops (effect, settings).first < 0.1);
}

TEST_CASE ("a stem effect stays finite through everything a deck can do", "[deck][stems][stemfx]")
{
    using Type = opendj::StemEffect::Type;
    const auto type = GENERATE (Type::echo, Type::reverb, Type::lowPass);
    const auto keyLocked = GENERATE (false, true);
    INFO ("type " << (int) type << ", key lock " << keyLocked);

    Fixture fixture;
    loadTrackOfStems (fixture);
    setUpStemFx (*fixture.deck, type, 1.0f);
    fixture.deck->setKeyLock (keyLocked);
    fixture.deck->setTempoRatio (1.25);
    fixture.deck->setStemFxEnabled (true);
    fixture.deck->play();

    auto loudest = 0.0f;

    const auto listen = [&fixture, &loudest] (int blocks)
    {
        for (const auto sample : fixture.capture (blocks))
        {
            REQUIRE (std::isfinite (sample));
            loudest = juce::jmax (loudest, std::abs (sample));
        }
    };

    listen (blocksFor (0.5));

    fixture.deck->setReverse (true);
    listen (blocksFor (0.2));
    fixture.deck->setReverse (false);

    fixture.deck->seekToSeconds (6.0);
    listen (blocksFor (0.2));

    fixture.deck->setJogTouched (true);
    fixture.deck->addJogTicks (-200.0);
    listen (blocksFor (0.1));
    fixture.deck->setJogTouched (false);

    fixture.deck->brake();
    listen (blocksFor (1.2));

    fixture.deck->play();
    fixture.deck->seekToSeconds (7.9);      // and off the end of the track
    listen (blocksFor (0.5));

    // Four tones at 0.2, and repeats that add up to no more than six times one
    // stem with the feedback at its highest.
    REQUIRE (loudest < 3.0f);
}

TEST_CASE ("leaving the stretcher does not put the same stem into its echo twice", "[deck][stems][stemfx][keylock]")
{
    // The stretcher is fed ahead of what is heard, and the stem effect with it.
    // When the stretcher stops, the deck goes back to reading where the room has
    // got to, and that stretch of the stem has already gone into the echo once.
    // Here a short burst sits in that stretch: it should come back once, a beat
    // later, and not twice.
    constexpr double burstAtSeconds = 1.0, burstSeconds = 0.01;

    const auto draw = [] (int stem, double seconds)
    {
        if (stem != (int) opendj::Stem::vocals || seconds < burstAtSeconds || seconds >= burstAtSeconds + burstSeconds)
            return 0.0f;

        const auto window = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (seconds - burstAtSeconds) / burstSeconds);
        return static_cast<float> (window) * sineAt (880.0, seconds, 0.5f);
    };

    Fixture fixture;
    loadDrawnStems (fixture, 3.0, draw);
    setUpStemFx (*fixture.deck, opendj::StemEffect::Type::echo, 0.5f);
    fixture.deck->setStemFxEnabled (true);
    fixture.deck->setKeyLock (true);
    fixture.deck->setTempoRatio (1.02);

    fixture.deck->seekToSeconds (burstAtSeconds - 0.02);
    fixture.deck->play();
    fixture.run (1);

    // Stretched for one block: the feed is past the burst, the room is not.
    const auto from = fixture.deck->getPositionSeconds();
    REQUIRE (from < burstAtSeconds);

    fixture.deck->setTempoRatio (1.0);
    const auto heard = fixture.capture (blocksFor (0.8));

    // At the recorded tempo, a sample out is a sample of the track.
    const auto at = [from] (double trackSeconds) { return trackSeconds - from; };
    const auto dry = slice (heard, at (burstAtSeconds) - 0.005, at (burstAtSeconds + burstSeconds) + 0.005);
    const auto repeats = slice (heard, at (burstAtSeconds + fxBeat) - 0.25, at (burstAtSeconds + fxBeat) + 0.05);

    const auto energy = [] (const std::vector<float>& samples)
    {
        double sum = 0.0;

        for (const auto sample : samples)
            sum += (double) sample * (double) sample;

        return sum;
    };

    // At half the amount, one repeat has a quarter of the burst's energy.
    const auto ratio = energy (repeats) / energy (dry);
    INFO ("repeats have " << ratio << " of the burst's energy");
    REQUIRE (energy (dry) > 0.0);
    REQUIRE (ratio > 0.15);
    REQUIRE (ratio < 0.35);
}

TEST_CASE ("pressing the lit pad lets go of a hold whose release never came", "[deck][stems][pads]")
{
    // A release can go missing: shift down when the pad comes up sends it
    // somewhere else, and so does a controller unplugged with a pad held. The
    // screen can only press, so a press of the pad that is lit has to be enough.
    Fixture fixture;
    loadTrackOfStems (fixture);
    auto& deck = *fixture.deck;

    deck.holdStemPad (2, true);
    REQUIRE (deck.getStemPad() == 2);

    deck.toggleStemPad (2);
    REQUIRE (deck.getStemPad() == -1);

    deck.play();
    fixture.run (20);
    requireStemLevels (fixture.capture (20), stemToneLevel, stemToneLevel, stemToneLevel, stemToneLevel);

    // A hold over a latched pad falls back to the latched one, as letting go would.
    deck.toggleStemPad (1);
    deck.holdStemPad (3, true);
    deck.toggleStemPad (3);
    REQUIRE (deck.getStemPad() == 1);

    // The release arriving late after all changes nothing.
    deck.holdStemPad (3, false);
    REQUIRE (deck.getStemPad() == 1);
}

TEST_CASE ("a stem echo forgets everything at once, however far round its line has gone", "[deck][stems][stemfx]")
{
    // Forgetting is a count rather than a clear of seconds of memory, which is
    // only right if nothing written before it can be read after, wherever the
    // line had got to and whatever the delay is when it starts again.
    using opendj::StemEffect;

    constexpr double rate = 44100.0;
    constexpr int block = 512;
    const auto seconds = GENERATE (0.3, StemEffect::longestDelaySeconds * 2.5);
    INFO ("written for " << seconds << " s first");

    StemEffect effect;
    effect.prepare (rate);

    StemEffect::Settings settings;
    settings.on = true;
    settings.type = StemEffect::Type::echo;
    settings.amount = 0.9f;
    settings.delaySeconds = 0.25;

    float left = 0.0f, right = 0.0f;
    auto heard = 0.0f;

    for (int i = 0; i < (int) (seconds * rate); )
    {
        effect.beginBlock (settings);

        for (int frame = 0; frame < block; ++frame, ++i)
        {
            effect.tick (0.3f, -0.3f, left, right);
            heard = juce::jmax (heard, std::abs (left));
        }
    }

    REQUIRE (heard > 0.1f);
    effect.reset();

    // Every delay the line can hold, glided through, and nothing going in.
    settings.on = false;
    settings.delaySeconds = StemEffect::longestDelaySeconds;
    auto afterwards = 0.0f;

    for (int i = 0; i < (int) ((StemEffect::longestDelaySeconds + 0.5) * rate) && effect.beginBlock (settings); )
    {
        for (int frame = 0; frame < block; ++frame, ++i)
        {
            effect.tick (0.0f, 0.0f, left, right);
            afterwards = juce::jmax (afterwards, juce::jmax (std::abs (left), std::abs (right)));
        }

        settings.delaySeconds = juce::jmax (0.1, settings.delaySeconds - 0.05);
    }

    REQUIRE (juce::exactlyEqual (afterwards, 0.0f));
}
