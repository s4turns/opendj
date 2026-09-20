/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "analysis/KeyDetector.h"

#include <vector>

namespace opendj
{

/** Peak data for drawing one waveform, at a fixed number of samples per bucket. */
struct WaveformPeaks
{
    struct Bucket
    {
        float minimum = 0.0f;
        float maximum = 0.0f;
        float energy = 0.0f;   ///< RMS, which is what gives a waveform its body

        /** RMS in each of the three bands the mixer's EQ splits at, which is
            what colours the waveform. They do not add up to `energy`: the bands
            come out of a Linkwitz-Riley crossover, whose halves add in amplitude
            rather than in power. */
        float low = 0.0f;
        float mid = 0.0f;
        float high = 0.0f;
    };

    int samplesPerBucket = 1;
    std::vector<Bucket> buckets;

    /** The bucket covering a position in the source, clamped to the range. */
    const Bucket& bucketAt (juce::int64 sampleIndex) const noexcept;
    bool isEmpty() const noexcept { return buckets.empty(); }
};

/** Everything analysis knows about a loaded track.

    This is deliberately separate from the audio the deck plays. It is built once,
    never modified afterwards, and handed around as a shared pointer, so the
    interface can draw from it without any bearing on the audio thread's own
    lifetime rules.
*/
struct TrackAnalysis
{
    WaveformPeaks overview;   ///< the whole track in a few thousand buckets
    WaveformPeaks detail;     ///< fine enough to scroll past the playhead

    double sampleRate = 44100.0;
    double bpm = 0.0;                ///< 0 when no stable tempo was found
    double firstBeatSeconds = 0.0;   ///< the downbeat the grid is anchored to
    float confidence = 0.0f;         ///< 0 to 1, how clear the tempo peak was

    MusicalKey key;                  ///< invalid when nothing tonal was found

    bool hasTempo() const noexcept { return bpm > 0.0; }
    bool hasKey() const noexcept { return key.isValid(); }

    /** The beat nearest a position, in seconds. Returns the position unchanged
        when there is no grid. */
    double nearestBeatSeconds (double seconds) const noexcept;

    double secondsPerBeat() const noexcept { return bpm > 0.0 ? 60.0 / bpm : 0.0; }
};

} // namespace opendj
