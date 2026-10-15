/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/AutoMixMath.h"
#include "core/MixPoints.h"

using Catch::Matchers::WithinAbs;
using namespace opendj;
using namespace opendj::automix;

namespace
{
    /** An analysis whose detail peaks hold one level per bar, laid on the grid
        the way a real track's would be: nothing before the first bar line. */
    TrackAnalysis trackOf (const std::vector<Bar>& levels, double bpm = 120.0, double firstBeat = 0.25)
    {
        TrackAnalysis analysis;
        analysis.sampleRate = 44100.0;
        analysis.bpm = bpm;
        analysis.firstBeatSeconds = firstBeat;
        analysis.confidence = 0.9f;
        analysis.detail.samplesPerBucket = 441;   // 10 ms

        const auto bucketSeconds = 0.01;
        const auto origin = barStartSeconds (analysis, 0);
        const auto bar = 4.0 * 60.0 / bpm;
        const auto total = origin + bar * (double) levels.size();

        for (double t = 0.0; t < total; t += bucketSeconds)
        {
            WaveformPeaks::Bucket bucket;
            const auto index = (int) std::floor ((t + bucketSeconds * 0.5 - origin) / bar);

            if (t >= origin && index < (int) levels.size())
            {
                bucket.energy = levels[(size_t) index].energy;
                bucket.mid = levels[(size_t) index].mid;
                bucket.high = levels[(size_t) index].high;
            }

            analysis.detail.buckets.push_back (bucket);
        }

        return analysis;
    }

    const Bar silence { 0.0f, 0.0f, 0.0f };
    const Bar full { 1.0f, 1.0f, 1.0f };
    const Bar drums { 0.9f, 0.3f, 0.4f };       ///< kick and hats: loud, but thin in the mids
    const Bar breakdown { 0.3f, 0.8f, 0.6f };   ///< no kick, pads carrying on

    void add (std::vector<Bar>& levels, int bars, Bar bar)
    {
        levels.insert (levels.end(), (size_t) bars, bar);
    }

    /** 2 bars of silence, an 8 bar drum intro, 48 bars of the main part, a 16
        bar drum outro, then 3 bars of silence. */
    std::vector<Bar> typicalTrack()
    {
        std::vector<Bar> levels;
        add (levels, 2, silence);
        add (levels, 8, drums);
        add (levels, 48, full);
        add (levels, 16, drums);
        add (levels, 3, silence);
        return levels;
    }
}

TEST_CASE ("bars are measured on the beat grid", "[automix]")
{
    const auto analysis = trackOf (typicalTrack());
    const auto bars = barLevels (analysis);

    REQUIRE (bars.size() == typicalTrack().size());
    REQUIRE_THAT (bars[0].energy, WithinAbs (0.0, 1e-6));
    REQUIRE_THAT (bars[2].energy, WithinAbs (0.9, 0.02));
    REQUIRE_THAT (bars[2].mid, WithinAbs (0.3, 0.02));
    REQUIRE_THAT (bars[2].high, WithinAbs (0.4, 0.02));
    REQUIRE_THAT (bars[30].energy, WithinAbs (1.0, 0.02));

    // Bar lines sit on the downbeat, and bar 0 is the first whole bar.
    REQUIRE_THAT (barStartSeconds (analysis, 0), WithinAbs (0.25, 1e-9));
    REQUIRE_THAT (barStartSeconds (analysis, 3), WithinAbs (6.25, 1e-9));
    REQUIRE_THAT (barStartSeconds (trackOf ({}, 120.0, 4.75), 0), WithinAbs (0.75, 1e-9));

    REQUIRE (barLevels (TrackAnalysis()).empty());   // no tempo, nothing to measure
}

TEST_CASE ("a track's intro and outro are found in whole phrases", "[automix]")
{
    // Found by what is missing from the mids and highs, although the drums
    // keep the overall level near full.
    const auto shape = shapeOf (barLevels (trackOf (typicalTrack())));

    REQUIRE (shape.isValid());
    REQUIRE (shape.firstBar == 2);
    REQUIRE (shape.lastBar == 73);
    REQUIRE (shape.introBars == 8);
    REQUIRE (shape.outroBars == 16);
    REQUIRE (shape.outroStartBar() == 58);
    REQUIRE_THAT (shape.body, WithinAbs (1.0, 0.02));
}

TEST_CASE ("a partial phrase of intro is not counted", "[automix]")
{
    std::vector<Bar> levels;
    add (levels, 6, drums);
    add (levels, 40, full);

    REQUIRE (shapeOf (levels).introBars == 4);
}

TEST_CASE ("an outro starts on a phrase boundary", "[automix]")
{
    // The quiet part starts two bars into a phrase; the outro starts at the next one.
    std::vector<Bar> levels;
    add (levels, 42, full);
    add (levels, 14, drums);

    const auto shape = shapeOf (levels);
    REQUIRE (shape.outroStartBar() == 44);
    REQUIRE (shape.outroBars == 12);
}

TEST_CASE ("a breakdown in the middle is not an outro", "[automix]")
{
    std::vector<Bar> levels;
    add (levels, 32, full);
    add (levels, 8, breakdown);
    add (levels, 32, full);

    const auto shape = shapeOf (levels);
    REQUIRE (shape.introBars == 0);
    REQUIRE (shape.outroBars == 0);

    // Nor is a thin stretch that covers more than half the track.
    std::vector<Bar> mostlyDrums;
    add (mostlyDrums, 15, full);
    add (mostlyDrums, 25, drums);
    REQUIRE (shapeOf (mostlyDrums).outroBars == 0);
}

TEST_CASE ("silence alone has no shape", "[automix]")
{
    REQUIRE_FALSE (shapeOf ({}).isValid());
    REQUIRE_FALSE (shapeOf ({ silence, silence, silence }).isValid());
}

TEST_CASE ("the mix lays the outro over the intro, phrase on phrase", "[automix]")
{
    const auto outgoing = trackOf (typicalTrack());
    const auto incoming = trackOf (typicalTrack(), 124.0, 0.1);
    const auto plan = planMix (outgoing, incoming);

    REQUIRE (plan.smart);

    // Out at the first downbeat of the outro; in at the first bar with music,
    // skipping the silence in front of it.
    REQUIRE_THAT (plan.outSeconds, WithinAbs (barStartSeconds (outgoing, 58), 1e-9));
    REQUIRE_THAT (plan.inSeconds, WithinAbs (barStartSeconds (incoming, 2), 1e-9));

    // Outro 64 beats, intro 32: the blend lasts as long as the shorter.
    REQUIRE (plan.fadeBeats == 32);
}

TEST_CASE ("a long outro is joined part way, not skipped", "[automix]")
{
    // 40 bars of outro: mixing at its start would leave all 40 unheard.
    std::vector<Bar> levels;
    add (levels, 60, full);
    add (levels, 40, drums);

    const auto analysis = trackOf (levels);
    REQUIRE (shapeOf (barLevels (analysis)).outroBars == 40);

    const auto plan = planMix (analysis, trackOf (typicalTrack()));
    REQUIRE (plan.smart);
    REQUIRE_THAT (plan.outSeconds, WithinAbs (barStartSeconds (analysis, 100 - mostBarsSkipped), 1e-9));
}

TEST_CASE ("tracks that end and start at full energy get a short fade before the end", "[automix]")
{
    std::vector<Bar> levels;
    add (levels, 1, silence);
    add (levels, 61, full);   // music in bars 1 to 61

    const auto analysis = trackOf (levels);
    const auto plan = planMix (analysis, analysis);

    REQUIRE (plan.smart);
    REQUIRE (plan.fadeBeats == shortFadeBeats);

    // Phrases count from bar 1. The last boundary leaving 4 bars of fade
    // before the music stops after bar 61 is bar 57.
    REQUIRE_THAT (plan.outSeconds, WithinAbs (barStartSeconds (analysis, 57), 1e-9));
}

TEST_CASE ("a short intro still gets a fade of a couple of bars", "[automix]")
{
    std::vector<Bar> withOutro;
    add (withOutro, 40, full);
    add (withOutro, 8, drums);

    std::vector<Bar> shortIntro;
    add (shortIntro, 4, drums);
    add (shortIntro, 40, full);

    const auto plan = planMix (trackOf (withOutro), trackOf (shortIntro));

    REQUIRE (plan.smart);
    REQUIRE (plan.fadeBeats == 16);   // a 4 bar intro, under the 8 bar outro
}

TEST_CASE ("a fade is one phrase or two, never part of one", "[automix]")
{
    // A 5 bar outro over a long intro: 20 beats would end mid-phrase, so 16.
    std::vector<Bar> shortOutro;
    add (shortOutro, 44, full);
    add (shortOutro, 5, drums);

    REQUIRE (shapeOf (shortOutro).outroBars == 5);
    REQUIRE (planMix (trackOf (shortOutro), trackOf (typicalTrack())).fadeBeats == shortFadeBeats);

    // Two phrases of outro into two of intro: the long blend.
    std::vector<Bar> longOutro;
    add (longOutro, 40, full);
    add (longOutro, 8, drums);
    REQUIRE (planMix (trackOf (longOutro), trackOf (typicalTrack())).fadeBeats == longFadeBeats);
}

TEST_CASE ("an untrusted grid falls back to the timed fade", "[automix]")
{
    const auto good = trackOf (typicalTrack());

    auto unsure = good;
    unsure.confidence = 0.1f;
    REQUIRE_FALSE (planMix (good, unsure).smart);
    REQUIRE_FALSE (planMix (unsure, good).smart);

    TrackAnalysis noTempo;
    REQUIRE_FALSE (planMix (good, noTempo).smart);

    // Too short to fit a fade after the first phrase.
    std::vector<Bar> tiny;
    add (tiny, 3, full);
    REQUIRE_FALSE (planMix (trackOf (tiny), good).smart);
}

TEST_CASE ("the bass swaps halfway through the fade", "[automix]")
{
    REQUIRE_THAT (incomingLowEq (0.0, 0.6f), WithinAbs (0.0, 1e-6));
    REQUIRE_THAT (incomingLowEq (0.49, 0.6f), WithinAbs (0.0, 1e-6));
    REQUIRE_THAT (incomingLowEq (0.51, 0.6f), WithinAbs (0.6, 1e-6));
    REQUIRE_THAT (incomingLowEq (1.0, 0.6f), WithinAbs (0.6, 1e-6));

    REQUIRE_THAT (outgoingLowEq (0.0, 0.5f), WithinAbs (0.5, 1e-6));
    REQUIRE_THAT (outgoingLowEq (0.49, 0.5f), WithinAbs (0.5, 1e-6));
    REQUIRE_THAT (outgoingLowEq (0.51, 0.5f), WithinAbs (0.0, 1e-6));
}

TEST_CASE ("a smart mix fades at the planned phrase, not a fade's length early", "[automix]")
{
    // AutoMix passes the time to the out point and no lead.
    Situation s;
    s.haveNext = true;
    s.nextReady = true;

    s.remainingSeconds = 0.5;
    REQUIRE (decide (s, 0.0) == Step::wait);

    s.remainingSeconds = 0.0;
    REQUIRE (decide (s, 0.0) == Step::fade);

    // And a skip still goes at once.
    s.remainingSeconds = 90.0;
    s.skipRequested = true;
    REQUIRE (decide (s, 0.0) == Step::fade);
}
