/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_core/juce_core.h>

namespace opendj
{

/** Everything a user can do, named once.

    Buttons, keys and MIDI messages all turn into one of these rather than
    calling a deck or the mixer directly. That is what lets a mapping file be
    data instead of code, and what stops the same behaviour being written twice
    with two slightly different meanings.
*/
enum class Action
{
    none = 0,

    // Transport. deck selects the deck.
    deckPlayToggle,
    deckPlay,
    deckPause,
    deckCue,            ///< value above zero is a press, zero is a release
    deckSync,
    deckLoadSelected,

    deckKeyLockToggle,  ///< tempo changes stop shifting the pitch

    // Continuous deck controls, value normalised 0 to 1 unless noted.
    deckTempo,          ///< 0 is the slowest end of the fader, 1 the fastest
    deckTrim,
    deckSeek,

    // Jog wheel. slot is unused.
    jogTouch,           ///< value above zero means a hand is on the platter
    jogTurn,            ///< value is signed ticks, not normalised

    // Performance pads. slot selects the pad, 0 to 7.
    hotCue,             ///< press sets an empty slot, or jumps to a set one
    hotCueClear,

    // Mixer. deck selects the channel; slot selects the EQ band for deckEq.
    channelFader,
    channelEq,          ///< slot 0 low, 1 mid, 2 high
    channelCueToggle,
    channelCue,

    crossfader,         ///< 0 is hard A, 1 is hard B
    masterGain,
    cueGain,
    cueMix,

    // Track browser. deck and slot are unused.
    browseScroll,       ///< value is signed rows to move the selection, not normalised

    // Modifier, held rather than toggled.
    shift
};

/** One thing that happened, ready to be executed. */
struct ActionMessage
{
    Action action = Action::none;
    int deck = 0;
    int slot = 0;
    float value = 0.0f;
};

/** The name used for an action in a mapping file, and the reverse lookup.
    Returns Action::none for anything unrecognised, which is how a mapping with
    a typo in it degrades: that one control stops working, not the whole file. */
juce::String toString (Action action);
Action actionFromString (const juce::String& name);

/** True when the action carries a continuous value rather than a press. */
bool isContinuous (Action action);

} // namespace opendj
