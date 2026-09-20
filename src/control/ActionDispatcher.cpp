/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "control/ActionDispatcher.h"

namespace opendj
{

ActionDispatcher::ActionDispatcher (AudioEngine& engineToUse)
    : engine (engineToUse)
{
    for (auto& range : tempoRangePercent)
        range.store (8.0, std::memory_order_relaxed);
}

void ActionDispatcher::setTempoRange (int deckIndex, double percent)
{
    if (juce::isPositiveAndBelow (deckIndex, AudioEngine::numDecks))
        tempoRangePercent[(size_t) deckIndex].store (juce::jlimit (1.0, 100.0, percent),
                                                     std::memory_order_relaxed);
}

double ActionDispatcher::getTempoRange (int deckIndex) const
{
    return juce::isPositiveAndBelow (deckIndex, AudioEngine::numDecks)
        ? tempoRangePercent[(size_t) deckIndex].load (std::memory_order_relaxed)
        : 8.0;
}

int ActionDispatcher::getFxDepthTarget (int deckIndex) const
{
    return juce::isPositiveAndBelow (deckIndex, AudioEngine::numDecks)
        ? fxDepthTarget[(size_t) deckIndex].load (std::memory_order_relaxed)
        : 0;
}

int ActionDispatcher::getPadMode (int deckIndex) const
{
    return juce::isPositiveAndBelow (deckIndex, AudioEngine::numDecks)
        ? padMode[(size_t) deckIndex].load (std::memory_order_relaxed)
        : 0;
}

namespace
{
    /** The Roland DJ-202's codes for its two roll modes, with and without
        shift. It is the only controller that reports its pad mode, so its
        codes are the ones there are; anything else, loop mode included, makes
        pads 1 to 4 set loops. Taken from Mixxx's DJ-202 script. */
    bool isRollMode (int code) noexcept
    {
        return code == 0x11 || code == 0x13;
    }
}

void ActionDispatcher::dispatch (const ActionMessage& message)
{
    const auto deckIndex = juce::jlimit (0, AudioEngine::numDecks - 1, message.deck);
    auto& deck = engine.getDeck (deckIndex);
    auto& mixer = engine.getMixer();

    const auto pressed = message.value > 0.5f;
    auto notify = true;

    switch (message.action)
    {
        case Action::none:
            return;

        case Action::shift:
            shiftHeld.store (pressed, std::memory_order_relaxed);
            notify = false;
            break;

        case Action::deckPlayToggle:
            // Buttons send a press and a release; only the press should act.
            if (pressed)
                deck.togglePlay();
            break;

        case Action::deckPlay:
            if (pressed)
                deck.play();
            break;

        case Action::deckPause:
            if (pressed)
                deck.pause();
            break;

        case Action::deckCue:
            if (pressed)
                deck.cuePressed();
            else
                deck.cueReleased();
            break;

        case Action::deckSync:
            if (pressed)
                if (const auto leader = engine.findSyncLeader (deckIndex); leader >= 0)
                    engine.syncDeck (deckIndex, leader);
            break;

        case Action::deckLoadSelected:
            if (pressed && selectedFileProvider != nullptr)
                if (const auto file = selectedFileProvider (deckIndex); file.existsAsFile())
                    engine.loadTrackAsync (deckIndex, file);
            break;

        case Action::deckKeyLockToggle:
            if (pressed)
                deck.toggleKeyLock();
            break;

        case Action::deckSelect:
            if (pressed && deckSelectHandler != nullptr)
                deckSelectHandler (deckIndex);
            notify = false;   // the shell redraws itself
            break;

        case Action::deckSwap:
            if (pressed && deckSelectHandler != nullptr)
                deckSelectHandler (-1);
            notify = false;
            break;

        case Action::channelCrossfaderAssign:
            if (pressed)
                mixer.setChannelCrossfaderAssign (deckIndex,
                    message.slot == 0 ? Mixer::CrossfaderAssign::a
                  : message.slot == 2 ? Mixer::CrossfaderAssign::b
                                      : Mixer::CrossfaderAssign::thru);
            break;

        case Action::deckWaveformZoom:
            if (waveformZoomHandler != nullptr)
                waveformZoomHandler (juce::roundToInt (message.value));

            notify = false;   // the waveforms redraw themselves
            break;

        case Action::browseScroll:
            if (browseScrollHandler != nullptr)
                browseScrollHandler (juce::roundToInt (message.value));
            notify = false;   // the browser redraws itself
            break;

        case Action::deckTempo:
        {
            // 0 is the slow end and 1 the fast end, whichever way the hardware
            // reports them: a mapping's `inverted` flag is where that is
            // settled, not here.
            const auto range = getTempoRange (deckIndex);
            const auto percent = (message.value * 2.0 - 1.0) * range;
            deck.setTempoRatio (1.0 + percent / 100.0);
            break;
        }

        case Action::deckTrim:
            deck.setTrim (message.value * 2.0f);
            break;

        case Action::deckSeek:
            deck.seekToFraction (message.value);
            break;

        case Action::jogTouch:
            deck.setJogTouched (pressed);
            break;

        case Action::jogTurn:
            deck.addJogTicks (message.value);
            notify = false;   // far too frequent to redraw on
            break;

        case Action::hotCue:
            if (pressed)
            {
                if (isShiftHeld())
                    deck.clearHotCue (message.slot);
                else
                    deck.hotCuePressed (message.slot);
            }
            break;

        case Action::deckSlipToggle:
            if (pressed)
                deck.toggleSlip();
            break;

        case Action::loopIn:
            if (pressed)
                deck.setLoopIn();
            break;

        case Action::loopOut:
            if (pressed)
                deck.setLoopOut();
            break;

        case Action::loopToggle:
            if (pressed)
                deck.toggleLoop();
            break;

        case Action::loopBeats:
            // Sticky: the button that turned a loop on is the button that turns
            // it off again. Pressing a different length while one is running
            // changes to that length rather than stopping, which is what a
            // hand reaching for 4 in the middle of an 8 means.
            if (pressed)
                deck.toggleLoopBeats (loopBeatsForSlot (message.slot));
            break;

        case Action::loopRoll:
            // Momentary: the pad being let go is as much a part of a roll as
            // the pad going down, which is why this one reads the release.
            if (pressed)
                deck.beginLoopRoll (loopBeatsForSlot (message.slot));
            else
                deck.endLoopRoll();
            break;

        case Action::loopHalve:
            if (pressed)
                deck.halveLoop();
            break;

        case Action::loopDouble:
            if (pressed)
                deck.doubleLoop();
            break;

        case Action::loopReloop:
            if (pressed)
                deck.reloop();
            break;

        case Action::hotCueClear:
            if (pressed)
                deck.clearHotCue (message.slot);
            break;

        case Action::padMode:
            padMode[(size_t) deckIndex].store (juce::roundToInt (message.value * 127.0f),
                                               std::memory_order_relaxed);
            notify = false;
            break;

        case Action::padLoop:
        {
            const auto pad = juce::jlimit (0, 3, message.slot);

            if (pressed)
            {
                // The same four pads, longest first in one mode and shortest
                // first in the other, which is how the DJ-202 prints them.
                if (isRollMode (getPadMode (deckIndex)))
                    deck.beginLoopRoll (1.0 / (double) (1 << pad));
                else
                    deck.toggleLoopBeats ((double) (1 << pad));
            }
            else if (deck.isLoopRolling())
            {
                // Whatever mode the pads are in now: a roll held while the mode
                // changed still ends when the pad lets go.
                deck.endLoopRoll();
            }
            break;
        }

        case Action::channelFader:
            mixer.setChannelFader (deckIndex, message.value);
            break;

        case Action::channelEq:
            mixer.setChannelEq (deckIndex, message.slot, message.value);
            break;

        case Action::channelCue:
            mixer.setChannelCue (deckIndex, pressed);
            break;

        case Action::deckStem:
            deck.setStemGain (static_cast<Stem> (juce::jlimit (0, numStems - 1, message.slot)),
                              message.value);
            break;

        case Action::deckStemToggle:
            if (pressed)
            {
                const auto stem = static_cast<Stem> (juce::jlimit (0, numStems - 1, message.slot));
                deck.setStemGain (stem, deck.getStemGain (stem) > 0.5f ? 0.0f : 1.0f);
            }
            break;

        case Action::samplerTrigger:
            // Shift turns a pad into its own stop button, which is the only way
            // to end a looping slot on hardware with one row of pads.
            if (pressed)
            {
                if (shiftHeld.load (std::memory_order_relaxed))
                    engine.getSampler().stop (message.slot);
                else
                    engine.getSampler().trigger (message.slot);
            }
            break;

        case Action::samplerStop:
            if (pressed)
                engine.getSampler().stop (message.slot);
            break;

        case Action::samplerGain:
            engine.getSampler().setGain (message.value);
            break;

        case Action::micToggle:
            if (pressed)
                engine.getMic().setEnabled (! engine.getMic().isEnabled());
            break;

        case Action::micGain:
            engine.getMic().setGain (message.value * 2.0f);
            break;

        case Action::micTalkover:
            if (pressed)
                engine.getMic().setTalkover (! engine.getMic().isTalkoverEnabled());
            break;

        case Action::channelFilter:
            mixer.setChannelFilter (deckIndex, message.value);
            break;

        case Action::channelEcho:
            mixer.setChannelEcho (deckIndex, message.value);
            break;

        case Action::channelReverb:
            mixer.setChannelReverb (deckIndex, message.value);
            break;

        case Action::channelFxSelect:
            // A select button decides where the depth knob goes next; it never
            // touches the mixer itself, so there is nothing here for the shell
            // to redraw beyond the light on the button, which refreshFeedback
            // already polls for.
            if (pressed)
                fxDepthTarget[(size_t) deckIndex].store (message.slot == 1 ? 1 : 0,
                                                         std::memory_order_relaxed);
            notify = false;
            break;

        case Action::channelFxDepth:
            if (getFxDepthTarget (deckIndex) == 1)
                mixer.setChannelReverb (deckIndex, message.value);
            else
                mixer.setChannelEcho (deckIndex, message.value);
            break;

        case Action::channelCueToggle:
            if (pressed)
                mixer.toggleChannelCue (deckIndex);
            break;

        case Action::crossfader:
            mixer.setCrossfaderPosition (message.value * 2.0f - 1.0f);
            break;

        case Action::masterGain:
            mixer.setMasterGain (message.value);
            break;

        case Action::cueGain:
            mixer.setCueGain (message.value);
            break;

        case Action::cueMix:
            mixer.setCueMix (message.value);
            break;
    }

    if (notify && onStateChanged != nullptr)
        onStateChanged();
}

} // namespace opendj
