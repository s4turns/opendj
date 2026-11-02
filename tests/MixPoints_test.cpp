/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/AutoMixMath.h"
#include "core/MixPoints.h"

#include <tuple>

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

namespace
{
    /** Separated stems on a 120 BPM grid with the downbeat at 0.25 s (2 s
        bars): drums all the way through, and vocals at `level` in the bars
        listed. */
    SeparatedTrack stemsWith (int numBars, const std::vector<std::pair<int, float>>& vocalBarsAndLevels)
    {
        SeparatedTrack track;
        track.sampleRate = 44100.0;
        const auto barSamples = (int) (2.0 * track.sampleRate);
        const auto numSamples = (int) (0.25 * track.sampleRate) + barSamples * numBars;

        for (auto& stem : track.stems)
        {
            stem.setSize (2, numSamples);
            stem.clear();
        }

        for (int i = 0; i < numSamples; ++i)
        {
            const auto t = i / track.sampleRate;
            const auto drum = 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 60.0 * t);

            for (int ch = 0; ch < 2; ++ch)
                track.stems[(size_t) Stem::drums].setSample (ch, i, drum);
        }

        for (const auto& [bar, level] : vocalBarsAndLevels)
        {
            const auto first = (int) (0.25 * track.sampleRate) + bar * barSamples;

            for (int i = first; i < first + barSamples && i < numSamples; ++i)
            {
                const auto t = i / track.sampleRate;
                const auto voice = level * (float) std::sin (juce::MathConstants<double>::twoPi * 330.0 * t);

                for (int ch = 0; ch < 2; ++ch)
                    track.stems[(size_t) Stem::vocals].setSample (ch, i, voice);
            }
        }

        return track;
    }

    std::vector<bool> sungBars (int count, int from, int to)
    {
        std::vector<bool> bars ((size_t) count, false);

        for (int b = from; b <= to && b < count; ++b)
            bars[(size_t) b] = true;

        return bars;
    }
}

TEST_CASE ("vocals are found bar by bar, and bleed is not singing", "[automix][vocals]")
{
    const auto analysis = trackOf ({}, 120.0, 0.25);

    // Bars 3 to 5 sung; bar 7 has a trace of vocal bleed under the drums.
    const auto stems = stemsWith (10, { { 3, 0.3f }, { 4, 0.3f }, { 5, 0.3f }, { 7, 0.02f } });
    const auto sung = vocalBars (stems, analysis);

    REQUIRE (sung.size() == 10);

    for (int b = 0; b < 10; ++b)
        REQUIRE (sung[(size_t) b] == (b >= 3 && b <= 5));

    // A voice alone, but almost silent, is not singing either.
    auto quiet = stemsWith (4, { { 1, 0.005f } });
    quiet.stems[(size_t) Stem::drums].clear();
    REQUIRE_FALSE (vocalBars (quiet, analysis)[1]);

    // No grid, no bars.
    REQUIRE (vocalBars (stems, TrackAnalysis()).empty());
}

TEST_CASE ("without vocals the plan is the one made from the shape", "[automix][vocals]")
{
    const auto outgoing = trackOf (typicalTrack());
    const auto incoming = trackOf (typicalTrack(), 124.0, 0.1);

    const auto plain = planMix (outgoing, incoming);
    const auto silent = planMix (outgoing, incoming, sungBars (77, 0, -1), sungBars (77, 0, -1));

    REQUIRE (silent.smart);
    REQUIRE_THAT (silent.outSeconds, WithinAbs (plain.outSeconds, 1e-9));
    REQUIRE (silent.fadeBeats == plain.fadeBeats);
    REQUIRE (silent.vocalClashBars == 0);
    REQUIRE (silent.vocalHandoffBar == -1);
}

TEST_CASE ("the mix moves a phrase on to clear the outgoing vocals", "[automix][vocals]")
{
    const auto outgoing = trackOf (typicalTrack());
    const auto incoming = trackOf (typicalTrack(), 124.0, 0.1);

    // Sung over the first phrase of the outro, where the plan would mix, and
    // the incoming track sings from its first bar.
    const auto plan = planMix (outgoing, incoming, sungBars (77, 58, 61), sungBars (77, 2, 5));

    REQUIRE (plan.smart);
    REQUIRE_THAT (plan.outSeconds, WithinAbs (barStartSeconds (outgoing, 62), 1e-9));
    REQUIRE (plan.fadeBeats == 32);
    REQUIRE (plan.vocalClashBars == 0);
    REQUIRE (plan.vocalHandoffBar == -1);
}

TEST_CASE ("vocals that cannot be kept apart get a short fade and a handoff", "[automix][vocals]")
{
    const auto outgoing = trackOf (typicalTrack());
    const auto incoming = trackOf (typicalTrack(), 124.0, 0.1);

    // Singing to the very end, into a track that sings from its first bar:
    // every mix overlaps, so the overlap is made as short as it can be, where
    // the shape put it, and the outgoing vocals make way at once.
    auto plan = planMix (outgoing, incoming, sungBars (77, 42, 73), sungBars (77, 2, 9));

    REQUIRE (plan.smart);
    REQUIRE (plan.fadeBeats == 16);
    REQUIRE_THAT (plan.outSeconds, WithinAbs (barStartSeconds (outgoing, 58), 1e-9));
    REQUIRE (plan.vocalClashBars == 4);
    REQUIRE (plan.vocalHandoffBar == 0);

    // The incoming vocals start two bars in: the outgoing ones go by then.
    plan = planMix (outgoing, incoming, sungBars (77, 42, 73), sungBars (77, 4, 9));

    REQUIRE (plan.fadeBeats == 16);
    REQUIRE (plan.vocalClashBars == 2);
    REQUIRE (plan.vocalHandoffBar == 2);
}

TEST_CASE ("the outgoing vocals fade over the bar before the handoff", "[automix][vocals]")
{
    REQUIRE_THAT (outgoingVocalGain (3.0, -1, 0.8f), WithinAbs (0.8, 1e-6));

    REQUIRE_THAT (outgoingVocalGain (0.5, 2, 1.0f), WithinAbs (1.0, 1e-6));
    REQUIRE_THAT (outgoingVocalGain (1.5, 2, 1.0f), WithinAbs (0.5, 1e-6));
    REQUIRE_THAT (outgoingVocalGain (2.0, 2, 1.0f), WithinAbs (0.0, 1e-6));
    REQUIRE_THAT (outgoingVocalGain (3.5, 2, 1.0f), WithinAbs (0.0, 1e-6));

    // At the very start there is no bar before: over the first beat instead.
    REQUIRE_THAT (outgoingVocalGain (0.0, 0, 1.0f), WithinAbs (1.0, 1e-6));
    REQUIRE_THAT (outgoingVocalGain (0.125, 0, 1.0f), WithinAbs (0.5, 1e-6));
    REQUIRE_THAT (outgoingVocalGain (0.25, 0, 1.0f), WithinAbs (0.0, 1e-6));

    // The user's own setting is where it starts from.
    REQUIRE_THAT (outgoingVocalGain (1.5, 2, 0.6f), WithinAbs (0.3, 1e-6));
}

TEST_CASE ("a matched deck needs key lock unless it plays at its own speed", "[automix][keylock]")
{
    REQUIRE_FALSE (needsKeyLock (1.0));
    REQUIRE_FALSE (needsKeyLock (1.0005));
    REQUIRE (needsKeyLock (1.05));
    REQUIRE (needsKeyLock (0.9));
}

//==============================================================================
// Sections and level matching
//==============================================================================

namespace
{
    const Bar verse { 0.9f, 0.7f, 0.7f };   ///< the main part, short of its fullest

    /** Intro, verse, chorus, breakdown, chorus, outro, in phrases. */
    std::vector<Bar> songTrack()
    {
        std::vector<Bar> levels;
        add (levels, 2, silence);
        add (levels, 8, drums);
        add (levels, 16, verse);
        add (levels, 16, full);
        add (levels, 8, breakdown);
        add (levels, 16, full);
        add (levels, 16, drums);
        add (levels, 2, silence);
        return levels;
    }
}

TEST_CASE ("a track is read as intro, verse, chorus, breakdown and outro", "[automix][sections]")
{
    const auto bars = barLevels (trackOf (songTrack()));
    const auto sections = sectionsOf (bars, shapeOf (bars));

    const std::vector<std::tuple<SectionKind, int, int>> expected
    {
        { SectionKind::intro, 2, 8 },
        { SectionKind::verse, 10, 16 },
        { SectionKind::chorus, 26, 16 },
        { SectionKind::breakdown, 42, 8 },
        { SectionKind::chorus, 50, 16 },
        { SectionKind::outro, 66, 16 },
    };

    REQUIRE (sections.size() == expected.size());

    for (size_t i = 0; i < expected.size(); ++i)
    {
        INFO ("section " << i << " is a " << toString (sections[i].kind) << " at bar " << sections[i].firstBar);
        REQUIRE (sections[i].kind == std::get<0> (expected[i]));
        REQUIRE (sections[i].firstBar == std::get<1> (expected[i]));
        REQUIRE (sections[i].bars == std::get<2> (expected[i]));
    }

    REQUIRE (sectionAt (sections, 30) == SectionKind::chorus);
    REQUIRE (sectionAt (sections, 45) == SectionKind::breakdown);
}

TEST_CASE ("a sung stretch nearly as full as the drop is a chorus", "[automix][sections][vocals]")
{
    std::vector<Bar> levels;
    add (levels, 8, drums);
    add (levels, 8, Bar { 1.0f, 0.9f, 0.9f });    // the hook, a touch thinner
    add (levels, 32, full);
    add (levels, 8, Bar { 1.0f, 0.9f, 0.9f });
    add (levels, 8, drums);

    const auto bars = barLevels (trackOf (levels));
    const auto shape = shapeOf (bars);

    REQUIRE (sectionAt (sectionsOf (bars, shape), 12) == SectionKind::verse);

    std::vector<bool> sung (bars.size(), false);

    for (int b = 8; b < 16; ++b)
        sung[(size_t) b] = true;

    const auto withVocals = sectionsOf (bars, shape, sung);
    REQUIRE (sectionAt (withVocals, 12) == SectionKind::chorus);
    REQUIRE (sectionAt (withVocals, 52) == SectionKind::verse);   // the same, unsung
}

TEST_CASE ("with no outro the mix starts after the last chorus, not in it", "[automix][sections]")
{
    // The track ends at full tilt: verse, then a closing chorus, no outro.
    std::vector<Bar> levels;
    add (levels, 2, silence);
    add (levels, 8, drums);
    add (levels, 16, full);
    add (levels, 8, verse);
    add (levels, 8, full);
    add (levels, 2, silence);

    const auto outgoing = trackOf (levels);
    const auto plan = planMix (outgoing, trackOf (typicalTrack()));

    REQUIRE (plan.smart);
    REQUIRE (plan.fadeBeats == 32);

    // The last phrase that leaves a whole fade is bar 34, inside the closing
    // chorus; the verse before it starts at bar 26.
    REQUIRE_THAT (plan.outSeconds, WithinAbs (barStartSeconds (outgoing, 26), 1e-9));
    REQUIRE (plan.outSection == SectionKind::verse);
}

TEST_CASE ("a mix from an outro is described as one", "[automix][sections]")
{
    const auto plan = planMix (trackOf (typicalTrack()), trackOf (typicalTrack()));

    REQUIRE (plan.smart);
    REQUIRE (plan.outSection == SectionKind::outro);
}

TEST_CASE ("the incoming track is matched to the loudness of the outgoing one", "[automix][level]")
{
    auto quieter = typicalTrack();

    for (auto& bar : quieter)
        bar.energy *= 0.5f;

    const auto loud = trackOf (typicalTrack());
    const auto quiet = trackOf (quieter);

    REQUIRE_THAT (levelMatch (loud, quiet), WithinAbs (2.0, 1e-4));
    REQUIRE_THAT (levelMatch (quiet, loud), WithinAbs (0.5, 1e-4));
    REQUIRE_THAT (levelMatch (loud, loud), WithinAbs (1.0, 1e-6));

    // Never more than about 6 dB either way.
    for (auto& bar : quieter)
        bar.energy *= 0.1f;

    REQUIRE_THAT (levelMatch (loud, trackOf (quieter)), WithinAbs (loudestMatch, 1e-6));

    // And nothing to measure is no change.
    REQUIRE_THAT (levelMatch (loud, TrackAnalysis {}), WithinAbs (1.0, 1e-6));
}
