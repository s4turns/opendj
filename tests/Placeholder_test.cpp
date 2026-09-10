/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).

    The engine classes this suite will cover do not exist yet. This file keeps
    the test target wired into CMake and CI so the first real test has somewhere
    to land; delete it once Mixer and JogWheel tests replace it.
*/

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("test harness is wired up", "[meta]")
{
    REQUIRE (1 + 1 == 2);
}
