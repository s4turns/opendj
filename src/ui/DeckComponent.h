/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/Deck.h"

#include <memory>

namespace opendj
{

/** A button that reports press and release separately, which is what cue and
    the performance pads need. TextButton only reports a completed click. */
class MomentaryButton final : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;

    std::function<void()> onPress;
    std::function<void()> onRelease;

    void mouseDown (const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseDown (e);

        if (onPress != nullptr)
            onPress();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseUp (e);

        if (onRelease != nullptr)
            onRelease();
    }
};

/** One deck: load, transport, tempo and a seek strip.

    The waveform view replaces the seek strip once analysis lands; until then the
    strip gives the same click-to-seek behaviour in a fraction of the code.
*/
class DeckComponent final : public juce::Component
{
public:
    DeckComponent (Deck& deckToControl, const juce::String& deckName);
    ~DeckComponent() override;

    /** Pulls transport state from the deck. Called from the shell's timer
        rather than each deck running its own. */
    void refresh();

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void loadButtonClicked();
    void applyTempoFromSlider();
    void seekFromMouse (const juce::MouseEvent& e);

    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;

    Deck& deck;
    juce::String name;

    juce::Label titleLabel;
    juce::Label timeLabel;
    juce::TextButton loadButton { "Load" };
    juce::TextButton playButton { "Play" };
    MomentaryButton cueButton { "Cue" };
    juce::Slider tempoSlider;
    juce::Label tempoLabel;
    juce::ComboBox tempoRangeBox;

    juce::Rectangle<int> seekStripBounds;
    std::unique_ptr<juce::FileChooser> fileChooser;

    double positionProportion = 0.0;
    double cueProportion = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeckComponent)
};

} // namespace opendj
