/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "core/MixPoints.h"

#include <algorithm>
#include <cmath>

namespace opendj::automix
{

namespace
{
    double barSeconds (const TrackAnalysis& analysis) noexcept
    {
        return analysis.secondsPerBeat() * beatsPerBar;
    }

    /** The first bar boundary at or after the start of the track. */
    double gridOrigin (const TrackAnalysis& analysis) noexcept
    {
        const auto bar = barSeconds (analysis);
        return analysis.firstBeatSeconds - std::floor (analysis.firstBeatSeconds / bar) * bar;
    }

    /** The 75th percentile: the level of the main part of a track, which
        stays put however long the intro, outro or breakdowns are. */
    float bodyOf (std::vector<float> values)
    {
        if (values.empty())
            return 0.0f;

        const auto percentile = values.begin() + (std::ptrdiff_t) ((values.size() - 1) * 3 / 4);
        std::nth_element (values.begin(), percentile, values.end());
        return *percentile;
    }

    /** `bar` moved later to the next phrase boundary, counting phrases from `firstBar`. */
    int nextPhraseFrom (int firstBar, int bar) noexcept
    {
        const auto into = (bar - firstBar) % barsPerPhrase;
        return into == 0 ? bar : bar + (barsPerPhrase - into);
    }
}

double barStartSeconds (const TrackAnalysis& analysis, int bar) noexcept
{
    return gridOrigin (analysis) + bar * barSeconds (analysis);
}

std::vector<Bar> barLevels (const TrackAnalysis& analysis)
{
    std::vector<Bar> bars;
    const auto& peaks = analysis.detail;

    if (! analysis.hasTempo() || peaks.isEmpty() || analysis.sampleRate <= 0.0)
        return bars;

    const auto bucketSeconds = peaks.samplesPerBucket / analysis.sampleRate;
    const auto totalSeconds = (double) peaks.buckets.size() * bucketSeconds;
    const auto origin = gridOrigin (analysis);
    const auto bar = barSeconds (analysis);
    const auto count = (int) std::floor ((totalSeconds - origin) / bar);

    for (int b = 0; b < count; ++b)
    {
        const auto first = (size_t) std::ceil ((origin + b * bar) / bucketSeconds);
        const auto last = std::min (peaks.buckets.size(), (size_t) std::ceil ((origin + (b + 1) * bar) / bucketSeconds));
        double energy = 0.0, mid = 0.0, high = 0.0;

        for (auto i = first; i < last; ++i)
        {
            energy += peaks.buckets[i].energy;
            mid += peaks.buckets[i].mid;
            high += peaks.buckets[i].high;
        }

        const auto n = (double) std::max ((size_t) 1, last - first);
        bars.push_back ({ (float) (energy / n), (float) (mid / n), (float) (high / n) });
    }

    return bars;
}

TrackShape shapeOf (const std::vector<Bar>& bars)
{
    TrackShape shape;
    std::vector<float> energy, mid, high;

    for (const auto& bar : bars)
    {
        energy.push_back (bar.energy);
        mid.push_back (bar.mid);
        high.push_back (bar.high);
    }

    shape.body = bodyOf (energy);

    if (shape.body <= 0.0f)
        return shape;

    // How full a bar is: its mids and highs against the track's own, averaged
    // so a bar missing either the synths or the hats counts as thin.
    const auto midBody = std::max (1e-9f, bodyOf (mid));
    const auto highBody = std::max (1e-9f, bodyOf (high));

    const auto silent = [&] (int b) { return bars[(size_t) b].energy < silentBelow * shape.body; };
    const auto thin = [&] (int b)
    {
        const auto& bar = bars[(size_t) b];
        return (bar.mid / midBody + bar.high / highBody) * 0.5f < thinBelow;
    };
    const auto count = (int) bars.size();

    shape.firstBar = 0;

    while (shape.firstBar < count && silent (shape.firstBar))
        ++shape.firstBar;

    shape.lastBar = count - 1;

    while (shape.lastBar >= shape.firstBar && silent (shape.lastBar))
        --shape.lastBar;

    if (! shape.isValid())
        return shape;

    // The intro: thin bars from the start, in whole phrases.
    auto intro = 0;

    while (shape.firstBar + intro <= shape.lastBar && thin (shape.firstBar + intro))
        ++intro;

    shape.introBars = intro / barsPerPhrase * barsPerPhrase;

    // The outro: thin bars back from the end, starting on a phrase boundary
    // and no earlier than halfway, so a breakdown is not taken for one.
    auto outroStart = shape.lastBar + 1;

    while (outroStart - 1 >= shape.firstBar && thin (outroStart - 1))
        --outroStart;

    outroStart = nextPhraseFrom (shape.firstBar, outroStart);
    const auto halfway = (shape.firstBar + shape.lastBar + 1) / 2;

    if (outroStart >= halfway && shape.lastBar + 1 - outroStart >= barsPerPhrase)
        shape.outroBars = shape.lastBar + 1 - outroStart;

    return shape;
}

MixPlan planMix (const TrackAnalysis& outgoing, const TrackAnalysis& incoming)
{
    MixPlan plan;

    if (outgoing.confidence < leastConfidence || incoming.confidence < leastConfidence)
        return plan;

    const auto out = shapeOf (barLevels (outgoing));
    const auto in = shapeOf (barLevels (incoming));

    if (! out.isValid() || ! in.isValid())
        return plan;

    // Thin over thin can blend for two phrases when both last that long;
    // anything else, including a track that ends or starts at full tilt, gets
    // one. Whole phrases, so the fade ends where a phrase does.
    const auto longEnough = [] (int bars) { return bars == 0 || bars * beatsPerBar >= longFadeBeats; };
    const auto shaped = out.outroBars > 0 || in.introBars > 0;

    plan.fadeBeats = shaped && longEnough (out.outroBars) && longEnough (in.introBars) ? longFadeBeats
                                                                                       : shortFadeBeats;
    const auto fadeBars = plan.fadeBeats / beatsPerBar;

    // Out: the start of the outro, or else the last phrase boundary that
    // leaves the whole fade before the music stops. A long outro is joined
    // part way: once the fade is over the outgoing track is not heard, and
    // skipping a minute and a half of drums is not mixing, it is cutting.
    const auto phraseAtOrBefore = [&] (int bar)
    {
        return out.firstBar + (bar - out.firstBar) / barsPerPhrase * barsPerPhrase;
    };

    const auto latest = phraseAtOrBefore (out.lastBar + 1 - fadeBars);
    const auto earliest = phraseAtOrBefore (out.lastBar + 1 - mostBarsSkipped);
    const auto outBar = out.outroBars > 0 ? std::max (out.outroStartBar(), std::min (earliest, latest)) : latest;

    if (outBar <= out.firstBar)
        return plan;

    plan.outSeconds = barStartSeconds (outgoing, outBar);
    plan.inSeconds = barStartSeconds (incoming, in.firstBar);
    plan.smart = true;
    return plan;
}

} // namespace opendj::automix
