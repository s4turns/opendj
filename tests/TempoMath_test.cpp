/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/TempoMath.h"

using Catch::Matchers::WithinAbs;
using namespace opendj::tempo;

TEST_CASE ("close tempos are matched directly", "[tempo]")
{
    REQUIRE_THAT (matchRatio (128.0, 120.0), WithinAbs (128.0 / 120.0, 1e-9));
    REQUIRE_THAT (matchRatio (120.0, 128.0), WithinAbs (120.0 / 128.0, 1e-9));
    REQUIRE_THAT (matchRatio (125.0, 125.0), WithinAbs (1.0, 1e-9));
}

TEST_CASE ("a tempo found an octave out is matched at half or double time", "[tempo]")
{
    // The follower was detected at half its real tempo: 70 for a 140.
    REQUIRE_THAT (matchRatio (140.0, 70.0), WithinAbs (1.0, 1e-9));
    // And the other way round.
    REQUIRE_THAT (matchRatio (70.0, 140.0), WithinAbs (1.0, 1e-9));
    // A leader at 128 and a follower at 70 meets at the leader's 64, 0.914.
    REQUIRE_THAT (matchRatio (128.0, 70.0), WithinAbs (64.0 / 70.0, 1e-9));
}

TEST_CASE ("no tempo means no change", "[tempo]")
{
    REQUIRE (matchRatio (0.0, 120.0) == 1.0);
    REQUIRE (matchRatio (120.0, 0.0) == 1.0);
}

TEST_CASE ("only gentle stretches are used unattended", "[tempo]")
{
    REQUIRE (isGentle (1.05, 0.12));
    REQUIRE (isGentle (0.90, 0.12));
    REQUIRE_FALSE (isGentle (1.30, 0.12));
    REQUIRE_FALSE (isGentle (0.70, 0.12));
    // A limit of zero means no limit, which is what the sync button uses.
    REQUIRE (isGentle (1.9, 0.0));
}
