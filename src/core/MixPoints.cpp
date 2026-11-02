/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "core/MixPoints.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

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

std::vector<Section> sectionsOf (const std::vector<Bar>& bars, const TrackShape& shape,
                                 const std::vector<bool>& vocals)
{
    std::vector<Section> sections;

    if (! shape.isValid() || bars.empty())
        return sections;

    std::vector<float> mid, high;

    for (int b = shape.firstBar; b <= shape.lastBar; ++b)
    {
        mid.push_back (bars[(size_t) b].mid);
        high.push_back (bars[(size_t) b].high);
    }

    const auto midBody = std::max (1e-9f, bodyOf (mid));
    const auto highBody = std::max (1e-9f, bodyOf (high));
    const auto mainStart = shape.firstBar + shape.introBars;
    const auto mainEnd = shape.outroBars > 0 ? shape.outroStartBar() : shape.lastBar + 1;

    const auto kindOf = [&] (int from, int to)
    {
        if (to <= mainStart)
            return SectionKind::intro;

        if (from >= mainEnd)
            return SectionKind::outro;

        double level = 0.0, fullness = 0.0;
        int sung = 0;

        for (int b = from; b < to; ++b)
        {
            const auto& bar = bars[(size_t) b];
            level += bar.energy / shape.body;
            fullness += (bar.mid / midBody + bar.high / highBody) * 0.5f;

            if (juce::isPositiveAndBelow (b, (int) vocals.size()) && vocals[(size_t) b])
                ++sung;
        }

        const auto n = (double) std::max (1, to - from);
        level /= n;
        fullness /= n;

        if (level < breakdownBelow)
            return SectionKind::breakdown;

        if (fullness >= chorusAbove)
            return SectionKind::chorus;

        // A sung stretch nearly as full as the fullest is the hook, which
        // the band energies alone would call a verse.
        if (! vocals.empty() && sung / n > sungAbove && fullness >= chorusAbove * 0.9)
            return SectionKind::chorus;

        return SectionKind::verse;
    };

    for (int from = shape.firstBar; from <= shape.lastBar; from += barsPerPhrase)
    {
        // Phrases do not straddle the intro and outro boundaries, which are
        // phrase-aligned already; the last phrase may be short.
        const auto to = std::min (from + barsPerPhrase, shape.lastBar + 1);
        const auto kind = kindOf (from, to);

        if (! sections.empty() && sections.back().kind == kind)
            sections.back().bars += to - from;
        else
            sections.push_back ({ kind, from, to - from });
    }

    return sections;
}

SectionKind sectionAt (const std::vector<Section>& sections, int bar) noexcept
{
    for (const auto& section : sections)
        if (bar >= section.firstBar && bar < section.endBar())
            return section.kind;

    return SectionKind::verse;
}

const char* toString (SectionKind kind) noexcept
{
    switch (kind)
    {
        case SectionKind::intro:     return "intro";
        case SectionKind::verse:     return "verse";
        case SectionKind::chorus:    return "chorus";
        case SectionKind::breakdown: return "breakdown";
        case SectionKind::outro:     return "outro";
    }

    return "verse";
}

float bodyLevel (const TrackAnalysis& analysis)
{
    std::vector<float> levels;
    levels.reserve (analysis.detail.buckets.size());

    for (const auto& bucket : analysis.detail.buckets)
        levels.push_back (bucket.energy);

    return bodyOf (std::move (levels));
}

float levelMatch (const TrackAnalysis& outgoing, const TrackAnalysis& incoming)
{
    const auto out = bodyLevel (outgoing);
    const auto in = bodyLevel (incoming);

    if (out <= 0.0f || in <= 0.0f)
        return 1.0f;

    return std::clamp (out / in, quietestMatch, loudestMatch);
}

std::vector<bool> vocalBars (const SeparatedTrack& stems, const TrackAnalysis& analysis)
{
    std::vector<bool> sung;

    if (! analysis.hasTempo() || stems.isEmpty() || stems.sampleRate <= 0.0)
        return sung;

    const auto& vocals = stems.stems[(size_t) Stem::vocals];
    const auto numSamples = stems.getNumSamples();
    const auto origin = gridOrigin (analysis);
    const auto bar = barSeconds (analysis);
    const auto count = (int) std::floor ((numSamples / stems.sampleRate - origin) / bar);

    for (int b = 0; b < std::max (0, count); ++b)
    {
        const auto first = std::clamp ((int) std::lround ((origin + b * bar) * stems.sampleRate), 0, numSamples);
        const auto last = std::clamp ((int) std::lround ((origin + (b + 1) * bar) * stems.sampleRate), first, numSamples);
        double vocalSquares = 0.0, mixSquares = 0.0;

        for (int ch = 0; ch < vocals.getNumChannels(); ++ch)
        {
            const auto* v = vocals.getReadPointer (ch);
            std::array<const float*, numStems> parts {};

            for (size_t s = 0; s < parts.size(); ++s)
                parts[s] = stems.stems[s].getReadPointer (ch);

            for (int i = first; i < last; ++i)
            {
                const auto mix = parts[0][i] + parts[1][i] + parts[2][i] + parts[3][i];
                vocalSquares += (double) v[i] * v[i];
                mixSquares += (double) mix * mix;
            }
        }

        const auto n = (double) std::max (1, (last - first) * vocals.getNumChannels());
        const auto vocalRms = std::sqrt (vocalSquares / n);
        const auto mixRms = std::sqrt (mixSquares / n);

        sung.push_back (vocalRms > vocalFloor && vocalRms > vocalShareAbove * mixRms);
    }

    return sung;
}

MixPlan planMix (const TrackAnalysis& outgoing, const TrackAnalysis& incoming,
                 const std::vector<bool>& outgoingVocals, const std::vector<bool>& incomingVocals)
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

    auto chosenBar = outBar;
    auto chosenFadeBars = fadeBars;

    const auto outSections = sectionsOf (barLevels (outgoing), out, outgoingVocals);

    const auto chorusBars = [&] (int from, int length)
    {
        auto count = 0;

        for (int b = 0; b < length; ++b)
            if (sectionAt (outSections, from + b) == SectionKind::chorus)
                ++count;

        return count;
    };

    // No outro to mix from: the fade would otherwise start a fade's length
    // from the end, wherever that falls. Moved back a phrase or so, it can
    // start after the last chorus rather than cut into it.
    if (out.outroBars == 0 && (outgoingVocals.empty() || incomingVocals.empty()))
    {
        auto best = std::make_pair (chorusBars (outBar, fadeBars), 0);

        for (auto bar = outBar - barsPerPhrase; bar >= std::max (out.firstBar + barsPerPhrase, earliest);
             bar -= barsPerPhrase)
        {
            if (const auto candidate = std::make_pair (chorusBars (bar, fadeBars), outBar - bar); candidate < best)
            {
                best = candidate;
                chosenBar = bar;
            }
        }
    }

    // Two singers at once is the clash nobody misses. Count the bars where
    // both sing for the planned mix, then for the phrases either side and the
    // shorter fade, and move only if that sings over less.
    if (! outgoingVocals.empty() && ! incomingVocals.empty())
    {
        const auto sungIn = [] (const std::vector<bool>& bars, int b)
        {
            return juce::isPositiveAndBelow (b, (int) bars.size()) && bars[(size_t) b];
        };

        const auto clashes = [&] (int from, int length)
        {
            auto count = 0;

            for (int b = 0; b < length; ++b)
                if (sungIn (outgoingVocals, from + b) && sungIn (incomingVocals, in.firstBar + b))
                    ++count;

            return count;
        };

        // Fewest bars sung over each other, then the fade the shape asked
        // for, then no chorus cut into where there is no outro, then the
        // phrase nearest the one it chose: moved as little as will do.
        const auto cost = [&] (int bar, int length)
        {
            return std::make_tuple (clashes (bar, length), length == fadeBars ? 0 : 1,
                                    out.outroBars == 0 ? chorusBars (bar, length) : 0,
                                    std::abs (bar - outBar));
        };

        auto best = cost (outBar, fadeBars);
        const auto lowest = std::max (out.firstBar + barsPerPhrase,
                                      phraseAtOrBefore (out.lastBar + 1 - mostBarsSkippedForVocals));

        for (const auto length : { fadeBars, shortFadeBeats / beatsPerBar })
        {
            for (auto bar = phraseAtOrBefore (out.lastBar + 1 - length); bar >= lowest; bar -= barsPerPhrase)
            {
                if (const auto candidate = cost (bar, length); candidate < best)
                {
                    best = candidate;
                    chosenBar = bar;
                    chosenFadeBars = length;
                }
            }
        }

        plan.vocalClashBars = std::get<0> (best);

        if (plan.vocalClashBars > 0)
            for (int b = 0; b < chosenFadeBars && plan.vocalHandoffBar < 0; ++b)
                if (sungIn (incomingVocals, in.firstBar + b) && sungIn (outgoingVocals, chosenBar + b))
                    plan.vocalHandoffBar = b;

        // The outgoing vocals go from the first bar the incoming ones are in,
        // even if the two only meet later, so the handoff is one clean change.
        if (plan.vocalHandoffBar > 0)
            while (plan.vocalHandoffBar > 0 && sungIn (incomingVocals, in.firstBar + plan.vocalHandoffBar - 1))
                --plan.vocalHandoffBar;
    }

    plan.fadeBeats = chosenFadeBars * beatsPerBar;
    plan.outSection = sectionAt (outSections, chosenBar);
    plan.outSeconds = barStartSeconds (outgoing, chosenBar);
    plan.inSeconds = barStartSeconds (incoming, in.firstBar);
    plan.smart = true;
    return plan;
}

} // namespace opendj::automix
