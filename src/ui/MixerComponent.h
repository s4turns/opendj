/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/AudioEngine.h"

#include <array>
#include <memory>

namespace opendj
{

/** The centre section: one strip per deck, the crossfader and the master and
    cue controls. */
class MixerComponent final : public juce::Component
{
public:
    MixerComponent (AudioEngine& engineToUse, Mixer& mixerToControl);

    /** Redraws the level meters from the mixer's peak holds. */
    void refresh();

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    struct Strip
    {
        juce::Label heading;
        std::array<juce::Slider, 3> eq;      // high, mid, low, top to bottom
        juce::Slider filter;                 // centred: low pass down, high pass up
        juce::Slider echo;                   // off at the bottom
        juce::ComboBox echoBeats;            // how long one repeat lasts
        juce::Slider reverb;                 // off at the bottom
        juce::ComboBox assign;               // which side of the crossfader
        juce::Slider fader;
        juce::TextButton cue { "Cue" };
        std::array<juce::Label, 8> captions; // a name beside each row, top to bottom
    };

    void configureKnob (juce::Slider& knob);
    void layOutStrip (Strip& strip, juce::Rectangle<int> area);

    AudioEngine& engine;
    Mixer& mixer;

    std::array<Strip, Mixer::numChannels> strips;
    juce::Slider crossfader;
    juce::ComboBox curveBox;
    juce::Slider masterKnob;
    juce::Slider cueKnob;
    juce::Slider cueMixKnob;
    juce::Label masterLabel, cueLabel, cueMixLabel, crossfaderLabel;

    juce::Rectangle<int> meterBounds;
    std::array<float, 2> meterLevels { 0.0f, 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerComponent)
};

} // namespace opendj
