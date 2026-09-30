/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <algorithm>

namespace opendj
{

/** The decisions behind the on-video overlay, free of any drawing so they can
    be tested without a graphics module (the same split as `AngleMath.h`). */
namespace overlay
{
    /** How long a title is held in full after it appears, and how long each
        fade takes. */
    inline constexpr double holdSeconds = 12.0;
    inline constexpr double fadeSeconds = 1.0;

    /** 0 to 1: a title's visibility `seconds` after it appeared, on the
        timer alone: fade in, hold, fade out. */
    inline float timedOpacity (double seconds) noexcept
    {
        if (seconds < 0.0 || seconds >= holdSeconds + fadeSeconds)
            return 0.0f;

        if (seconds < fadeSeconds)
            return (float) (seconds / fadeSeconds);

        if (seconds < holdSeconds)
            return 1.0f;

        return (float) (1.0 - (seconds - holdSeconds) / fadeSeconds);
    }

    /** The now-playing title's visibility. It comes in on the timer, and then
        stays for as long as its deck is actually in the mix: fading the
        track out fades the name out, as VirtualDJ's live skin does, rather
        than the name leaving after a fixed time while the song carries on. */
    inline float nowPlayingOpacity (double secondsSinceChange, float audibility, bool playing) noexcept
    {
        const auto fadeIn = (float) std::clamp (secondsSinceChange / fadeSeconds, 0.0, 1.0);
        const auto held = secondsSinceChange >= 0.0 && secondsSinceChange < holdSeconds ? 1.0f : 0.0f;
        const auto inMix = playing ? std::clamp (audibility, 0.0f, 1.0f) : 0.0f;
        return fadeIn * std::max (held, inMix);
    }
}

} // namespace opendj
