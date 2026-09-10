/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "control/Action.h"

#include <array>

namespace opendj
{

namespace
{
    struct Entry { Action action; const char* name; };

    // One table, read in both directions, so a name can never disagree with
    // itself between the parser and the writer.
    constexpr std::array<Entry, 22> actionNames
    {{
        { Action::deckPlayToggle,   "deck.play_toggle" },
        { Action::deckPlay,         "deck.play" },
        { Action::deckPause,        "deck.pause" },
        { Action::deckCue,          "deck.cue" },
        { Action::deckSync,         "deck.sync" },
        { Action::deckLoadSelected, "deck.load" },
        { Action::deckTempo,        "deck.tempo" },
        { Action::deckTrim,         "deck.trim" },
        { Action::deckSeek,         "deck.seek" },
        { Action::jogTouch,         "jog.touch" },
        { Action::jogTurn,          "jog.turn" },
        { Action::hotCue,           "pad.hotcue" },
        { Action::hotCueClear,      "pad.hotcue_clear" },
        { Action::channelFader,     "mixer.fader" },
        { Action::channelEq,        "mixer.eq" },
        { Action::channelCueToggle, "mixer.cue_toggle" },
        { Action::channelCue,       "mixer.cue" },
        { Action::crossfader,       "mixer.crossfader" },
        { Action::masterGain,       "mixer.master" },
        { Action::cueGain,          "mixer.phones" },
        { Action::cueMix,           "mixer.cue_mix" },
        { Action::shift,            "modifier.shift" }
    }};
}

juce::String toString (Action action)
{
    for (const auto& entry : actionNames)
        if (entry.action == action)
            return entry.name;

    return "none";
}

Action actionFromString (const juce::String& name)
{
    for (const auto& entry : actionNames)
        if (name == entry.name)
            return entry.action;

    return Action::none;
}

bool isContinuous (Action action)
{
    switch (action)
    {
        case Action::deckTempo:
        case Action::deckTrim:
        case Action::deckSeek:
        case Action::jogTurn:
        case Action::channelFader:
        case Action::channelEq:
        case Action::crossfader:
        case Action::masterGain:
        case Action::cueGain:
        case Action::cueMix:
            return true;

        default:
            return false;
    }
}

} // namespace opendj
