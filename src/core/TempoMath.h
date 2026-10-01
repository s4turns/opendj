/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <cmath>

namespace opendj
{

/** Tempo matching decisions as plain functions, so they can be tested
    without a deck or a file (the same split as `AngleMath.h`). */
namespace tempo
{
    /** The ratio to play a follower at so it keeps time with a leader.

        Beat detection regularly lands an octave out, finding 70 where a DJ
        would count 140, and a plain ratio between two such numbers plays one
        track at twice or half speed. So the leader's tempo is also tried at
        double and half time, and whichever needs the follower stretched least
        wins: 140 against 70 matches at 1.0 rather than 2.0, and 128 against
        70 matches at 0.91 (the leader's half time, 64), not 1.83. */
    inline double matchRatio (double leaderBpm, double followerBpm) noexcept
    {
        if (leaderBpm <= 0.0 || followerBpm <= 0.0)
            return 1.0;

        auto best = leaderBpm / followerBpm;

        for (const auto multiple : { 0.5, 2.0 })
        {
            const auto candidate = leaderBpm * multiple / followerBpm;

            if (std::abs (std::log (candidate)) < std::abs (std::log (best)))
                best = candidate;
        }

        return best;
    }

    /** Whether a ratio is gentle enough to use unattended. A DJ nudges a few
        percent; stretching a track by a quarter to make a mix work sounds
        worse than the slight mismatch it fixes. */
    inline bool isGentle (double ratio, double maxStretch) noexcept
    {
        return maxStretch <= 0.0 || std::abs (ratio - 1.0) <= maxStretch;
    }
}

} // namespace opendj
