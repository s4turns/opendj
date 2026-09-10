/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <memory>

namespace opendj
{

/** A file decoded to memory: always stereo, at the file's own sample rate. */
struct DecodedAudio
{
    juce::AudioBuffer<float> audio;
    double sampleRate = 44100.0;

    double lengthSeconds() const noexcept
    {
        return sampleRate > 0.0 ? audio.getNumSamples() / sampleRate : 0.0;
    }
};

/** Reads a whole file into memory, the way both the decks and the library
    scanner need it. One place for the rules about what is refused, so a track
    the scanner analysed is one the deck will also play. */
class TrackDecoder
{
public:
    /** Refuse absurd files rather than exhausting memory on a mistaken load. */
    static constexpr double maxTrackMinutes = 30.0;

    /** Returns null if the file could not be read or is too long. */
    static std::unique_ptr<DecodedAudio> decode (juce::AudioFormatManager& formatManager,
                                                 const juce::File& file);
};

} // namespace opendj
