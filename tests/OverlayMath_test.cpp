/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ui/OverlayMath.h"

using Catch::Matchers::WithinAbs;
using namespace opendj::overlay;

TEST_CASE ("a title fades in, holds, and fades out on the timer alone", "[overlay]")
{
    REQUIRE_THAT (timedOpacity (0.0), WithinAbs (0.0, 1e-6));
    REQUIRE_THAT (timedOpacity (fadeSeconds / 2), WithinAbs (0.5, 1e-6));
    REQUIRE_THAT (timedOpacity (holdSeconds - 0.1), WithinAbs (1.0, 1e-6));
    REQUIRE_THAT (timedOpacity (holdSeconds + fadeSeconds / 2), WithinAbs (0.5, 1e-6));
    REQUIRE (timedOpacity (holdSeconds + fadeSeconds + 1.0) == 0.0f);
}
