/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include "analysis/Stems.h"
#include "analysis/TrackAnalysis.h"

#include <algorithm>
#include <vector>

namespace opendj
{

/** Where the auto crossfader mixes one track into the next, worked out from
    the beat grid and the energy of each bar, the way a DJ reads a waveform:
    the thin stretch at the end is the outro, the thin stretch at the start
    is the intro, and the mix lays one over the other with phrases lined up.

    "Thin" is measured in the mid and high bands. In dance music the intro and
    outro are usually the drums alone, so the overall level hardly drops (the
    kick carries most of it), but the bass line, synths and vocals that fill
    the mids and highs are missing. Overall level is used only to find silence.

    Plain functions on analysis that is already there when a track is loaded,
    so nothing has to be stored in the library, and the tests need no audio.
    The thresholds are guesses tuned on a handful of tracks; they are named
    here so they can be revisited in one place. */
namespace automix
{
    constexpr int beatsPerBar = 4;
    constexpr int barsPerPhrase = 4;            ///< 16 beats, the unit phrases are counted in
    constexpr float silentBelow = 0.1f;         ///< of the body level: no music at all
    constexpr float thinBelow = 0.7f;           ///< of the body's mids and highs: intro or outro
    constexpr int longFadeBeats = 32;           ///< when the outro and the intro both last that long
    constexpr int shortFadeBeats = 16;          ///< otherwise: one phrase
    constexpr int mostBarsSkipped = 16;         ///< the most of a long outro a mix leaves unplayed
    constexpr float leastConfidence = 0.3f;     ///< below this the beat grid is not trusted

    // Vocals, from the separated vocal stem. The separator lets a little of
    // everything through, so a bar counts as sung when the vocals are a fair
    // share of the whole, not merely when the stem is not silent.
    constexpr float vocalShareAbove = 0.25f;    ///< vocal RMS against the bar's full mix: about -12 dB
    constexpr float vocalFloor = 0.01f;         ///< and above -40 dBFS, so a fade to nothing is not singing
    constexpr int mostBarsSkippedForVocals = 32; ///< how far back a mix may move to keep two singers apart

    // Sections, phrase by phrase, against the track's own main part.
    constexpr float breakdownBelow = 0.6f;      ///< overall level, of the body: the kick has dropped out
    constexpr float chorusAbove = 0.95f;        ///< fullness, of the body's mids and highs: the fullest parts
    constexpr float sungAbove = 0.5f;           ///< share of a phrase's bars with singing to call it sung

    // Level matching: the incoming track is brought to the loudness of the
    // one it replaces, within these limits, so a quiet master is not pushed
    // into clipping and a hot one is not buried.
    constexpr float quietestMatch = 0.5f;       ///< about -6 dB
    constexpr float loudestMatch = 2.0f;        ///< about +6 dB

    /** The start of bar `bar`, in seconds. Bars line up with the downbeat the
        grid is anchored to, and bar 0 is the first whole bar in the track. */
    double barStartSeconds (const TrackAnalysis& analysis, int bar) noexcept;

    /** Mean RMS of one bar, overall and in the bands that say how full it is. */
    struct Bar
    {
        float energy = 0.0f;
        float mid = 0.0f;
        float high = 0.0f;
    };

    /** Every whole bar, from the detail peaks. Empty without a tempo. */
    std::vector<Bar> barLevels (const TrackAnalysis& analysis);

    /** The shape of a track in bars. Invalid when there was nothing to measure. */
    struct TrackShape
    {
        int firstBar = 0;       ///< the first bar with music in it
        int lastBar = -1;       ///< the last bar with music in it
        int introBars = 0;      ///< whole phrases of thin bars from firstBar
        int outroBars = 0;      ///< thin bars ending at lastBar, from a phrase boundary
        float body = 0.0f;      ///< the overall level of the main part, the 75th percentile

        bool isValid() const noexcept { return lastBar >= firstBar && body > 0.0f; }
        int outroStartBar() const noexcept { return lastBar + 1 - outroBars; }
    };

    TrackShape shapeOf (const std::vector<Bar>& bars);

    /** What part of a track a stretch of bars is. Found from the energy of
        each phrase against the track's own main part, and from the vocals
        when the track has been separated, not from recognising repeated
        material, so the names are a reading of the waveform:

        - intro and outro are the thin stretches at either end;
        - a breakdown is where the overall level drops, the kick gone;
        - a chorus is the track's fullest stretch (the drop, in dance music),
          sung or not; with vocals known, a sung stretch nearly as full
          counts too, since the hook is often a touch thinner than the drop;
        - a verse is everything else in the main part. */
    enum class SectionKind
    {
        intro,
        verse,
        chorus,
        breakdown,
        outro
    };

    struct Section
    {
        SectionKind kind = SectionKind::verse;
        int firstBar = 0;
        int bars = 0;

        int endBar() const noexcept { return firstBar + bars; }
    };

    /** The track from its first bar with music to its last, in sections of
        whole phrases (the last may be shorter), neighbours of the same kind
        merged. `vocals` is optional, one entry per bar as vocalBars gives. */
    std::vector<Section> sectionsOf (const std::vector<Bar>& bars, const TrackShape& shape,
                                     const std::vector<bool>& vocals = {});

    /** The kind of section `bar` is in, or verse when it is in none. */
    SectionKind sectionAt (const std::vector<Section>& sections, int bar) noexcept;

    const char* toString (SectionKind kind) noexcept;

    /** The loudness of a track's main part: the 75th percentile of its
        detail RMS, which a long intro, outro or breakdown does not drag
        down. Needs no beat grid. 0 for an empty analysis. */
    float bodyLevel (const TrackAnalysis& analysis);

    /** The gain that brings `incoming` to the loudness of `outgoing`, within
        quietestMatch and loudestMatch. 1 when either cannot be measured. */
    float levelMatch (const TrackAnalysis& outgoing, const TrackAnalysis& incoming);

    /** Which bars have singing in them, one entry per bar of the grid, from a
        track's separated vocals. Empty without a tempo. */
    std::vector<bool> vocalBars (const SeparatedTrack& stems, const TrackAnalysis& analysis);

    struct MixPlan
    {
        bool smart = false;        ///< false: fade at a fixed time before the end, as before
        double outSeconds = 0.0;   ///< in the outgoing track, where the fade starts
        double inSeconds = 0.0;    ///< in the incoming track, where it starts playing
        int fadeBeats = 0;

        /** What the outgoing track is doing where the fade starts, for the
            plan's description. */
        SectionKind outSection = SectionKind::outro;

        /** Bars of the fade where both tracks would be singing at once. */
        int vocalClashBars = 0;

        /** When a clash could not be planned away: the bar into the fade where
            the incoming vocals start, by which the outgoing vocals must be
            gone. -1 when nothing needs doing. */
        int vocalHandoffBar = -1;
    };

    /** Plans the mix from the track on air into the next one. Without an
        outro to mix from, it moves the fade back a phrase or so rather than
        cut into the outgoing track's chorus. With both tracks' vocal bars, it
        also keeps two singers from overlapping: it looks at the phrases
        either side and the shorter fade for the mix with the fewest bars sung
        over each other, and failing that marks where the outgoing vocals have
        to make way. Two singers apart matters more than a chorus kept whole. */
    MixPlan planMix (const TrackAnalysis& outgoing, const TrackAnalysis& incoming,
                     const std::vector<bool>& outgoingVocals = {},
                     const std::vector<bool>& incomingVocals = {});

    /** The incoming channel's low EQ a fraction of the way through the fade,
        as a knob position: killed for the first half, then back to where the
        user had it. The outgoing channel gets the mirror image. Two kick drums
        and bass lines at full together are the muddiest part of a blend. */
    inline float incomingLowEq (double progress, float usersSetting) noexcept
    {
        return progress < 0.5 ? 0.0f : usersSetting;
    }

    inline float outgoingLowEq (double progress, float usersSetting) noexcept
    {
        return progress < 0.5 ? usersSetting : 0.0f;
    }

    /** The outgoing vocal stem's gain `barsIntoFade` bars into the fade, when
        the plan has a handoff: as the user set it, then down to nothing over
        the bar before the incoming vocals start. A handoff on the first bar
        has no bar before it, so the outgoing vocals go over its first beat. */
    inline float outgoingVocalGain (double barsIntoFade, int handoffBar, float usersSetting) noexcept
    {
        if (handoffBar < 0)
            return usersSetting;

        const auto left = handoffBar == 0 ? 1.0 - barsIntoFade * beatsPerBar
                                          : (double) handoffBar - barsIntoFade;
        return usersSetting * (float) std::clamp (left, 0.0, 1.0);
    }
}

} // namespace opendj
