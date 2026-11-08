/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "analysis/TapTempo.h"

using Catch::Matchers::WithinAbs;
using opendj::TapTempo;

TEST_CASE ("tap tempo needs four taps before it answers", "[tempo][tap]")
{
    TapTempo taps;

    REQUIRE (taps.tap (10.0) == 0.0);
    REQUIRE (taps.tap (10.5) == 0.0);
    REQUIRE (taps.tap (11.0) == 0.0);
    REQUIRE_THAT (taps.tap (11.5), WithinAbs (120.0, 1.0e-9));
}

TEST_CASE ("tap tempo averages an unsteady finger", "[tempo][tap]")
{
    TapTempo taps;
    const double times[] = { 0.0, 0.47, 0.94, 1.40, 1.88, 2.34 };   // 2.34 s for five beats

    auto bpm = 0.0;

    for (const auto t : times)
        bpm = taps.tap (t);

    REQUIRE_THAT (bpm, WithinAbs (60.0 * 5.0 / 2.34, 1.0e-9));
}

TEST_CASE ("tap tempo follows only the latest eight taps", "[tempo][tap]")
{
    TapTempo taps;

    // Eight slow taps, then eight quick ones: the answer is the quick tempo.
    auto t = 0.0;

    for (int i = 0; i < 8; ++i, t += 0.6)
        taps.tap (t);

    auto bpm = 0.0;

    for (int i = 0; i < 8; ++i, t += 0.5)
        bpm = taps.tap (t);

    REQUIRE (taps.getTapCount() == TapTempo::maximumTaps);
    REQUIRE_THAT (bpm, WithinAbs (120.0, 1.0e-9));
}

TEST_CASE ("a pause starts tap tempo over", "[tempo][tap]")
{
    TapTempo taps;

    for (int i = 0; i < 4; ++i)
        taps.tap (i * 0.5);

    REQUIRE (taps.getBpm() > 0.0);

    // Three seconds later: a fresh start, not one very slow beat.
    REQUIRE (taps.tap (4.5) == 0.0);
    REQUIRE (taps.getTapCount() == 1);
}
