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

    if (! reader->read (&decoded->audio, 0, numSamples, 0, true, true))
    {
        report (failureReason,
                "The file opened as " + reader->getFormatName()
                    + " but decoding failed part way through. It is probably damaged.");
        return nullptr;
    }

    // A mono file reads into channel 0 only, so mirror it across.
    if (reader->numChannels == 1)
        decoded->audio.copyFrom (1, 0, decoded->audio, 0, 0, decoded->audio.getNumSamples());

    return decoded;
}

} // namespace opendj
