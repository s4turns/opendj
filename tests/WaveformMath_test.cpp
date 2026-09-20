/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ui/WaveformMath.h"

using Catch::Matchers::WithinAbs;
using namespace opendj;

namespace
{
    constexpr double sampleRate = 44100.0;

    /** Peaks whose buckets each hold one known value, so a reduction over a
        span can be checked against arithmetic rather than against audio. */
    WaveformPeaks rampPeaks (int bucketCount, int samplesPerBucket)
    {
        WaveformPeaks peaks;
        peaks.samplesPerBucket = samplesPerBucket;
        peaks.buckets.resize (static_cast<size_t> (bucketCount));

        for (int i = 0; i < bucketCount; ++i)
        {
            auto& bucket = peaks.buckets[static_cast<size_t> (i)];
            bucket.maximum = static_cast<float> (i);
            bucket.minimum = -static_cast<float> (i);
            bucket.low = static_cast<float> (i);
            bucket.mid = static_cast<float> (i) * 2.0f;
            bucket.high = static_cast<float> (i) * 3.0f;
            bucket.energy = static_cast<float> (i);
        }

        return peaks;
    }
}

TEST_CASE ("the zoom ladder clamps at both ends", "[ui][waveform]")
{
    REQUIRE_THAT (waveform::stepZoom (waveform::zoomLevels.front(), 5),
                  WithinAbs (waveform::zoomLevels.front(), 0.0001));
    REQUIRE_THAT (waveform::stepZoom (waveform::zoomLevels.back(), -5),
                  WithinAbs (waveform::zoomLevels.back(), 0.0001));
}

TEST_CASE ("a step in and back out returns to where it started", "[ui][waveform]")
{
    const auto start = waveform::defaultZoomSeconds;

    REQUIRE_THAT (waveform::stepZoom (waveform::stepZoom (start, 1), -1), WithinAbs (start, 0.0001));
    REQUIRE_THAT (waveform::stepZoom (waveform::stepZoom (start, -1), 1), WithinAbs (start, 0.0001));
}

TEST_CASE ("zooming in narrows the window and zooming out widens it", "[ui][waveform]")
{
    REQUIRE (waveform::stepZoom (waveform::defaultZoomSeconds, 1) < waveform::defaultZoomSeconds);
    REQUIRE (waveform::stepZoom (waveform::defaultZoomSeconds, -1) > waveform::defaultZoomSeconds);
}

TEST_CASE ("a zoom from a hand edited file lands on the nearest rung", "[ui][waveform]")
{
    REQUIRE_THAT (waveform::nearestZoom (3.4), WithinAbs (3.0, 0.0001));
    REQUIRE_THAT (waveform::nearestZoom (1000.0), WithinAbs (waveform::zoomLevels.back(), 0.0001));
    REQUIRE_THAT (waveform::nearestZoom (-5.0), WithinAbs (waveform::zoomLevels.front(), 0.0001));
}

TEST_CASE ("seconds per pixel follows the window and the width", "[ui][waveform]")
{
    // The window is either side of the playhead, so the span is twice it.
    REQUIRE_THAT (waveform::secondsPerPixel (3.0, 600), WithinAbs (0.01, 0.00001));
    REQUIRE_THAT (waveform::secondsPerPixel (3.0, 0), WithinAbs (6.0, 0.00001));
}

TEST_CASE ("the coarser peaks take over once a pixel is wider than one of their buckets",
           "[ui][waveform]")
{
    TrackAnalysis analysis;
    analysis.sampleRate = sampleRate;
    analysis.detail = rampPeaks (100, 88);            // 2 ms a bucket
    analysis.overview = rampPeaks (100, 4410);        // 100 ms a bucket

    REQUIRE (&waveform::peaksFor (analysis, 0.01) == &analysis.detail);
    REQUIRE (&waveform::peaksFor (analysis, 0.2) == &analysis.overview);
}

TEST_CASE ("a pixel reduces to the extremes and the mean of what is under it", "[ui][waveform]")
{
    // Ten buckets of one second each, reduced over the first four.
    const auto peaks = rampPeaks (10, static_cast<int> (sampleRate));
    const auto bucket = waveform::reduce (peaks, sampleRate, 0.0, 3.5);

    REQUIRE_THAT (bucket.maximum, WithinAbs (3.0f, 0.0001f));   // the largest, not the last
    REQUIRE_THAT (bucket.minimum, WithinAbs (-3.0f, 0.0001f));
    REQUIRE_THAT (bucket.low, WithinAbs (1.5f, 0.0001f));       // the mean of 0, 1, 2 and 3
    REQUIRE_THAT (bucket.mid, WithinAbs (3.0f, 0.0001f));
    REQUIRE_THAT (bucket.high, WithinAbs (4.5f, 0.0001f));
}

TEST_CASE ("a pixel narrower than a bucket reads that one bucket", "[ui][waveform]")
{
    const auto peaks = rampPeaks (10, static_cast<int> (sampleRate));
    const auto bucket = waveform::reduce (peaks, sampleRate, 4.1, 4.2);

    REQUIRE_THAT (bucket.maximum, WithinAbs (4.0f, 0.0001f));
    REQUIRE_THAT (bucket.low, WithinAbs (4.0f, 0.0001f));
}

TEST_CASE ("a reduction past either end of the track is clamped, not read off it", "[ui][waveform]")
{
    const auto peaks = rampPeaks (10, static_cast<int> (sampleRate));

    REQUIRE_THAT (waveform::reduce (peaks, sampleRate, -50.0, -49.0).maximum, WithinAbs (0.0f, 0.0001f));
    REQUIRE_THAT (waveform::reduce (peaks, sampleRate, 500.0, 501.0).maximum, WithinAbs (9.0f, 0.0001f));
    REQUIRE_THAT (waveform::reduce ({}, sampleRate, 0.0, 1.0).maximum, WithinAbs (0.0f, 0.0001f));
}

TEST_CASE ("a beat grid denser than two pixels a beat is not worth drawing", "[ui][waveform]")
{
    const auto secondsPerBeat = 0.5;   // 120 BPM

    REQUIRE (waveform::beatsAreWorthDrawing (secondsPerBeat, 0.05));
    REQUIRE (waveform::beatsAreWorthDrawing (secondsPerBeat, 0.25));
    REQUIRE_FALSE (waveform::beatsAreWorthDrawing (secondsPerBeat, 0.3));
    REQUIRE_FALSE (waveform::beatsAreWorthDrawing (0.0, 0.01));
}
