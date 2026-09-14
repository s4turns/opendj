/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "control/Action.h"

#include <array>
#include <iterator>

namespace opendj
{

namespace
{
    struct Entry { Action action; const char* name; };

    // One table, read in both directions, so a name can never disagree with
    // itself between the parser and the writer.
    constexpr std::array<Entry, 49> actionNames
    {{
        { Action::deckPlayToggle,   "deck.play_toggle" },
        { Action::deckPlay,         "deck.play" },
        { Action::deckPause,        "deck.pause" },
        { Action::deckCue,          "deck.cue" },
        { Action::deckSync,         "deck.sync" },
        { Action::deckLoadSelected, "deck.load" },
        { Action::deckKeyLockToggle, "deck.keylock_toggle" },
        { Action::deckSlipToggle,   "deck.slip_toggle" },
        { Action::deckSelect,       "deck.select" },
        { Action::deckSwap,         "deck.swap" },
        { Action::deckTempo,        "deck.tempo" },
        { Action::deckTrim,         "deck.trim" },
        { Action::deckSeek,         "deck.seek" },
        { Action::deckStem,         "deck.stem" },
        { Action::deckStemToggle,   "deck.stem_toggle" },
        { Action::jogTouch,         "jog.touch" },
        { Action::jogTurn,          "jog.turn" },
        { Action::hotCue,           "pad.hotcue" },
        { Action::hotCueClear,      "pad.hotcue_clear" },
        { Action::loopIn,           "loop.in" },
        { Action::loopOut,          "loop.out" },
        { Action::loopToggle,       "loop.toggle" },
        { Action::loopBeats,        "loop.beats" },
        { Action::loopRoll,         "loop.roll" },
        { Action::loopHalve,        "loop.halve" },
        { Action::loopDouble,       "loop.double" },
        { Action::loopReloop,       "loop.reloop" },
        { Action::channelFader,     "mixer.fader" },
        { Action::channelEq,        "mixer.eq" },
        { Action::channelCueToggle, "mixer.cue_toggle" },
        { Action::channelCue,       "mixer.cue" },
        { Action::channelFilter,    "mixer.filter" },
        { Action::channelEcho,      "mixer.echo" },
        { Action::channelReverb,    "mixer.reverb" },
        { Action::channelFxSelect,  "mixer.fx_select" },
        { Action::channelFxDepth,   "mixer.fx_depth" },
        { Action::channelCrossfaderAssign, "mixer.crossfader_assign" },
        { Action::crossfader,       "mixer.crossfader" },
        { Action::masterGain,       "mixer.master" },
        { Action::cueGain,          "mixer.phones" },
        { Action::cueMix,           "mixer.cue_mix" },
        { Action::samplerTrigger,   "sampler.trigger" },
        { Action::samplerStop,      "sampler.stop" },
        { Action::samplerGain,      "sampler.gain" },
        { Action::micToggle,        "mic.toggle" },
        { Action::micGain,          "mic.gain" },
        { Action::micTalkover,      "mic.talkover" },
        { Action::browseScroll,     "browse.scroll" },
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

double loopBeatsForSlot (int slot)
{
    // A quarter beat up to thirty-two, doubling each step. Short enough at one
    // end to stutter a single hit, long enough at the other to hold a phrase.
    constexpr double lengths[] = { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0 };
    constexpr auto count = static_cast<int> (std::size (lengths));

    return lengths[juce::jlimit (0, count - 1, slot)];
}

bool isContinuous (Action action)
{
    switch (action)
    {
        case Action::deckTempo:
        case Action::deckTrim:
        case Action::deckSeek:
        case Action::deckStem:
        case Action::jogTurn:
        case Action::channelFader:
        case Action::channelEq:
        case Action::channelFilter:
        case Action::channelEcho:
        case Action::channelReverb:
        case Action::channelFxDepth:
        case Action::crossfader:
        case Action::masterGain:
        case Action::cueGain:
        case Action::cueMix:
        case Action::samplerGain:
        case Action::micGain:
        case Action::browseScroll:
            return true;

        default:
            return false;
    }
}

} // namespace opendj
