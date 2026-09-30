/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <algorithm>

namespace opendj
{

/** The decisions behind the auto crossfader, as plain functions so they can
    be tested without an engine, a clock or a file (the same split as
    `AngleMath.h`). `AutoMix` is the thin part that acts on them. */
namespace automix
{
    /** How long before the fade starts the next track is loaded. Decoding and
        analysing a long track takes a second or two, and a slow disk takes
        longer; this is enough to be ready, with room to spare. */
    inline constexpr double preloadLeadSeconds = 25.0;

    enum class Step
    {
        wait,   ///< nothing to do yet
        load,   ///< start loading the next track onto the idle deck
        fade    ///< start the next track and sweep the crossfader
    };

    struct Situation
    {
        /** Seconds until the track on air ends, at the speed it is playing. */
        double remainingSeconds = 0.0;

        bool haveNext = false;
        bool nextReady = false;
        bool nextLoading = false;

        /** The user asked to move on now rather than at the planned moment. */
        bool skipRequested = false;
    };

    inline Step decide (const Situation& s, double fadeSeconds) noexcept
    {
        if (! s.haveNext)
            return Step::wait;

        if (s.nextReady)
            return s.skipRequested || s.remainingSeconds <= fadeSeconds ? Step::fade : Step::wait;

        if (! s.nextLoading && (s.skipRequested || s.remainingSeconds <= fadeSeconds + preloadLeadSeconds))
            return Step::load;

        return Step::wait;
    }

    /** The fade can never be longer than half of either track, or a short
        track would be mostly overlap. */
    inline double effectiveFadeSeconds (double wanted, double outgoingLength, double incomingLength) noexcept
    {
        const auto shortest = std::min (outgoingLength, incomingLength);
        return std::max (0.5, std::min (wanted, shortest * 0.5));
    }

    /** The crossfader position a fraction of the way through the fade, eased
        at both ends so the mix does not lurch in or out. */
    inline float fadePosition (double progress, float from, float to) noexcept
    {
        const auto p = (float) std::clamp (progress, 0.0, 1.0);
        const auto eased = p * p * (3.0f - 2.0f * p);
        return from + (to - from) * eased;
    }

    /** The playlist entry after `current`, or -1 at the end of a playlist that
        is not looping. */
    inline int nextIndex (int current, int size, bool loop) noexcept
    {
        if (size <= 0)
            return -1;

        if (current + 1 < size)
            return current + 1;

        return loop ? 0 : -1;
    }
}

} // namespace opendj
