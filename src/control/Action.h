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

    deckKeyUp,          ///< press moves the pitch up a semitone
    deckKeyDown,        ///< press moves the pitch down a semitone
    deckKeyReset,       ///< press goes back to the track's own key
    deckKeyMatch,       ///< press shifts to the key of the deck a sync would follow

    deckBrake,          ///< press slows the deck to a stop
    deckBackspin,       ///< press flings the record backwards to a stop
    deckReverseToggle,  ///< plays backwards until pressed again
    deckCensor,         ///< backwards while held, landing back on time

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

    /** Zooms the scrolling waveform. The value is signed rungs of a fixed
        ladder, not normalised, the same shape as browseScroll; positive zooms
        in. One zoom is shared by every deck, so `deck` is unused. */
    deckWaveformZoom,

    // Jog wheel. slot is unused.
    jogTouch,           ///< value above zero means a hand is on the platter
    jogTurn,            ///< value is signed ticks, not normalised

    /** One part of a separated track. slot selects it: 0 drums, 1 bass,
        2 other, 3 vocals. The value is the gain, so a knob sweeps it and a
        button sends 1 or 0. */
    deckStem,
    deckStemToggle,     ///< press flips that stem between silent and full

    /** The stem pads. slot selects one: 0 no vocals, 1 acapella, 2 drums only,
        3 no drums. They do nothing until the track has stems. */
    deckStemPad,        ///< on for as long as it is held, then back to any latched pad
    deckStemPadToggle,  ///< press latches it, and pressing it again turns it off

    /** One effect on one stem of the deck. They do nothing until the track has
        stems, except that the choices are remembered. */
    deckStemFx,         ///< press turns it on and off; off lets the tail ring out
    deckStemFxTarget,   ///< slot picks the stem: 0 drums, 1 bass, 2 other, 3 vocals
    deckStemFxType,     ///< slot picks the effect: 0 echo, 1 reverb, 2 low pass
    deckStemFxBeats,    ///< slot picks the echo length: 0 is 1/4, then 1/2, 1 and 2 beats
    deckStemFxAmount,   ///< how much of it, or for the low pass how far it closes

    // Performance pads. slot selects the pad, 0 to 7.
    hotCue,             ///< press sets an empty slot, or jumps to a set one
    hotCueClear,

    /** Which mode a controller's pads are in, as the controller reports it:
        the value is the code it sent, over 127. Stored per deck and read by
        padLoop, the way channelFxSelect is read by channelFxDepth; nothing in
        the engine changes. */
    padMode,

    /** Pads 1 to 4 of a loop section whose meaning depends on padMode: a loop
        of 1, 2, 4 or 8 beats in loop mode, or a roll of 1, 1/2, 1/4 or 1/8 of
        a beat for as long as the pad is held in roll mode. slot picks the pad,
        0 to 3. */
    padLoop,

    // Loops. slot is an index into the beat lengths in loopBeatsForSlot().
    loopIn,
    loopOut,
    loopToggle,
    loopBeats,          ///< an automatic loop of that many beats
    loopRoll,           ///< the same, but only while the pad is held down
    loopHalve,
    loopDouble,
    loopReloop,

    // Beat jump. slot is an index into loopBeatsForSlot(), so a controller
    // can have one button per size, as it does for loop lengths.
    beatJumpForward,
    beatJumpBack,
    deckQuantizeToggle, ///< cues and loops snap to the beat grid

    // Beat grid editing. deck selects the deck.
    deckTapTempo,       ///< each press is a tap; four or more set the tempo
    gridDownbeatHere,   ///< the playhead becomes the first beat of a bar
    gridShiftEarlier,   ///< slides the grid 5 ms earlier
    gridShiftLater,     ///< slides the grid 5 ms later

    // Mixer. deck selects the channel; slot selects the EQ band for deckEq.
    channelFader,
    channelEq,          ///< slot 0 low, 1 mid, 2 high
    channelCueToggle,
    channelCue,
    channelFilter,      ///< 0.5 is out of the way, down is a low pass, up a high pass
    channelEcho,        ///< 0 is off, a beat-synced echo above that
    channelReverb,      ///< 0 is off, a wash of tail above that
    channelEchoOutToggle, ///< press takes the track away and leaves its echo
    autoGainToggle,     ///< press turns levelling by loudness on or off, for every deck

    /** Arms echo or reverb as the effect a shared depth knob controls next.
        slot picks it: 0 echo, 1 reverb. For a controller with one FX knob
        behind several effect-select buttons, such as the DJ-202's. Nothing
        in the engine changes; this only decides where channelFxDepth goes. */
    channelFxSelect,

    /** The shared depth knob itself: writes to whichever effect
        channelFxSelect last armed for this channel, echo until told
        otherwise. */
    channelFxDepth,

    /** Which side of the crossfader a channel answers to. slot picks it:
        0 the A side, 1 neither, 2 the B side. */
    channelCrossfaderAssign,

    crossfader,         ///< 0 is hard A, 1 is hard B
    masterGain,
    cueGain,
    cueMix,

    // Master effects. deck selects the unit, 0 or 1, because they sit over
    // the whole mix rather than on one channel.
    masterFxToggle,     ///< press turns the unit on or off
    masterFxOn,         ///< on while the value is above half, for a held button
    masterFxWet,        ///< how much of the effect is heard
    masterFxParam,      ///< slot picks the parameter, 0 or 1, of the current effect
    masterFxType,       ///< press picks an effect: slot 0 echo, 1 reverb, 2 filter, 3 plugin, 4 flanger, 5 phaser, 6 gate, 7 bitcrusher, 8 stutter
    masterFxNextType,   ///< press steps on to the next effect

    // Sampler. slot selects the pad, 0 to 7; deck is unused, because the
    // sampler sits over the whole mix rather than on one channel.
    samplerTrigger,     ///< press starts that pad of the bank on show from the beginning
    samplerStop,
    samplerBank,        ///< press puts bank `slot` (0 to 3) on the pads
    samplerNextBank,    ///< press steps on to the next bank
    samplerGain,        ///< the level of the whole sampler

    // Mic. deck and slot are unused: like the sampler, it sits over the whole mix.
    micToggle,          ///< press turns the mic on or off
    micGain,            ///< the mic's level; 0.5 is unity, 1 about +6 dB
    micTalkover,        ///< press turns talkover on or off

    // Aux line, such as browser audio. deck and slot are unused.
    auxToggle,          ///< press turns the aux on or off
    auxGain,            ///< the aux's level; 0.5 is unity, 1 about +6 dB

    // Track browser. deck and slot are unused.
    browseScroll,       ///< value is signed rows to move the selection, not normalised

    // Stems ahead of time. deck and slot are unused.
    stemsPrepareSelected,   ///< press queues the list on show: the library's selection, or the whole playlist on show
    stemsPrepareCancel,     ///< press forgets the queue; a deck's own request carries on

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

/** Whether a deck or a slot means anything to an action, so that an interface
    need only ask for what it uses. Master effects take a unit as their deck. */
bool actionTakesDeck (Action action);
bool actionTakesSlot (Action action);

/** The last action there is, for walking all of them. */
inline constexpr Action lastAction = Action::shift;

} // namespace opendj
