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
    inline constexpr double holdSeconds = 15.0;
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
}

} // namespace opendj
