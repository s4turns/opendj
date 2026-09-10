/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/MixerComponent.h"

namespace opendj
{

namespace
{
    const juce::Colour panelColour  { 0xff1c1c22 };
    const juce::Colour accentColour { 0xff35c2f0 };

    // EQ knobs are laid out high to low, matching every DJ mixer ever built,
    // while the engine numbers its bands low to high.
    constexpr int bandForRow (int row) noexcept { return 2 - row; }
}

MixerComponent::MixerComponent (Mixer& mixerToControl)
    : mixer (mixerToControl)
{
    const char* headings[] = { "A", "B" };

    for (size_t c = 0; c < strips.size(); ++c)
    {
        auto& strip = strips[c];
        const auto channel = static_cast<int> (c);

        strip.heading.setText (headings[c], juce::dontSendNotification);
        strip.heading.setJustificationType (juce::Justification::centred);
        strip.heading.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (strip.heading);

        for (int row = 0; row < 3; ++row)
        {
            auto& knob = strip.eq[(size_t) row];
            configureKnob (knob);
            knob.setValue (0.5, juce::dontSendNotification);
            knob.onValueChange = [this, channel, row, &knob]
            {
                mixer.setChannelEq (channel, bandForRow (row), static_cast<float> (knob.getValue()));
            };
            addAndMakeVisible (knob);
        }

        strip.fader.setSliderStyle (juce::Slider::LinearVertical);
        strip.fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        strip.fader.setRange (0.0, 1.0, 0.0);
        strip.fader.setValue (0.8, juce::dontSendNotification);
        strip.fader.onValueChange = [this, channel, &strip]
        {
            mixer.setChannelFader (channel, static_cast<float> (strip.fader.getValue()));
        };
        addAndMakeVisible (strip.fader);

        strip.cue.setClickingTogglesState (true);
        strip.cue.setColour (juce::TextButton::buttonOnColourId, accentColour.darker (0.3f));
        strip.cue.onClick = [this, channel, &strip]
        {
            mixer.setChannelCue (channel, strip.cue.getToggleState());
        };
        addAndMakeVisible (strip.cue);
    }

    crossfader.setSliderStyle (juce::Slider::LinearHorizontal);
    crossfader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    crossfader.setRange (-1.0, 1.0, 0.0);
    crossfader.setValue (0.0, juce::dontSendNotification);
    crossfader.setDoubleClickReturnValue (true, 0.0);
    crossfader.onValueChange = [this]
    {
        mixer.setCrossfaderPosition (static_cast<float> (crossfader.getValue()));
    };
    addAndMakeVisible (crossfader);

    curveBox.addItemList ({ "Smooth", "Linear", "Cut" }, 1);
    curveBox.setSelectedId (1, juce::dontSendNotification);
    curveBox.onChange = [this]
    {
        switch (curveBox.getSelectedId())
        {
            case 2:  mixer.setCrossfaderCurve (Mixer::CrossfaderCurve::linear); break;
            case 3:  mixer.setCrossfaderCurve (Mixer::CrossfaderCurve::sharpCut); break;
            default: mixer.setCrossfaderCurve (Mixer::CrossfaderCurve::constantPower); break;
        }
    };
    addAndMakeVisible (curveBox);

    const auto setUpMasterKnob = [this] (juce::Slider& knob, juce::Label& label,
                                         const juce::String& text, double initial,
                                         std::function<void (float)> apply)
    {
        configureKnob (knob);
        knob.setValue (initial, juce::dontSendNotification);
        knob.onValueChange = [&knob, apply = std::move (apply)]
        {
            apply (static_cast<float> (knob.getValue()));
        };
        addAndMakeVisible (knob);

        label.setText (text, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, juce::Colours::grey);
        label.setFont (juce::FontOptions (11.0f));
        addAndMakeVisible (label);
    };

    setUpMasterKnob (masterKnob, masterLabel, "Master", 0.8,
                     [this] (float v) { mixer.setMasterGain (v); });
    setUpMasterKnob (cueKnob, cueLabel, "Phones", 0.7,
                     [this] (float v) { mixer.setCueGain (v); });
    setUpMasterKnob (cueMixKnob, cueMixLabel, "Cue/Mix", 0.0,
                     [this] (float v) { mixer.setCueMix (v); });

    crossfaderLabel.setText ("Crossfader", juce::dontSendNotification);
    crossfaderLabel.setJustificationType (juce::Justification::centred);
    crossfaderLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    crossfaderLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (crossfaderLabel);
}

void MixerComponent::configureKnob (juce::Slider& knob)
{
    knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    knob.setRange (0.0, 1.0, 0.0);
    knob.setDoubleClickReturnValue (true, 0.5);
    knob.setColour (juce::Slider::rotarySliderFillColourId, accentColour);
}

void MixerComponent::refresh()
{
    for (int ch = 0; ch < 2; ++ch)
    {
        // Fall faster than we rise, so peaks stay readable without holding.
        const auto level = mixer.getMasterPeak (ch);
        meterLevels[(size_t) ch] = juce::jmax (level, meterLevels[(size_t) ch] * 0.82f);
    }

    repaint (meterBounds);
}

void MixerComponent::paint (juce::Graphics& g)
{
    g.setColour (panelColour);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);

    g.setColour (juce::Colour (0xff101014));
    g.fillRoundedRectangle (meterBounds.toFloat(), 3.0f);

    const auto barWidth = meterBounds.getWidth() / 2.0f - 3.0f;

    for (int ch = 0; ch < 2; ++ch)
    {
        // Meter in decibels, from -48 up to 0, so quiet material still moves.
        const auto db = juce::Decibels::gainToDecibels (meterLevels[(size_t) ch], -48.0f);
        const auto proportion = juce::jlimit (0.0f, 1.0f, (db + 48.0f) / 48.0f);

        const auto x = meterBounds.getX() + 2.0f + ch * (barWidth + 2.0f);
        const auto height = meterBounds.getHeight() * proportion;

        g.setColour (db > -0.5f ? juce::Colours::red
                                : (db > -6.0f ? juce::Colours::orange : accentColour));
        g.fillRect (x, meterBounds.getBottom() - height, barWidth, height);
    }
}

void MixerComponent::layOutStrip (Strip& strip, juce::Rectangle<int> area)
{
    strip.heading.setBounds (area.removeFromTop (18));
    area.removeFromTop (4);

    for (auto& knob : strip.eq)
    {
        knob.setBounds (area.removeFromTop (44).reduced (4, 2));
    }

    strip.cue.setBounds (area.removeFromBottom (24).reduced (4, 2));
    strip.fader.setBounds (area.reduced (10, 6));
}

void MixerComponent::resized()
{
    auto area = getLocalBounds().reduced (8);

    auto footer = area.removeFromBottom (46);
    crossfaderLabel.setBounds (footer.removeFromTop (14));
    curveBox.setBounds (footer.removeFromRight (76).reduced (2, 2));
    crossfader.setBounds (footer.reduced (4, 0));

    area.removeFromBottom (6);

    auto masterRow = area.removeFromBottom (66);
    const auto knobWidth = masterRow.getWidth() / 3;

    const auto placeKnob = [&masterRow, knobWidth] (juce::Slider& knob, juce::Label& label)
    {
        auto cell = masterRow.removeFromLeft (knobWidth);
        label.setBounds (cell.removeFromBottom (14));
        knob.setBounds (cell.reduced (4, 1));
    };

    placeKnob (masterKnob, masterLabel);
    placeKnob (cueKnob, cueLabel);
    placeKnob (cueMixKnob, cueMixLabel);

    area.removeFromBottom (6);

    meterBounds = area.removeFromRight (22).reduced (2, 18);
    area.removeFromRight (4);

    const auto stripWidth = area.getWidth() / static_cast<int> (strips.size());

    for (auto& strip : strips)
        layOutStrip (strip, area.removeFromLeft (stripWidth));
}

} // namespace opendj
