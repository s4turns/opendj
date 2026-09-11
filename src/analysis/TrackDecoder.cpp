/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "analysis/TrackDecoder.h"

namespace opendj
{

namespace
{
    void report (juce::String* destination, const juce::String& reason)
    {
        if (destination != nullptr)
            *destination = reason;
    }

    juce::String describeLength (double seconds)
    {
        const auto minutes = static_cast<int> (seconds) / 60;
        const auto remainder = static_cast<int> (seconds) % 60;

        return juce::String (minutes) + " min " + juce::String (remainder) + " sec";
    }
}

std::unique_ptr<DecodedAudio> TrackDecoder::decode (juce::AudioFormatManager& formatManager,
                                                    const juce::File& file,
                                                    juce::String* failureReason)
{
    report (failureReason, {});

    if (! file.existsAsFile())
    {
        report (failureReason, "The file is not there any more.");
        return nullptr;
    }

    if (file.getSize() == 0)
    {
        report (failureReason, "The file is empty.");
        return nullptr;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader == nullptr)
    {
        // Either the extension is one nothing here reads, or the contents are
        // not what the extension claims. Saying which formats are understood is
        // more use than saying this one was not.
        report (failureReason,
                "Nothing here can read that file. OpenDJ reads "
                + formatManager.getWildcardForAllFormats().replace (";", ", ") + ".");
        return nullptr;
    }

    if (reader->numChannels == 0 || reader->lengthInSamples <= 0)
    {
        // A reader that opens but reports nothing usually means a truncated or
        // half-written file: a download that stopped, or an export still being
        // written when it was dragged in.
        report (failureReason,
                "The file opened as " + reader->getFormatName()
                    + " but contains no audio. It may be truncated or still being written.");
        return nullptr;
    }

    const auto seconds = static_cast<double> (reader->lengthInSamples) / reader->sampleRate;

    if (seconds > maxTrackMinutes * 60.0)
    {
        report (failureReason,
                "It is " + describeLength (seconds) + " long, and the limit is "
                    + juce::String (static_cast<int> (maxTrackMinutes))
                    + " minutes. A whole track is decoded into memory, which costs about 10 MB a minute.");
        return nullptr;
    }

    // A track longer than this cannot be addressed by an int sample index, which
    // is what the buffer and every read position use.
    if (reader->lengthInSamples > std::numeric_limits<int>::max())
    {
        report (failureReason, "The file has too many samples to address.");
        return nullptr;
    }

    auto decoded = std::make_unique<DecodedAudio>();
    decoded->sampleRate = reader->sampleRate;

    const auto numSamples = static_cast<int> (reader->lengthInSamples);

    try
    {
        decoded->audio.setSize (2, numSamples);
    }
    catch (const std::bad_alloc&)
    {
        report (failureReason,
                "There was not enough memory for " + describeLength (seconds) + " of audio.");
        return nullptr;
    }

    // Read in chunks rather than in one call, and keep whatever decoded.
    //
    // A compressed file's length is an estimate. An MP3's is worked out from its
    // frame headers, and for a file whose last frame is short or padded the
    // estimate overshoots what the decoder can actually produce, by a few
    // thousand samples at the very end. Demanding the whole reported length in
    // one read then fails on the final chunk, and refusing the track over the
    // last ninety milliseconds of it is the wrong answer: the other 99.9 percent
    // is perfectly good audio.
    // On a failure the chunk is halved and tried again, down to a small floor.
    // Without that, a short file whose tail is bad loses everything, because a
    // single chunk covers the whole of it; with it, what is salvaged is within
    // a few hundred samples of the truth whatever the file's length.
    constexpr int chunkSamples = 1 << 16;
    constexpr int smallestChunk = 512;

    int decodedSamples = 0;

    while (decodedSamples < numSamples)
    {
        auto count = juce::jmin (chunkSamples, numSamples - decodedSamples);
        auto read = false;

        while (! (read = reader->read (&decoded->audio, decodedSamples, count,
                                       decodedSamples, true, true))
               && count > smallestChunk)
        {
            count /= 2;
        }

        if (! read)
            break;

        decodedSamples += count;
    }

    if (decodedSamples == 0)
    {
        report (failureReason,
                "The file opened as " + reader->getFormatName()
                    + " but no audio could be decoded from it. It is probably damaged.");
        return nullptr;
    }

    if (decodedSamples < numSamples)
    {
        // Trim to what really decoded, so the length, the waveform and the beat
        // grid all describe the same audio rather than trailing off into a tail
        // of silence nobody asked for.
        decoded->audio.setSize (2, decodedSamples, true, false, true);
    }

    // A mono file reads into channel 0 only, so mirror it across.
    if (reader->numChannels == 1)
        decoded->audio.copyFrom (1, 0, decoded->audio, 0, 0, decoded->audio.getNumSamples());

    return decoded;
}

} // namespace opendj
