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

TEST_CASE ("the now-playing title stays while its deck is in the mix and leaves with it", "[overlay]")
{
    const auto late = holdSeconds + 30.0;

    // Long after the timer has run out, a deck at full level still shows it.
    REQUIRE_THAT (nowPlayingOpacity (late, 1.0f, true), WithinAbs (1.0, 1e-6));
    // Fading the deck down fades the name.
    REQUIRE_THAT (nowPlayingOpacity (late, 0.4f, true), WithinAbs (0.4, 1e-6));
    // A stopped deck's name goes once the timer is done.
    REQUIRE (nowPlayingOpacity (late, 1.0f, false) == 0.0f);
    // Just after a change it is held in full whatever the fader says.
    REQUIRE_THAT (nowPlayingOpacity (3.0, 0.0f, true), WithinAbs (1.0, 1e-6));
}

TEST_CASE ("the corner turntables are the two decks that matter", "[overlay]")
{
    std::array<DeckLook, 4> decks {};

    REQUIRE (pickCornerDecks (decks) == std::array<int, 2> { -1, -1 });

    decks[2].loaded = true;
    REQUIRE (pickCornerDecks (decks) == std::array<int, 2> { 2, -1 });

    decks[0].loaded = true;
    decks[3].loaded = decks[3].playing = true;
    decks[3].audibility = 0.8f;
    decks[1].loaded = decks[1].playing = true;
    decks[1].audibility = 0.3f;

    // Both playing decks win over loaded ones, lower index on the left.
    REQUIRE (pickCornerDecks (decks) == std::array<int, 2> { 1, 3 });

    // A third playing deck, louder than the quietest, pushes it out.
    decks[0].playing = true;
    decks[0].audibility = 1.0f;
    REQUIRE (pickCornerDecks (decks) == std::array<int, 2> { 0, 3 });
}
