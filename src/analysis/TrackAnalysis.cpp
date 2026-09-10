/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "analysis/TrackAnalysis.h"

#include <cmath>

namespace opendj
{

const WaveformPeaks::Bucket& WaveformPeaks::bucketAt (juce::int64 sampleIndex) const noexcept
{
    static const Bucket empty;

    if (buckets.empty() || samplesPerBucket <= 0)
        return empty;

    const auto index = juce::jlimit<juce::int64> (0,
                                                  static_cast<juce::int64> (buckets.size()) - 1,
                                                  sampleIndex / samplesPerBucket);

    return buckets[static_cast<size_t> (index)];
}

double TrackAnalysis::nearestBeatSeconds (double seconds) const noexcept
{
    const auto period = secondsPerBeat();

    if (period <= 0.0)
        return seconds;

    const auto beatsFromAnchor = std::round ((seconds - firstBeatSeconds) / period);
    return firstBeatSeconds + beatsFromAnchor * period;
}

} // namespace opendj
