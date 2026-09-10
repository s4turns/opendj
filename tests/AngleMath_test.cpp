/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ui/AngleMath.h"

using Catch::Matchers::WithinAbs;
using namespace opendj::angles;

namespace
{
    constexpr double pi = juce::MathConstants<double>::pi;
    constexpr double quarterTurn = pi / 2.0;
}

TEST_CASE ("angles fold into a single turn", "[platter][angles]")
{
    REQUIRE_THAT (wrap (0.0), WithinAbs (0.0, 1.0e-9));
    REQUIRE_THAT (wrap (twoPi), WithinAbs (0.0, 1.0e-9));
    REQUIRE_THAT (wrap (twoPi + quarterTurn), WithinAbs (quarterTurn, 1.0e-9));
    REQUIRE_THAT (wrap (-quarterTurn), WithinAbs (twoPi - quarterTurn, 1.0e-9));
    REQUIRE_THAT (wrap (-5.0 * twoPi - quarterTurn), WithinAbs (twoPi - quarterTurn, 1.0e-9));
}

TEST_CASE ("dragging takes the short way round", "[platter][angles]")
{
    REQUIRE_THAT (shortestDelta (0.0, quarterTurn), WithinAbs (quarterTurn, 1.0e-9));
    REQUIRE_THAT (shortestDelta (quarterTurn, 0.0), WithinAbs (-quarterTurn, 1.0e-9));
}

TEST_CASE ("dragging across twelve o'clock is a small step, not a whole turn", "[platter][angles]")
{
    // This is the bug every circular drag control is born with: without the
    // wrap, nudging past the top reads as a full turn backwards and the track
    // leaps two seconds.
    const auto justBefore = twoPi - 0.05;
    const auto justAfter = 0.05;

    REQUIRE_THAT (shortestDelta (justBefore, justAfter), WithinAbs (0.1, 1.0e-9));
    REQUIRE_THAT (shortestDelta (justAfter, justBefore), WithinAbs (-0.1, 1.0e-9));
}

TEST_CASE ("half a turn is the largest step a drag can report", "[platter][angles]")
{
    for (double from = 0.0; from < twoPi; from += 0.37)
        for (double to = 0.0; to < twoPi; to += 0.41)
            REQUIRE (std::abs (shortestDelta (from, to)) <= pi + 1.0e-9);
}

TEST_CASE ("a full turn of the mouse is a full turn of ticks", "[platter][angles]")
{
    REQUIRE_THAT (toTicks (twoPi, 512), WithinAbs (512.0, 1.0e-9));
    REQUIRE_THAT (toTicks (pi, 512), WithinAbs (256.0, 1.0e-9));
    REQUIRE_THAT (toTicks (-quarterTurn, 512), WithinAbs (-128.0, 1.0e-9));
}

TEST_CASE ("the platter turns once every 1.8 seconds", "[platter][angles]")
{
    // 33 1/3 rpm, so the marker is back at the top after 1.8 seconds and
    // halfway round after 0.9.
    REQUIRE_THAT (platterAngle (0.0), WithinAbs (0.0, 1.0e-9));
    REQUIRE_THAT (platterAngle (0.9), WithinAbs (pi, 1.0e-9));
    REQUIRE_THAT (platterAngle (1.8), WithinAbs (0.0, 1.0e-9));
    REQUIRE_THAT (platterAngle (5.4), WithinAbs (0.0, 1.0e-9));
}

TEST_CASE ("a point's angle is measured clockwise from the top", "[platter][angles]")
{
    constexpr double cx = 100.0, cy = 100.0;

    REQUIRE_THAT (angleFromCentre (100.0,   0.0, cx, cy), WithinAbs (0.0, 1.0e-6));
    REQUIRE_THAT (angleFromCentre (200.0, 100.0, cx, cy), WithinAbs (quarterTurn, 1.0e-6));
    REQUIRE_THAT (angleFromCentre (100.0, 200.0, cx, cy), WithinAbs (pi, 1.0e-6));
    REQUIRE_THAT (angleFromCentre (  0.0, 100.0, cx, cy), WithinAbs (3.0 * quarterTurn, 1.0e-6));
}

TEST_CASE ("dragging a platter round once moves the track 1.8 seconds", "[platter][angles]")
{
    // The round trip the platter actually performs: a mouse position becomes an
    // angle, the angle becomes ticks, and the deck reads the ticks as vinyl.
    constexpr double cx = 100.0, cy = 100.0, radius = 100.0;
    const auto ticksPerRevolution = 512;

    auto previous = angleFromCentre (cx, cy - radius, cx, cy);
    auto totalTicks = 0.0;

    // Sixteen steps clockwise, all the way round.
    for (int step = 1; step <= 16; ++step)
    {
        const auto angle = step * twoPi / 16.0;
        const auto x = cx + radius * std::sin (angle);
        const auto y = cy - radius * std::cos (angle);

        const auto current = angleFromCentre (x, y, cx, cy);
        totalTicks += toTicks (shortestDelta (previous, current), ticksPerRevolution);
        previous = current;
    }

    REQUIRE_THAT (totalTicks, WithinAbs (512.0, 1.0e-3));

    // The deck turns one revolution of ticks into one revolution of vinyl.
    const auto seconds = totalTicks / ticksPerRevolution * 1.8;
    REQUIRE_THAT (seconds, WithinAbs (1.8, 1.0e-5));
}
