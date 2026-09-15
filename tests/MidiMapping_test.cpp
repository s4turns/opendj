/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>

#include "control/MidiMapping.h"

namespace
{
    opendj::MidiMapping parse (const juce::String& json, juce::StringArray& warnings)
    {
        juce::var parsed;
        REQUIRE (juce::JSON::parse (json, parsed).wasOk());

        opendj::MidiMapping mapping;
        const auto result = opendj::MidiMapping::loadFromJson (parsed, mapping, warnings);
        INFO (result.getErrorMessage());
        REQUIRE (result.wasOk());

        return mapping;
    }

    /** The mapping that ships with the application, found by walking up from the
        test binary the same way the application finds it. */
    juce::File findShippedMapping()
    {
        auto directory = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                             .getParentDirectory();

        while (directory.exists() && directory != directory.getParentDirectory())
        {
            if (const auto candidate = directory.getChildFile ("mappings/roland-dj-202.json");
                candidate.existsAsFile())
                return candidate;

            directory = directory.getParentDirectory();
        }

        return {};
    }
}

TEST_CASE ("a mapping reads hexadecimal and decimal numbers alike", "[midi][mapping]")
{
    juce::StringArray warnings;
    const auto mapping = parse (R"({
        "name": "Test",
        "controls": [
            { "name": "hex",     "status": "0x90", "number": "0x0A", "action": "deck.play_toggle" },
            { "name": "decimal", "status": 176,    "number": 9,      "action": "deck.tempo" }
        ]
    })", warnings);

    REQUIRE (warnings.isEmpty());
    REQUIRE (mapping.controls.size() == 2);
    REQUIRE (mapping.controls[0].status == 0x90);
    REQUIRE (mapping.controls[0].number == 10);
    REQUIRE (mapping.controls[1].status == 0xB0);
    REQUIRE (mapping.controls[1].number == 9);
}

TEST_CASE ("a control's mode defaults to what its action needs", "[midi][mapping]")
{
    juce::StringArray warnings;
    const auto mapping = parse (R"({
        "name": "Test",
        "controls": [
            { "name": "button", "status": "0x90", "number": "0x00", "action": "deck.play_toggle" },
            { "name": "knob",   "status": "0xB0", "number": "0x17", "action": "mixer.eq" },
            { "name": "fader",  "status": "0xB0", "number": "0x09",
              "fineNumber": "0x3B", "action": "deck.tempo" }
        ]
    })", warnings);

    REQUIRE (mapping.controls[0].mode == opendj::ValueMode::button);
    REQUIRE (mapping.controls[1].mode == opendj::ValueMode::absolute);

    // A fine half implies a 14-bit pair without having to say so.
    REQUIRE (mapping.controls[2].mode == opendj::ValueMode::absolute14Bit);
}

TEST_CASE ("one bad control does not take the mapping down with it", "[midi][mapping]")
{
    juce::StringArray warnings;
    const auto mapping = parse (R"({
        "name": "Test",
        "controls": [
            { "name": "good", "status": "0x90", "number": "0x00", "action": "deck.play_toggle" },
            { "name": "typo", "status": "0x90", "number": "0x01", "action": "deck.plya" },
            { "name": "bare", "action": "deck.cue" }
        ]
    })", warnings);

    REQUIRE (mapping.controls.size() == 1);
    REQUIRE (warnings.size() == 2);
    REQUIRE (warnings[0].contains ("typo"));
    REQUIRE (warnings[1].contains ("bare"));
}

TEST_CASE ("a mapping with no usable controls is an error", "[midi][mapping]")
{
    juce::var parsed;
    juce::JSON::parse (R"({ "name": "Test", "controls": [] })", parsed);

    opendj::MidiMapping mapping;
    juce::StringArray warnings;

    REQUIRE (opendj::MidiMapping::loadFromJson (parsed, mapping, warnings).failed());
}

TEST_CASE ("shifted controls take priority over unshifted ones", "[midi][mapping]")
{
    juce::StringArray warnings;
    const auto mapping = parse (R"({
        "name": "Test",
        "controls": [
            { "name": "plain",   "status": "0x94", "number": "0x01", "action": "pad.hotcue" },
            { "name": "shifted", "status": "0x94", "number": "0x01",
              "action": "pad.hotcue_clear", "shift": true }
        ]
    })", warnings);

    const auto* unshifted = mapping.findControl (0x94, 0x01, false);
    const auto* shifted = mapping.findControl (0x94, 0x01, true);

    REQUIRE (unshifted != nullptr);
    REQUIRE (shifted != nullptr);
    REQUIRE (unshifted->name == "plain");
    REQUIRE (shifted->name == "shifted");
}

TEST_CASE ("a 14-bit control answers to both halves of its pair", "[midi][mapping]")
{
    juce::StringArray warnings;
    const auto mapping = parse (R"({
        "name": "Test",
        "controls": [
            { "name": "crossfader", "status": "0xBF", "number": "0x08",
              "fineNumber": "0x58", "action": "mixer.crossfader" }
        ]
    })", warnings);

    REQUIRE (mapping.findControl (0xBF, 0x08, false) != nullptr);
    REQUIRE (mapping.findControl (0xBF, 0x58, false) != nullptr);
    REQUIRE (mapping.findControl (0xBF, 0x09, false) == nullptr);
}

TEST_CASE ("device name hints are matched loosely", "[midi][mapping]")
{
    juce::StringArray warnings;
    const auto mapping = parse (R"({
        "name": "Test",
        "deviceNameHints": ["DJ-202"],
        "controls": [
            { "name": "play", "status": "0x90", "number": "0x00", "action": "deck.play_toggle" }
        ]
    })", warnings);

    REQUIRE (mapping.matchesDeviceName ("DJ-202"));
    REQUIRE (mapping.matchesDeviceName ("2- DJ-202 MIDI 1"));
    REQUIRE_FALSE (mapping.matchesDeviceName ("DDJ-400"));
}

TEST_CASE ("the shipped DJ-202 mapping loads without warnings", "[midi][mapping][dj202]")
{
    const auto file = findShippedMapping();
    INFO ("looked for mappings/roland-dj-202.json above " << file.getFullPathName());
    REQUIRE (file.existsAsFile());

    opendj::MidiMapping mapping;
    juce::StringArray warnings;

    const auto result = opendj::MidiMapping::loadFromFile (file, mapping, warnings);
    INFO (result.getErrorMessage());
    REQUIRE (result.wasOk());
    REQUIRE (warnings.isEmpty());

    REQUIRE (mapping.matchesDeviceName ("DJ-202"));

    // Measured, not taken from the documentation: two turns of the platter on
    // real hardware produced 1590 ticks on CC 0x06, so a revolution is about
    // 800 rather than the 512 the Mixxx mapping states. This is what decides
    // whether a scratch tracks the hand, so it is worth pinning.
    REQUIRE (mapping.jogTicksPerRevolution == 800);

    // The controls a two-deck mix cannot happen without.
    const auto hasControl = [&mapping] (int status, int number)
    {
        return mapping.findControl (status, number, false) != nullptr;
    };

    REQUIRE (hasControl (0x90, 0x00));   // deck A play
    REQUIRE (hasControl (0x91, 0x00));   // deck B play
    REQUIRE (hasControl (0x90, 0x01));   // deck A cue
    REQUIRE (hasControl (0x90, 0x06));   // deck A platter touch
    REQUIRE (hasControl (0xB0, 0x06));   // deck A platter turn
    REQUIRE (hasControl (0xB0, 0x09));   // deck A tempo fader
    REQUIRE (hasControl (0xBF, 0x08));   // crossfader
    REQUIRE (hasControl (0x94, 0x01));   // deck A pad 1

    // The platter has to be relative, or a scratch would read as a seek.
    const auto* platter = mapping.findControl (0xB0, 0x06, false);
    REQUIRE (platter->action == opendj::Action::jogTurn);
    REQUIRE (platter->mode == opendj::ValueMode::relativeOffset);

    // Measured on the hardware: the fader reports 0x3FFF at the top and 0 at
    // the bottom, and Roland prints the + at the bottom. `deck.tempo` wants
    // 1 at the fast end, so the raw value has to be turned over.
    for (int deck = 0; deck < 2; ++deck)
    {
        const auto* tempo = mapping.findControl (0xB0 + deck, 0x09, false);
        REQUIRE (tempo != nullptr);
        REQUIRE (tempo->action == opendj::Action::deckTempo);
        REQUIRE (tempo->mode == opendj::ValueMode::absolute14Bit);
        REQUIRE (tempo->inverted);
    }

    // Both decks map every EQ band, in the right order.
    for (int deck = 0; deck < 2; ++deck)
    {
        for (int band = 0; band < 3; ++band)
        {
            const auto* knob = mapping.findControl (0xB0 + deck, 0x19 - band, false);
            REQUIRE (knob != nullptr);
            REQUIRE (knob->action == opendj::Action::channelEq);
            REQUIRE (knob->deck == deck);
            REQUIRE (knob->slot == band);
        }
    }
}

TEST_CASE ("a mapping reads init and keep-alive messages", "[midi][mapping]")
{
    juce::StringArray warnings;
    const auto mapping = parse (R"({
        "name": "Test",
        "initMessages": [ "F0 00 20 7F 00 F7", "F0 00 20 7F 01 F7" ],
        "keepAlive": { "message": "BF 64 00", "intervalMs": 500 },
        "controls": [ { "name": "play", "status": "0x90", "number": "0x00", "action": "deck.play_toggle" } ]
    })", warnings);

    REQUIRE (warnings.isEmpty());
    REQUIRE (mapping.initMessages.size() == 2);

    // JUCE stores a system exclusive message with its start and end bytes on.
    REQUIRE (mapping.initMessages[0].isSysEx());
    REQUIRE (mapping.initMessages[0].getSysExDataSize() == 4);

    const auto* data = mapping.initMessages[0].getSysExData();
    REQUIRE (data[0] == 0x00);
    REQUIRE (data[1] == 0x20);
    REQUIRE (data[2] == 0x7F);
    REQUIRE (data[3] == 0x00);

    REQUIRE (mapping.keepAliveIntervalMs == 500);
    REQUIRE (mapping.keepAliveMessages.size() == 1);
    REQUIRE (mapping.keepAliveMessages[0].isController());
    REQUIRE (mapping.keepAliveMessages[0].getControllerNumber() == 0x64);
    REQUIRE (mapping.keepAliveMessages[0].getControllerValue() == 0x00);
}

TEST_CASE ("a mapping with no handshake asks for none", "[midi][mapping]")
{
    juce::StringArray warnings;
    const auto mapping = parse (R"({ "name": "Test", "controls": [ { "name": "play", "status": "0x90", "number": "0x00", "action": "deck.play_toggle" } ] })", warnings);

    REQUIRE (mapping.initMessages.empty());
    REQUIRE (mapping.keepAliveMessages.empty());
    REQUIRE (mapping.keepAliveIntervalMs == 0);
}

TEST_CASE ("a malformed handshake message is reported, not sent", "[midi][mapping]")
{
    juce::StringArray warnings;
    const auto mapping = parse (R"({
        "name": "Test",
        "initMessages": [ "F0 00 20 7F 00 F7", "not hex", "F0 11" ],
        "controls": [ { "name": "play", "status": "0x90", "number": "0x00", "action": "deck.play_toggle" } ]
    })", warnings);

    // The good one still goes; the two broken ones are named so they can be fixed.
    REQUIRE (mapping.initMessages.size() == 1);
    REQUIRE (warnings.size() == 2);
}

TEST_CASE ("the shipped DJ-202 mapping wakes the controller up", "[midi][mapping][dj202]")
{
    const auto file = findShippedMapping();

    if (! file.existsAsFile())
        SUCCEED ("mapping file not found beside the test binary");
    else
    {
        opendj::MidiMapping mapping;
        juce::StringArray warnings;
        REQUIRE (opendj::MidiMapping::loadFromFile (file, mapping, warnings).wasOk());

        // Without these the DJ-202 stays in standalone mode and not one button
        // on it produces a MIDI message.
        REQUIRE (mapping.initMessages.size() == 2);
        REQUIRE (mapping.keepAliveMessages.size() == 1);

        // It drops back to standalone about 1.5 seconds after the last one.
        REQUIRE (mapping.keepAliveIntervalMs > 0);
        REQUIRE (mapping.keepAliveIntervalMs <= 1000);
    }
}

TEST_CASE ("a platter reporting absolute position is read as movement", "[midi][mapping][jog]")
{
    juce::StringArray warnings;
    const auto mapping = parse (R"({
        "name": "Test",
        "controls": [
            { "name": "platter", "status": "0xE0", "number": "0x00", "action": "jog.turn",
              "deck": 0, "mode": "position14", "ticksPerRevolution": 16384 }
        ]
    })", warnings);

    REQUIRE (warnings.isEmpty());
    REQUIRE (mapping.controls.size() == 1);
    REQUIRE (mapping.controls[0].mode == opendj::ValueMode::absolutePosition14);
    REQUIRE (mapping.controls[0].ticksPerRevolution == 16384);

    // Pitch bend is addressed by status alone, since its second byte is part of
    // the value rather than a controller number.
    REQUIRE (mapping.findControl (0xE0, 0, false) != nullptr);
}

TEST_CASE ("a control without its own tick count defers to the mapping", "[midi][mapping][jog]")
{
    juce::StringArray warnings;
    const auto mapping = parse (R"({
        "name": "Test",
        "jogTicksPerRevolution": 512,
        "controls": [
            { "name": "platter", "status": "0xb0", "number": "0x06", "action": "jog.turn",
              "deck": 0, "mode": "relative_offset" }
        ]
    })", warnings);

    REQUIRE (mapping.jogTicksPerRevolution == 512);
    REQUIRE (mapping.controls[0].ticksPerRevolution == 0);   // meaning "use the mapping's"
}

TEST_CASE ("the shipped DJ-202 mapping covers the platters and the toggled decks",
           "[midi][mapping][dj202]")
{
    const auto file = findShippedMapping();

    if (! file.existsAsFile())
        SUCCEED ("mapping file not found beside the test binary");
    else
    {
        opendj::MidiMapping mapping;
        juce::StringArray warnings;
        REQUIRE (opendj::MidiMapping::loadFromFile (file, mapping, warnings).wasOk());
        INFO (warnings.joinIntoString ("; "));
        REQUIRE (warnings.isEmpty());

        // The platters are the relative encoder on CC 0x06. They also stream
        // absolute position as pitch bend, but that flows whenever a hand rests
        // on the wheel, so mapping it too would feed the deck a hand's tremor
        // and fight the encoder for the tick scale.
        for (const auto status : { 0xB0, 0xB1 })
        {
            const auto* platter = mapping.findControl (status, 0x06, false);
            REQUIRE (platter != nullptr);
            REQUIRE (platter->action == opendj::Action::jogTurn);
            REQUIRE (platter->mode == opendj::ValueMode::relativeOffset);
        }

        for (const auto status : { 0xE0, 0xE1 })
            REQUIRE (mapping.findControl (status, 0, false) == nullptr);

        // Pressing DECK moves a side onto channels 3 and 4. Leaving those
        // unmapped is what makes a stray press kill half the controller.
        REQUIRE (mapping.findControl (0x92, 0x00, false) != nullptr);   // deck A play
        REQUIRE (mapping.findControl (0x93, 0x01, false) != nullptr);   // deck B cue
        REQUIRE (mapping.findControl (0xB2, 0x17, false) != nullptr);   // deck A EQ high

        // And the filter, which the hardware has always had.
        for (const auto status : { 0xB0, 0xB1 })
        {
            const auto* filter = mapping.findControl (status, 0x1A, false);
            REQUIRE (filter != nullptr);
            REQUIRE (filter->action == opendj::Action::channelFilter);
        }
    }
}

TEST_CASE ("the shipped DJ-202 mapping reaches the loop section", "[midi][mapping][dj202]")
{
    const auto file = findShippedMapping();

    if (! file.existsAsFile())
        SUCCEED ("mapping file not found beside the test binary");
    else
    {
        opendj::MidiMapping mapping;
        juce::StringArray warnings;
        REQUIRE (opendj::MidiMapping::loadFromFile (file, mapping, warnings).wasOk());
        INFO (warnings.joinIntoString ("; "));
        REQUIRE (warnings.isEmpty());

        struct Expected { int status, number; opendj::Action action; };

        // The pads are on 0x94 and 0x95, and the loop section sits with them.
        const Expected expected[]
        {
            { 0x94, 0x15, opendj::Action::loopIn },
            { 0x94, 0x16, opendj::Action::loopOut },
            { 0x94, 0x45, opendj::Action::loopHalve },
            { 0x94, 0x46, opendj::Action::loopDouble },
            { 0x95, 0x15, opendj::Action::loopIn },
            { 0x95, 0x16, opendj::Action::loopOut },
        };

        for (const auto& e : expected)
        {
            INFO (juce::String::toHexString (e.status) << " " << juce::String::toHexString (e.number));
            const auto* control = mapping.findControl (e.status, e.number, false);
            REQUIRE (control != nullptr);
            REQUIRE (control->action == e.action);
        }

        // And a stray press of DECK must not take the loop section away either.
        REQUIRE (mapping.findControl (0x96, 0x15, false) != nullptr);
        REQUIRE (mapping.findControl (0x97, 0x16, false) != nullptr);
    }
}

TEST_CASE ("the shipped DJ-202 mapping reaches the pad modes", "[midi][mapping][dj202]")
{
    const auto file = findShippedMapping();

    if (! file.existsAsFile())
        SUCCEED ("mapping file not found beside the test binary");
    else
    {
        opendj::MidiMapping mapping;
        juce::StringArray warnings;
        REQUIRE (opendj::MidiMapping::loadFromFile (file, mapping, warnings).wasOk());
        INFO (warnings.joinIntoString ("; "));
        REQUIRE (warnings.isEmpty());

        using opendj::Action;
        struct Expected { int status, number; Action action; int deck, slot; };

        // Every pad channel, deck toggled or not: the mode note, loop and roll
        // pads 1 to 4, exit and reloop, and sampler mode's pads and their stops.
        const Expected expected[]
        {
            { 0x94, 0x00, Action::padMode,        0, 0 },
            { 0x97, 0x00, Action::padMode,        1, 0 },
            { 0x94, 0x11, Action::padLoop,        0, 0 },
            { 0x96, 0x12, Action::padLoop,        0, 1 },
            { 0x95, 0x14, Action::padLoop,        1, 3 },
            { 0x94, 0x17, Action::loopToggle,     0, 0 },
            { 0x97, 0x18, Action::loopReloop,     1, 0 },
            { 0x94, 0x21, Action::samplerTrigger, 0, 0 },
            { 0x95, 0x28, Action::samplerTrigger, 1, 7 },
            { 0x96, 0x29, Action::samplerStop,    0, 0 },
            { 0x97, 0x30, Action::samplerStop,    1, 7 },
        };

        for (const auto& e : expected)
        {
            INFO (juce::String::toHexString (e.status) << " " << juce::String::toHexString (e.number));
            const auto* control = mapping.findControl (e.status, e.number, false);
            REQUIRE (control != nullptr);
            REQUIRE (control->action == e.action);
            REQUIRE (control->deck == e.deck);
            REQUIRE (control->slot == e.slot);
        }

        // The mode is carried in the velocity, so it is read as a value.
        REQUIRE (mapping.findControl (0x94, 0x00, false)->mode == opendj::ValueMode::absolute);

        // Loop in and out share the loop pads' note range and are unchanged.
        REQUIRE (mapping.findControl (0x94, 0x15, false)->action == Action::loopIn);
        REQUIRE (mapping.findControl (0x95, 0x16, false)->action == Action::loopOut);
    }
}

//==============================================================================
// The action registry itself. A mapping file names actions as strings, so a
// name that does not round-trip is a control that silently stops working.

TEST_CASE ("every action has a name and every name finds its action", "[mapping][actions]")
{
    using namespace opendj;

    // Action::none is the answer for anything unrecognised, so it is deliberately
    // not in the table and is checked separately below.
    for (int i = 1; i <= (int) Action::shift; ++i)
    {
        const auto action = (Action) i;
        const auto name = toString (action);

        INFO ("action number " << i << " named " << name);

        // A name of "none" here means the table is shorter than the enum, which
        // is what happens when an action is added and its entry is not.
        REQUIRE (name != "none");
        REQUIRE (actionFromString (name) == action);
    }

    REQUIRE (toString (Action::none) == "none");
    REQUIRE (actionFromString ("no such action") == Action::none);
    REQUIRE (actionFromString ("") == Action::none);
}

TEST_CASE ("the four deck and sampler actions are reachable from a mapping", "[mapping][actions]")
{
    using namespace opendj;

    REQUIRE (actionFromString ("deck.select") == Action::deckSelect);
    REQUIRE (actionFromString ("deck.swap") == Action::deckSwap);
    REQUIRE (actionFromString ("mixer.crossfader_assign") == Action::channelCrossfaderAssign);
    REQUIRE (actionFromString ("sampler.trigger") == Action::samplerTrigger);
    REQUIRE (actionFromString ("sampler.stop") == Action::samplerStop);
    REQUIRE (actionFromString ("sampler.gain") == Action::samplerGain);
    REQUIRE (actionFromString ("mic.toggle") == Action::micToggle);
    REQUIRE (actionFromString ("mic.gain") == Action::micGain);
    REQUIRE (actionFromString ("mic.talkover") == Action::micTalkover);

    // The sampler and mic levels are knobs; the pads and switches are buttons.
    REQUIRE (isContinuous (Action::samplerGain));
    REQUIRE (isContinuous (Action::micGain));
    REQUIRE (! isContinuous (Action::micToggle));
    REQUIRE (! isContinuous (Action::micTalkover));
    REQUIRE (! isContinuous (Action::samplerTrigger));
    REQUIRE (! isContinuous (Action::deckSwap));
}

TEST_CASE ("the shipped DJ-202 mapping reaches the deck select and sampler", "[midi][mapping][dj202]")
{
    const auto file = findShippedMapping();

    if (! file.existsAsFile())
        SUCCEED ("mapping file not found beside the test binary");
    else
    {
        opendj::MidiMapping mapping;
        juce::StringArray warnings;
        REQUIRE (opendj::MidiMapping::loadFromFile (file, mapping, warnings).wasOk());
        INFO (warnings.joinIntoString ("; "));
        REQUIRE (warnings.isEmpty());

        // DECK swaps the A/B pair for C/D: the DJ-202 has two jog wheels, so
        // there is never a reason to look at one side of each pair at once.
        for (const auto status : { 0x90, 0x91 })
        {
            const auto* control = mapping.findControl (status, 0x08, false);
            REQUIRE (control != nullptr);
            REQUIRE (control->action == opendj::Action::deckSwap);
        }

        // And it survives a stray press of DECK itself.
        REQUIRE (mapping.findControl (0x92, 0x08, false) != nullptr);
        REQUIRE (mapping.findControl (0x93, 0x08, false) != nullptr);

        // The TR/SAMPLER row is global, not per deck, which is where OpenDJ's
        // sampler lives too.
        struct Expected { int number; int slot; };

        const Expected triggers[]
        {
            { 0x21, 0 }, { 0x22, 1 }, { 0x23, 2 }, { 0x24, 3 },
            { 0x25, 4 }, { 0x26, 5 }, { 0x2E, 6 }, { 0x2F, 7 },
        };

        for (const auto& e : triggers)
        {
            INFO ("note " << juce::String::toHexString (e.number));
            const auto* control = mapping.findControl (0x9F, e.number, false);
            REQUIRE (control != nullptr);
            REQUIRE (control->action == opendj::Action::samplerTrigger);
            REQUIRE (control->slot == e.slot);
        }

        const auto* level = mapping.findControl (0xBF, 0x1A, false);
        REQUIRE (level != nullptr);
        REQUIRE (level->action == opendj::Action::samplerGain);
    }
}

TEST_CASE ("the shipped DJ-202 mapping reaches the FX section", "[midi][mapping][dj202]")
{
    const auto file = findShippedMapping();

    if (! file.existsAsFile())
        SUCCEED ("mapping file not found beside the test binary");
    else
    {
        opendj::MidiMapping mapping;
        juce::StringArray warnings;
        REQUIRE (opendj::MidiMapping::loadFromFile (file, mapping, warnings).wasOk());
        INFO (warnings.joinIntoString ("; "));
        REQUIRE (warnings.isEmpty());

        // FX1 arms echo, FX2 arms reverb, on both sides and on both halves of
        // the deck toggle. Notes 0x98/0x99 are the untoggled pair; 0x9A/0x9B
        // are what DECK moves them to.
        struct Select { int status, number, deck, slot; };

        const Select selects[]
        {
            { 0x98, 0x00, 0, 0 }, { 0x98, 0x01, 0, 1 },
            { 0x99, 0x00, 1, 0 }, { 0x99, 0x01, 1, 1 },
            { 0x9A, 0x00, 0, 0 }, { 0x9A, 0x01, 0, 1 },
            { 0x9B, 0x00, 1, 0 }, { 0x9B, 0x01, 1, 1 },
        };

        for (const auto& e : selects)
        {
            INFO (juce::String::toHexString (e.status) << " " << juce::String::toHexString (e.number));
            const auto* control = mapping.findControl (e.status, e.number, false);
            REQUIRE (control != nullptr);
            REQUIRE (control->action == opendj::Action::channelFxSelect);
            REQUIRE (control->deck == e.deck);
            REQUIRE (control->slot == e.slot);
        }

        // FX3 and FX ON/TAP are recognised by the hardware but have nothing to
        // reach: OpenDJ has two effects, not three, and the depth knob is
        // already how an effect is switched off.
        REQUIRE (mapping.findControl (0x98, 0x02, false) == nullptr);
        REQUIRE (mapping.findControl (0x98, 0x04, false) == nullptr);

        // The depth knob only binds CC 0x00 of the three the hardware sends at
        // once for a single turn, or one hand movement would write the mapping
        // three times.
        for (const auto status : { 0xB8, 0xB9, 0xBA, 0xBB })
        {
            const auto* depth = mapping.findControl (status, 0x00, false);
            REQUIRE (depth != nullptr);
            REQUIRE (depth->action == opendj::Action::channelFxDepth);

            REQUIRE (mapping.findControl (status, 0x01, false) == nullptr);
            REQUIRE (mapping.findControl (status, 0x02, false) == nullptr);
        }
    }
}
