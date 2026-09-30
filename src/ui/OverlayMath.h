/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

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

    /** What the picker needs to know about a deck. */
    struct DeckLook
    {
        bool loaded = false;
        bool playing = false;
        float audibility = 0.0f;
    };

    /** The two decks whose turntables are drawn, lower index first, or -1
        for an empty corner. Playing decks win, loudest first; loaded decks
        fill what is left; ties go to the lower deck. */
    template <std::size_t N>
    inline std::array<int, 2> pickCornerDecks (const std::array<DeckLook, N>& decks) noexcept
    {
        std::array<int, N> order {};

        for (std::size_t i = 0; i < N; ++i)
            order[i] = (int) i;

        const auto rank = [&] (int i)
        {
            const auto& d = decks[(std::size_t) i];
            return d.playing ? 2.0f + d.audibility : d.loaded ? 1.0f : 0.0f;
        };

        std::stable_sort (order.begin(), order.end(), [&] (int a, int b) { return rank (a) > rank (b); });

        std::array<int, 2> picked { -1, -1 };
        std::size_t count = 0;

        for (const auto i : order)
            if (count < 2 && decks[(std::size_t) i].loaded)
                picked[count++] = i;

        if (picked[1] >= 0 && picked[0] > picked[1])
            std::swap (picked[0], picked[1]);

        return picked;
    }
}

} // namespace opendj
