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
                engine.syncDeck (deckIndex, AudioEngine::numDecks - 1 - deckIndex);
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

        case Action::browseScroll:
            if (browseScrollHandler != nullptr)
                browseScrollHandler (juce::roundToInt (message.value));
            notify = false;   // the browser redraws itself
            break;

        case Action::deckTempo:
        {
            // A fader at the top is faster, so the normalised value is inverted
            // against the usual physical layout of a pitch fader.
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

        case Action::hotCueClear:
            if (pressed)
                deck.clearHotCue (message.slot);
            break;

        case Action::channelFader:
            mixer.setChannelFader (deckIndex, message.value);
            break;

        case Action::channelEq:
            mixer.setChannelEq (deckIndex, message.slot, message.value);
            break;

        case Action::channelCue:
            mixer.setChannelCue (deckIndex, pressed);
            break;

        case Action::channelFilter:
            mixer.setChannelFilter (deckIndex, message.value);
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
