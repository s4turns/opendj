/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "control/ActionDispatcher.h"
#include "core/AudioEngine.h"

using namespace opendj;

TEST_CASE ("the FX depth knob starts armed to echo", "[control][fx]")
{
    AudioEngine engine;
    ActionDispatcher dispatcher (engine);

    REQUIRE (dispatcher.getFxDepthTarget (0) == 0);

    dispatcher.dispatch ({ Action::channelFxDepth, 0, 0, 0.6f });

    REQUIRE (engine.getMixer().getChannelEcho (0) > 0.0f);
    REQUIRE (engine.getMixer().getChannelReverb (0) == 0.0f);
}

TEST_CASE ("arming reverb moves the depth knob to reverb, not echo", "[control][fx]")
{
    AudioEngine engine;
    ActionDispatcher dispatcher (engine);

    // Echo first, so the test would fail if arming reverb only added a second
    // target rather than replacing the first.
    dispatcher.dispatch ({ Action::channelFxDepth, 0, 0, 0.5f });
    REQUIRE (engine.getMixer().getChannelEcho (0) > 0.0f);

    dispatcher.dispatch ({ Action::channelFxSelect, 0, 1, 1.0f });
    REQUIRE (dispatcher.getFxDepthTarget (0) == 1);

    dispatcher.dispatch ({ Action::channelFxDepth, 0, 0, 0.8f });

    REQUIRE (engine.getMixer().getChannelReverb (0) > 0.0f);

    // Echo is left exactly where the first turn put it: arming a different
    // effect must not touch the one that is no longer selected.
    REQUIRE_THAT (engine.getMixer().getChannelEcho (0), Catch::Matchers::WithinAbs (0.5, 0.0001));
}

TEST_CASE ("a select button only arms an effect on its own channel", "[control][fx]")
{
    AudioEngine engine;
    ActionDispatcher dispatcher (engine);

    dispatcher.dispatch ({ Action::channelFxSelect, 0, 1, 1.0f });

    REQUIRE (dispatcher.getFxDepthTarget (0) == 1);
    REQUIRE (dispatcher.getFxDepthTarget (1) == 0);   // untouched, still echo
}

TEST_CASE ("a select button releasing does not re-arm the effect it named", "[control][fx]")
{
    AudioEngine engine;
    ActionDispatcher dispatcher (engine);

    dispatcher.dispatch ({ Action::channelFxSelect, 0, 1, 1.0f });   // press
    dispatcher.dispatch ({ Action::channelFxSelect, 0, 0, 0.0f });   // a different button's release

    // The release carries value 0, which reads as "not pressed" for any
    // button action; the selection made by the press must still stand.
    REQUIRE (dispatcher.getFxDepthTarget (0) == 1);
}
