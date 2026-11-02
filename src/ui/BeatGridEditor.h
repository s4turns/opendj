/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/Deck.h"

namespace opendj
{

/** The beat grid editor that drops down from a deck's BPM readout: the
    track's own tempo, steps up and down, half and double, sliding the grid
    earlier or later, the playhead as the downbeat, tap tempo, and revert.

    Every button acts on the deck at once, so the grid on the waveform moves
    while the editor is open and the result can be checked by ear and eye
    before it is closed. The deck keeps the edit with the track in the
    library; nothing here needs saving.
*/
class BeatGridEditor final : public juce::Component,
                             private juce::Timer
{
public:
    explicit BeatGridEditor (Deck& deckToEdit);
    ~BeatGridEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    /** Opens the editor in a callout pointing at `target`. */
    static void showFor (Deck& deck, juce::Component& target);

private:
    void timerCallback() override;
    void refresh();

    juce::TextButton& addButton (const juce::String& text, const juce::String& tooltip,
                                 std::function<void()> action);

    Deck& deck;

    juce::Label tempoLabel;
    juce::Label stateLabel;
    juce::OwnedArray<juce::TextButton> tempoButtons;   // -1 .. +1, /2, x2
    juce::OwnedArray<juce::TextButton> phaseButtons;   // earlier, later, downbeat
    juce::TextButton tapButton { "Tap" };
    juce::TextButton revertButton { "Revert" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatGridEditor)
};

} // namespace opendj
