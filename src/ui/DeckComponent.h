/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/AudioEngine.h"
#include "ui/PlatterComponent.h"
#include "ui/WaveformComponent.h"

#include <array>
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

/** One deck: the two waveform views, transport, tempo and sync. Also a drop
    target for rows dragged out of the browser. */
class DeckComponent final : public juce::Component,
                            public juce::DragAndDropTarget
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

    bool isInterestedInDragSource (const SourceDetails& details) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails& details) override;

private:
    void loadButtonClicked();
    void refreshLoopControls();
    void applyTempoFromSlider();
    double rangePercent() const;
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
    juce::TextButton keyLockButton { "Key" };
    juce::TextButton slipButton { "Slip" };

    /** One knob per part of the track, and the button that asks for them. The
        knobs stay dead until a separation exists, since turning down a stem
        that is not there would do nothing and look broken. */
    struct StemControls
    {
        juce::TextButton separate { "Stems" };
        std::array<juce::Slider, numStems> knobs;
        std::array<juce::Label, numStems> labels;
    };

    StemControls stemControls;
    bool stemsWereAvailable = false;
    MomentaryButton cueButton { "Cue" };
    juce::Slider tempoSlider;
    juce::Label tempoLabel;
    juce::ComboBox tempoRangeBox;

    // Loop controls. The beat buttons set an automatic loop of that length, and
    // hold reads as a roll: press and hold repeats, letting go carries on from
    // where the track would have reached.
    static constexpr int numLoopButtons = 6;
    std::array<MomentaryButton, numLoopButtons> loopButtons;
    juce::TextButton loopToggleButton { "Loop" };
    juce::TextButton loopHalveButton { "/2" };
    juce::TextButton loopDoubleButton { "x2" };
    /** How long a loop button has to be held before it counts as a roll
        rather than a click. Below this it leaves a loop running. */
    static constexpr juce::uint32 holdIsARollMs = 350;

    std::array<juce::uint32, numLoopButtons> loopButtonPressedAt {};
    std::array<bool, numLoopButtons> loopButtonRolling { };

    WaveformComponent scrollingWave { WaveformComponent::Mode::scrolling };
    WaveformComponent overviewWave { WaveformComponent::Mode::overview };
    PlatterComponent platter;

    std::unique_ptr<juce::FileChooser> fileChooser;
    std::shared_ptr<const TrackAnalysis> shownAnalysis;
    bool wasLoading = false;
    bool dragHovering = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeckComponent)
};

} // namespace opendj
