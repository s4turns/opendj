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
    REQUIRE (mapping.jogTicksPerRevolution == 512);

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
