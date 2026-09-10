/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include "analysis/TrackAnalysis.h"

#include <memory>

namespace opendj
{

/** Turns a decoded track into waveform peaks and a beat grid.

    Tempo detection follows the usual three steps. A spectral flux onset envelope
    says where energy rises; autocorrelation of that envelope says how far apart
    those rises are; and correlating a pulse train against the envelope says where
    the beats actually fall. It is deliberately self contained rather than pulling
    in another dependency, and it is accurate enough for the four-to-the-floor
    material this is aimed at.

    All of this is slow and blocking, so it runs on a background thread.
*/
class TrackAnalyser
{
public:
    struct Options
    {
        /** Roughly how many buckets the overview waveform should have. */
        int overviewBuckets = 2048;

        /** Milliseconds of audio per bucket in the scrolling waveform. */
        double detailBucketMs = 2.0;

        double minimumBpm = 70.0;
        double maximumBpm = 180.0;
    };

    /** Analyses a stereo buffer. Never returns null. */
    static std::shared_ptr<const TrackAnalysis> analyse (const juce::AudioBuffer<float>& audio,
                                                         double sampleRate,
                                                         Options options);

    /** The same with default options. A separate overload rather than a default
        argument, because GCC refuses `Options options = {}` on a nested struct
        with member initialisers (bug 96645); MSVC accepts it, so the Windows
        build never noticed. */
    static std::shared_ptr<const TrackAnalysis> analyse (const juce::AudioBuffer<float>& audio,
                                                         double sampleRate)
    {
        return analyse (audio, sampleRate, Options());
    }

    /** Builds the waveforms but takes the tempo as given, for a track the
        library analysed on an earlier day. The peaks are a linear pass over the
        audio and cheap; the tempo pass is the part worth remembering. */
    static std::shared_ptr<const TrackAnalysis> withKnownTempo (const juce::AudioBuffer<float>& audio,
                                                                double sampleRate,
                                                                double bpm,
                                                                double firstBeatSeconds,
                                                                float confidence,
                                                                Options options);

    /** Peaks only, for when a waveform is wanted before the tempo pass finishes. */
    static WaveformPeaks buildPeaks (const juce::AudioBuffer<float>& audio, int samplesPerBucket);
};

} // namespace opendj
