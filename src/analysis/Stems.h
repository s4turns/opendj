/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <memory>

namespace opendj
{

/** The four parts a track is separated into.

    The order is the one every separation model and every controller agrees on,
    and it is the order the DJ-202's first four pads sit in.
*/
enum class Stem
{
    drums = 0,
    bass,
    other,
    vocals
};

inline constexpr int numStems = 4;

inline const char* toString (Stem stem)
{
    switch (stem)
    {
        case Stem::drums:  return "drums";
        case Stem::bass:   return "bass";
        case Stem::other:  return "other";
        case Stem::vocals: return "vocals";
    }

    return "unknown";
}

/** A separated track: four stereo buffers that sum back to the original.

    Immutable once built and handed around by shared pointer, like TrackAnalysis
    and for the same reason: the interface and the loader can hold one for as
    long as they like without any bearing on the audio thread's lifetime rules.

    Separation is not something that can be done as the music plays. Every model
    worth using needs several seconds of audio either side of the moment it is
    working on, so it cannot be run on a signal that has not arrived yet. What
    can be instant is the *control*: with the parts already separated, muting the
    vocals is a gain change, and it works while scratching, while cueing and
    under key lock, because it happens after all of them.
*/
struct SeparatedTrack
{
    std::array<juce::AudioBuffer<float>, numStems> stems;
    double sampleRate = 44100.0;

    int getNumSamples() const noexcept { return stems[0].getNumSamples(); }

    bool isEmpty() const noexcept { return getNumSamples() <= 0; }

    /** True when every stem is the same length and carries two channels, which
        is what the audio thread assumes without checking. */
    bool isWellFormed() const noexcept
    {
        for (const auto& stem : stems)
            if (stem.getNumChannels() != 2 || stem.getNumSamples() != getNumSamples())
                return false;

        return getNumSamples() > 0;
    }
};

} // namespace opendj
