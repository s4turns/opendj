/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/AudioEngine.h"

#include <array>

namespace opendj
{

/** The two decks on screen, their beats on one time axis.

    A row per deck, a tick per beat and one playhead down the middle. Beats are
    placed in the time a listener is in rather than in track time, so two decks
    at the same tempo draw the same spacing whatever their records were cut at,
    and the rows line up exactly when the decks are in phase. Out of phase, they
    stagger, and how far they stagger is how far out they are.

    It is a readout and holds no state of its own: everything it draws is asked
    for afresh from the decks, on the shell's timer, like every other view.
*/
class BeatMatchComponent final : public juce::Component
{
public:
    explicit BeatMatchComponent (AudioEngine& engineToUse);

    /** Which deck each row shows, left then right. */
    void setDecks (int leftDeck, int rightDeck);

    /** Pulls positions and tempos from the decks. Driven by the shell's timer
        rather than a timer of its own. */
    void refresh();

    void paint (juce::Graphics& g) override;

private:
    struct Row
    {
        int deck = 0;
        juce::String name;
        bool loaded = false;
        double bpm = 0.0;              ///< as recorded, 0 when none was found
        double firstBeatSeconds = 0.0;
        double positionSeconds = 0.0;
        double tempoRatio = 1.0;
    };

    void paintRow (juce::Graphics& g, const Row& row, juce::Rectangle<int> area, double span) const;

    /** Half the width of the strip, in seconds as heard. Four beats of whatever
        is playing, so the strip always shows about eight of them however fast
        the decks are running. */
    double halfSpanSeconds() const;

    AudioEngine& engine;
    std::array<Row, 2> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatMatchComponent)
};

} // namespace opendj
