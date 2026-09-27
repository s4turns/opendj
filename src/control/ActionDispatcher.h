/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include "control/Action.h"
#include "core/AudioEngine.h"

#include <functional>

namespace opendj
{

/** Executes actions against the engine.

    Every input, whether a mouse click, a key or a MIDI message, ends up here.
    That is on purpose: it means the interface and the controller cannot drift
    apart, and adding an input device is a matter of producing ActionMessages
    rather than reaching into the engine again.

    Actions arrive on the MIDI thread as well as the message thread. Everything
    it touches on the engine is safe to call from either.
*/
class ActionDispatcher
{
public:
    explicit ActionDispatcher (AudioEngine& engineToUse);

    void dispatch (const ActionMessage& message);

    bool isShiftHeld() const noexcept { return shiftHeld.load (std::memory_order_relaxed); }

    /** Called after any action that the interface should redraw for. May be
        called from the MIDI thread, so implementations must marshal. */
    std::function<void()> onStateChanged;

    /** Asked for a file when a load action arrives. Returns an invalid file to
        mean there is nothing selected. The browser provides this. */
    std::function<juce::File (int deck)> selectedFileProvider;

    /** Moves the browser's selection by a number of rows, negative for up.
        May be called from the MIDI thread, so implementations must marshal. */
    std::function<void (int rows)> browseScrollHandler;

    /** Zooms the scrolling waveforms by a number of rungs, positive to zoom in.
        One zoom is shared by every deck, so no deck is named. Only the shell
        knows the current level, so it provides this. May be called from the
        MIDI thread, so implementations must marshal. */
    std::function<void (int steps)> waveformZoomHandler;

    /** Puts a deck on screen. Given -1 it swaps whichever pair the last deck
        touched belongs to, which is what a single toggle button on a controller
        means. Only the shell knows which decks are on screen, so it provides
        this. May be called from the MIDI thread, so implementations must
        marshal. */
    std::function<void (int deckIndex)> deckSelectHandler;

    /** How far the tempo fader travels, as a percentage either side of zero. */
    void setTempoRange (int deckIndex, double percent);
    double getTempoRange (int deckIndex) const;

    /** Which effect a channel's shared FX depth knob currently reaches: 0 for
        echo, 1 for reverb. Echo until a select button says otherwise, which is
        also where a controller's own FX section powers up. Exposed so the
        mapping's select buttons can light up the one that is armed. */
    int getFxDepthTarget (int deckIndex) const;

    /** The pad mode a controller last reported for a deck, as the code it
        sent, or 0 before it has said anything. padLoop reads it. */
    int getPadMode (int deckIndex) const;

private:
    AudioEngine& engine;
    std::atomic<bool> shiftHeld { false };
    std::array<std::atomic<double>, AudioEngine::numDecks> tempoRangePercent;
    std::array<std::atomic<int>, AudioEngine::numDecks> fxDepthTarget {};
    std::array<std::atomic<int>, AudioEngine::numDecks> padMode {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ActionDispatcher)
};

} // namespace opendj
