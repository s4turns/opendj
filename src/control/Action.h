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

    /** Brings a deck on screen in place of the one it shares a side with.
        `deck` names the deck to show; a press with no deck named swaps
        whichever side that control belongs to, which is what a controller's
        single deck-toggle button sends. */
    deckSelect,
    deckSwap,
    deckSlipToggle,     ///< the music keeps running under a scratch or a loop

    // Continuous deck controls, value normalised 0 to 1 unless noted.
    deckTempo,          ///< 0 is the slowest end of the fader, 1 the fastest
    deckTrim,
    deckSeek,

    // Jog wheel. slot is unused.
    jogTouch,           ///< value above zero means a hand is on the platter
    jogTurn,            ///< value is signed ticks, not normalised

    /** One part of a separated track. slot selects it: 0 drums, 1 bass,
        2 other, 3 vocals. The value is the gain, so a knob sweeps it and a
        button sends 1 or 0. */
    deckStem,
    deckStemToggle,     ///< press flips that stem between silent and full

    // Performance pads. slot selects the pad, 0 to 7.
    hotCue,             ///< press sets an empty slot, or jumps to a set one
    hotCueClear,

    // Loops. slot is an index into the beat lengths in loopBeatsForSlot().
    loopIn,
    loopOut,
    loopToggle,
    loopBeats,          ///< an automatic loop of that many beats
    loopRoll,           ///< the same, but only while the pad is held down
    loopHalve,
    loopDouble,
    loopReloop,

    // Mixer. deck selects the channel; slot selects the EQ band for deckEq.
    channelFader,
    channelEq,          ///< slot 0 low, 1 mid, 2 high
    channelCueToggle,
    channelCue,
    channelFilter,      ///< 0.5 is out of the way, down is a low pass, up a high pass

    /** Which side of the crossfader a channel answers to. slot picks it:
        0 the A side, 1 neither, 2 the B side. */
    channelCrossfaderAssign,

    crossfader,         ///< 0 is hard A, 1 is hard B
    masterGain,
    cueGain,
    cueMix,

    // Sampler. slot selects the pad, 0 to 7; deck is unused, because the
    // sampler sits over the whole mix rather than on one channel.
    samplerTrigger,     ///< press starts that slot from the beginning
    samplerStop,
    samplerGain,        ///< the level of the whole sampler

    // Track browser. deck and slot are unused.
    browseScroll,       ///< value is signed rows to move the selection, not normalised

    // Modifier, held rather than toggled.
    shift
};

/** The loop length a pad slot stands for, in beats.

    Eight lengths, shortest first, which is the order the second row of pads on
    a controller runs in and the order a length list should read in. */
double loopBeatsForSlot (int slot);

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
