/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>

#include "analysis/TrackDecoder.h"

#include <memory>

namespace
{
    constexpr double fakeSampleRate = 44100.0;

    /** A reader that claims more samples than it will hand over.

        This is exactly what a real MP3 reader does: the length is worked out
        from the frame headers, and for a file whose last frame is short or
        padded the estimate overshoots what the decoder can produce. Reproducing
        it here rather than shipping an MP3 fixture keeps the test honest about
        what it is testing and portable to any machine. */
    class ShortReader final : public juce::AudioFormatReader
    {
    public:
        ShortReader (juce::InputStream* stream, juce::int64 claimed, juce::int64 available)
            : juce::AudioFormatReader (stream, "Short"), availableSamples (available)
        {
            sampleRate = fakeSampleRate;
            bitsPerSample = 32;
            numChannels = 2;
            usesFloatingPointData = true;
            lengthInSamples = claimed;
        }

        bool readSamples (int* const* destChannels, int numDestChannels,
                          int startOffsetInDestBuffer, juce::int64 startSampleInFile,
                          int numSamples) override
        {
            if (startSampleInFile + numSamples > availableSamples)
                return false;

            for (int ch = 0; ch < numDestChannels; ++ch)
            {
                if (destChannels[ch] == nullptr)
                    continue;

                auto* out = reinterpret_cast<float*> (destChannels[ch]) + startOffsetInDestBuffer;

                // A ramp, so a trimmed buffer can be checked for content rather
                // than merely for length.
                for (int i = 0; i < numSamples; ++i)
                    out[i] = 0.5f;
            }

            return true;
        }

    private:
        const juce::int64 availableSamples;
    };

    /** Wraps ShortReader so an AudioFormatManager will hand it out. */
    class ShortFormat final : public juce::AudioFormat
    {
    public:
        ShortFormat (juce::int64 claimed, juce::int64 available)
            : juce::AudioFormat ("Short", ".short"),
              claimedSamples (claimed), availableSamples (available) {}

        juce::Array<int> getPossibleSampleRates() override { return { 44100 }; }
        juce::Array<int> getPossibleBitDepths() override   { return { 32 }; }
        bool canDoStereo() override                        { return true; }
        bool canDoMono() override                          { return true; }

        juce::AudioFormatReader* createReaderFor (juce::InputStream* stream,
                                                  bool deleteStreamIfOpeningFails) override
        {
            if (stream == nullptr)
                return nullptr;

            juce::ignoreUnused (deleteStreamIfOpeningFails);
            return new ShortReader (stream, claimedSamples, availableSamples);
        }

        // Never written to; the decoder only ever reads.
        std::unique_ptr<juce::AudioFormatWriter>
            createWriterFor (std::unique_ptr<juce::OutputStream>&,
                             const juce::AudioFormatWriterOptions&) override
        {
            return nullptr;
        }

    private:
        const juce::int64 claimedSamples;
        const juce::int64 availableSamples;
    };

    /** A file with the right extension and some bytes in it, so the checks
        before the reader is created all pass. */
    struct Fixture
    {
        Fixture (juce::int64 claimed, juce::int64 available)
        {
            file.getFile().replaceWithText ("not really audio, the format is faked");
            formats.registerFormat (new ShortFormat (claimed, available), true);
        }

        juce::TemporaryFile file { ".short" };
        juce::AudioFormatManager formats;
    };
}

TEST_CASE ("a length that overshoots what decodes is trimmed, not refused", "[decoder]")
{
    // The real case: over 99.9 percent of the file decodes and the last fraction
    // of a second does not. Refusing the track over that is the wrong answer.
    const juce::int64 claimed = 5308416;
    const juce::int64 available = 5304320;

    Fixture fixture (claimed, available);

    juce::String reason;
    const auto decoded = opendj::TrackDecoder::decode (fixture.formats, fixture.file.getFile(), &reason);

    INFO ("reason: " << reason);
    REQUIRE (decoded != nullptr);

    // Trimmed to what really decoded, within one of the smallest retried reads,
    // so the length, the waveform and the beat grid all describe the same audio.
    REQUIRE (decoded->audio.getNumSamples() <= available);
    REQUIRE (decoded->audio.getNumSamples() > available - 512);

    // And it is audio, not an empty buffer of the right size.
    REQUIRE (decoded->audio.getMagnitude (0, 0, decoded->audio.getNumSamples()) > 0.4f);
}

TEST_CASE ("a short file with a bad tail still loads", "[decoder]")
{
    // Without retrying at a smaller size, one chunk covers the whole of a short
    // file and a single failure loses all of it.
    Fixture fixture (40000, 36000);

    juce::String reason;
    const auto decoded = opendj::TrackDecoder::decode (fixture.formats, fixture.file.getFile(), &reason);

    INFO ("reason: " << reason);
    REQUIRE (decoded != nullptr);
    REQUIRE (decoded->audio.getNumSamples() > 35000);
}

TEST_CASE ("a file that decodes nothing at all is refused", "[decoder]")
{
    Fixture fixture (44100, 0);

    juce::String reason;
    const auto decoded = opendj::TrackDecoder::decode (fixture.formats, fixture.file.getFile(), &reason);

    REQUIRE (decoded == nullptr);
    REQUIRE (reason.contains ("no audio could be decoded"));
}

TEST_CASE ("a failure says which cause it was", "[decoder]")
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    SECTION ("a file that is not there")
    {
        juce::String reason;
        const auto decoded = opendj::TrackDecoder::decode (
            formats, juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("opendj-no-such-file.wav"), &reason);

        REQUIRE (decoded == nullptr);
        REQUIRE (reason.contains ("not there"));
    }

    SECTION ("a file nothing can read")
    {
        juce::TemporaryFile text { ".txt" };
        text.getFile().replaceWithText ("plainly not audio");

        juce::String reason;
        const auto decoded = opendj::TrackDecoder::decode (formats, text.getFile(), &reason);

        REQUIRE (decoded == nullptr);

        // And it says what it can read, which is more use than what it cannot.
        REQUIRE (reason.contains ("Nothing here can read"));
        REQUIRE (reason.contains ("wav"));
    }

    SECTION ("an empty file")
    {
        juce::TemporaryFile empty { ".wav" };
        empty.getFile().create();

        juce::String reason;
        const auto decoded = opendj::TrackDecoder::decode (formats, empty.getFile(), &reason);

        REQUIRE (decoded == nullptr);
        REQUIRE (reason.contains ("empty"));
    }
}

TEST_CASE ("a track past the length limit is refused, and says by how much", "[decoder]")
{
    const auto claimed = static_cast<juce::int64> (
        (opendj::TrackDecoder::maxTrackMinutes + 5.0) * 60.0 * fakeSampleRate);

    Fixture fixture (claimed, claimed);

    juce::String reason;
    const auto decoded = opendj::TrackDecoder::decode (fixture.formats, fixture.file.getFile(), &reason);

    REQUIRE (decoded == nullptr);
    INFO ("reason: " << reason);

    // The real duration and the limit, so the reader can tell how far over it is
    // rather than being told only that it is over.
    REQUIRE (reason.contains ("65 min"));
    REQUIRE (reason.contains ("60 minutes"));
}
