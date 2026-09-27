/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include "analysis/TrackAnalysis.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <vector>

namespace opendj
{

/** The arithmetic behind the waveforms and the beat strip.

    Header only and free of any component, the same bargain `AngleMath.h` makes
    for the platters: what reliably goes wrong in a zoomable view is the mapping
    between seconds, pixels and buckets at the ends of its range, and what goes
    wrong in a strip comparing two decks is the one between track time and the
    time a listener is in. Both deserve testing on their own rather than only by
    eye.
*/
namespace waveform
{
    /** The zoom ladder, in seconds either side of the playhead. Fixed steps
        rather than a continuous zoom, so the view can be returned to exactly
        where it was and two decks can be trusted to be at the same scale. */
    inline constexpr std::array<double, 10> zoomLevels
    {
        0.5, 1.0, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0, 24.0
    };

    /** Three seconds either side, which is the six second window the scrolling
        waveform had before there was anything to zoom with. */
    inline constexpr double defaultZoomSeconds = 3.0;

    /** The rung nearest a number, for settling a value that arrived from a
        settings file somebody has edited by hand. */
    inline double nearestZoom (double seconds) noexcept
    {
        auto best = zoomLevels.front();

        for (const auto level : zoomLevels)
            if (std::abs (level - seconds) < std::abs (best - seconds))
                best = level;

        return best;
    }

    /** A number of rungs up or down, clamped at both ends. Positive steps zoom
        in, which is the direction a wheel pushed away from you should go. */
    inline double stepZoom (double seconds, int steps) noexcept
    {
        const auto current = nearestZoom (seconds);
        auto index = 0;

        for (size_t i = 0; i < zoomLevels.size(); ++i)
            if (juce::approximatelyEqual (zoomLevels[i], current))
                index = static_cast<int> (i);

        // A step in is a step down the ladder, since the levels are widths.
        const auto wanted = juce::jlimit (0, static_cast<int> (zoomLevels.size()) - 1, index - steps);
        return zoomLevels[static_cast<size_t> (wanted)];
    }

    inline double secondsPerPixel (double windowSeconds, int width) noexcept
    {
        return windowSeconds * 2.0 / juce::jmax (1, width);
    }

    /** How much of the track one of these buckets covers. */
    inline double bucketSeconds (const WaveformPeaks& peaks, double sampleRate) noexcept
    {
        if (sampleRate <= 0.0 || peaks.samplesPerBucket <= 0)
            return 0.0;

        return peaks.samplesPerBucket / sampleRate;
    }

    /** Which set of peaks to read at a given scale.

        The overview's buckets are a fixed share of the track rather than a
        fixed length, so this cannot be a constant: at 2048 buckets one covers
        0.15 s of a five minute track and 0.015 s of a thirty second one. Once a
        pixel is wider than a bucket the coarser set says the same thing for a
        fraction of the reads, and below that it would lose detail the finer set
        still has. */
    inline const WaveformPeaks& peaksFor (const TrackAnalysis& analysis, double perPixel) noexcept
    {
        const auto overview = bucketSeconds (analysis.overview, analysis.sampleRate);

        if (analysis.detail.isEmpty() || (overview > 0.0 && perPixel >= overview))
            return analysis.overview;

        return analysis.detail;
    }

    /** Everything under one pixel, as a single bucket.

        Extremes for the outline and averages for the colour, which is what each
        is for: one loud sample inside the pixel should reach the top of the
        bar, but should not by itself decide what colour the pixel is. Reading a
        single bucket per pixel instead, which is what this replaces, threw away
        six buckets in every seven even before anything could be zoomed out, and
        turned into noise the moment it could. */
    inline WaveformPeaks::Bucket reduce (const WaveformPeaks& peaks,
                                         double sampleRate,
                                         double fromSeconds,
                                         double toSeconds) noexcept
    {
        if (peaks.isEmpty() || sampleRate <= 0.0 || peaks.samplesPerBucket <= 0)
            return {};

        const auto last = static_cast<juce::int64> (peaks.buckets.size()) - 1;

        const auto bucketOf = [&] (double seconds)
        {
            const auto sample = static_cast<juce::int64> (seconds * sampleRate);
            return juce::jlimit<juce::int64> (0, last, sample / peaks.samplesPerBucket);
        };

        const auto first = bucketOf (fromSeconds);
        const auto final = juce::jmax (first, bucketOf (toSeconds));

        auto result = peaks.buckets[static_cast<size_t> (first)];

        auto low = static_cast<double> (result.low);
        auto mid = static_cast<double> (result.mid);
        auto high = static_cast<double> (result.high);
        auto energy = static_cast<double> (result.energy);

        for (auto i = first + 1; i <= final; ++i)
        {
            const auto& bucket = peaks.buckets[static_cast<size_t> (i)];

            result.minimum = juce::jmin (result.minimum, bucket.minimum);
            result.maximum = juce::jmax (result.maximum, bucket.maximum);

            low += bucket.low;
            mid += bucket.mid;
            high += bucket.high;
            energy += bucket.energy;
        }

        const auto count = static_cast<double> (final - first + 1);

        result.low = static_cast<float> (low / count);
        result.mid = static_cast<float> (mid / count);
        result.high = static_cast<float> (high / count);
        result.energy = static_cast<float> (energy / count);

        return result;
    }

    /** One beat of a deck, placed in the time a listener is in.

        `offsetSeconds` is how long until it is heard, negative once it has
        been. */
    struct BeatMark
    {
        double offsetSeconds = 0.0;
        bool isDownbeat = false;
    };

    /** As many beats of a deck as fall within `spanSeconds` either side of now.

        The waveform draws its grid in track seconds, which is right there: it
        is drawing the track. This is not. The question a strip of two decks
        answers is whether their beats land together as heard, so it works in
        real time, and a deck pulled by its tempo fader spaces its beats out
        accordingly. `bpm` and `firstBeatSeconds` are the recorded track's, and
        `tempoRatio` is how much faster than recorded it is being played.

        Nothing is returned for a deck with no tempo, rather than a grid made up
        to fill the row. */
    inline std::vector<BeatMark> beatMarks (double bpm,
                                            double firstBeatSeconds,
                                            double positionSeconds,
                                            double tempoRatio,
                                            double spanSeconds)
    {
        std::vector<BeatMark> marks;

        if (bpm <= 0.0 || tempoRatio <= 0.0 || spanSeconds <= 0.0)
            return marks;

        const auto period = 60.0 / bpm;                    // track seconds a beat
        const auto reach = spanSeconds * tempoRatio;       // track seconds in view

        const auto from = std::ceil ((positionSeconds - reach - firstBeatSeconds) / period);
        const auto to = std::floor ((positionSeconds + reach - firstBeatSeconds) / period);

        // A tempo of 180 over a couple of seconds is a handful of beats. This is
        // only here so that nonsense arriving from a bad analysis cannot turn
        // into an allocation the size of the machine.
        constexpr int mostWorthDrawing = 256;

        for (auto beat = from; beat <= to && marks.size() < mostWorthDrawing; beat += 1.0)
        {
            const auto trackSeconds = firstBeatSeconds + beat * period;

            marks.push_back ({ (trackSeconds - positionSeconds) / tempoRatio,
                               std::abs (std::fmod (beat, 4.0)) < 0.001 });
        }

        return marks;
    }

    /** How long a beat of this deck lasts as heard, which is what two decks are
        being compared on. Zero when the deck has no tempo. */
    inline double playedSecondsPerBeat (double bpm, double tempoRatio) noexcept
    {
        return bpm > 0.0 && tempoRatio > 0.0 ? 60.0 / (bpm * tempoRatio) : 0.0;
    }

    /** Whether a beat grid at this scale would say anything.

        Below a couple of pixels a beat the lines merge into a wash that hides
        the waveform instead of measuring it, and the loop that draws them is
        bounded by the same rule: it can never run past half the width of the
        component in iterations. */
    inline bool beatsAreWorthDrawing (double secondsPerBeat, double perPixel) noexcept
    {
        return secondsPerBeat > 0.0 && perPixel > 0.0 && secondsPerBeat / perPixel >= 2.0;
    }
}

} // namespace opendj
