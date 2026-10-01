/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/AutoMixMath.h"

using Catch::Matchers::WithinAbs;
using namespace opendj::automix;

TEST_CASE ("the next track is loaded at once and faded in on time", "[automix]")
{
    Situation s;
    s.haveNext = true;
    const auto fade = 12.0;

    // However much of the current track is left, the next one is cued now.
    s.remainingSeconds = 300.0;
    REQUIRE (decide (s, fade) == Step::load);

    // Already loading or loaded: do not ask again.
    s.nextLoading = true;
    REQUIRE (decide (s, fade) == Step::wait);

    s.nextLoading = false;
    s.nextReady = true;
    REQUIRE (decide (s, fade) == Step::wait);

    s.remainingSeconds = fade - 0.1;
    REQUIRE (decide (s, fade) == Step::fade);
}

TEST_CASE ("a next track that arrived late is still faded in at once", "[automix]")
{
    Situation s;
    s.haveNext = true;
    s.nextReady = true;
    s.remainingSeconds = 3.0;   // well inside the fade window already

    REQUIRE (decide (s, 12.0) == Step::fade);
}

TEST_CASE ("nothing happens at the end of a playlist that is not looping", "[automix]")
{
    Situation s;
    s.haveNext = false;
    s.remainingSeconds = 1.0;

    REQUIRE (decide (s, 12.0) == Step::wait);
}

TEST_CASE ("skipping loads and then fades without waiting for the end", "[automix]")
{
    Situation s;
    s.haveNext = true;
    s.skipRequested = true;
    s.remainingSeconds = 200.0;

    REQUIRE (decide (s, 12.0) == Step::load);

    s.nextReady = true;
    REQUIRE (decide (s, 12.0) == Step::fade);
}

TEST_CASE ("the fade is shortened for short tracks", "[automix]")
{
    REQUIRE_THAT (effectiveFadeSeconds (12.0, 300.0, 240.0), WithinAbs (12.0, 1e-9));
    REQUIRE_THAT (effectiveFadeSeconds (12.0, 300.0, 10.0), WithinAbs (5.0, 1e-9));
    REQUIRE_THAT (effectiveFadeSeconds (12.0, 0.4, 0.4), WithinAbs (0.5, 1e-9));
}

TEST_CASE ("the crossfader sweeps from one side to the other, eased", "[automix]")
{
    REQUIRE_THAT (fadePosition (0.0, -1.0f, 1.0f), WithinAbs (-1.0, 1e-6));
    REQUIRE_THAT (fadePosition (0.5, -1.0f, 1.0f), WithinAbs (0.0, 1e-6));
    REQUIRE_THAT (fadePosition (1.0, -1.0f, 1.0f), WithinAbs (1.0, 1e-6));
    REQUIRE_THAT (fadePosition (2.0, -1.0f, 1.0f), WithinAbs (1.0, 1e-6));

    // Eased: slower than a straight line at the start.
    REQUIRE (fadePosition (0.1, -1.0f, 1.0f) < -0.8f);

    // And it works towards the other side too.
    REQUIRE_THAT (fadePosition (1.0, 1.0f, -1.0f), WithinAbs (-1.0, 1e-6));
}

TEST_CASE ("the playlist advances and wraps only when asked to loop", "[automix]")
{
    REQUIRE (nextIndex (0, 3, false) == 1);
    REQUIRE (nextIndex (2, 3, false) == -1);
    REQUIRE (nextIndex (2, 3, true) == 0);
    REQUIRE (nextIndex (0, 1, true) == 0);
    REQUIRE (nextIndex (0, 0, true) == -1);
}
