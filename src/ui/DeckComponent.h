/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/AudioEngine.h"
#include "ui/PlatterComponent.h"
#include "ui/WaveformComponent.h"

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

/** One deck: the two waveform views, transport, tempo and sync. */
class DeckComponent final : public juce::Component
{
public:
    DeckComponent (AudioEngine& engineToUse, int deckIndex, const juce::String& deckName);
    ~DeckComponent() override;

    /** Pulls transport state from the deck. Driven by the shell's timer rather
        than each deck running one of its own. */
    void refresh();

    /** Starts a background load and analysis of the given file. */
    void load (const juce::File& file);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void loadButtonClicked();
    void applyTempoFromSlider();
    void updateTempoReadout();

    AudioEngine& engine;
    const int index;
    juce::String name;

    Deck& deck;

    juce::Label titleLabel;
    juce::Label timeLabel;
    juce::Label bpmLabel;
    juce::TextButton loadButton { "Load" };
    juce::TextButton playButton { "Play" };
    juce::TextButton syncButton { "Sync" };
    MomentaryButton cueButton { "Cue" };
    juce::Slider tempoSlider;
    juce::Label tempoLabel;
    juce::ComboBox tempoRangeBox;

    WaveformComponent scrollingWave { WaveformComponent::Mode::scrolling };
    WaveformComponent overviewWave { WaveformComponent::Mode::overview };
    PlatterComponent platter;

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::shared_ptr<const TrackAnalysis> shownAnalysis;
    bool wasLoading = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeckComponent)
};

} // namespace opendj
