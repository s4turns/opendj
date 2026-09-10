/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "analysis/TrackDecoder.h"

namespace opendj
{

std::unique_ptr<DecodedAudio> TrackDecoder::decode (juce::AudioFormatManager& formatManager,
                                                    const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader == nullptr || reader->numChannels == 0 || reader->lengthInSamples <= 0)
        return nullptr;

    const auto seconds = static_cast<double> (reader->lengthInSamples) / reader->sampleRate;

    if (seconds > maxTrackMinutes * 60.0)
        return nullptr;

    auto decoded = std::make_unique<DecodedAudio>();
    decoded->sampleRate = reader->sampleRate;
    decoded->audio.setSize (2, static_cast<int> (reader->lengthInSamples));

    if (! reader->read (&decoded->audio, 0, static_cast<int> (reader->lengthInSamples), 0, true, true))
        return nullptr;

    // A mono file reads into channel 0 only, so mirror it across.
    if (reader->numChannels == 1)
        decoded->audio.copyFrom (1, 0, decoded->audio, 0, 0, decoded->audio.getNumSamples());

    return decoded;
}

} // namespace opendj
