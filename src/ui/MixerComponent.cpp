/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/MixerComponent.h"

#include <cmath>

namespace opendj
{

namespace
{
    const juce::Colour panelColour  { 0xff1c1c22 };
    const juce::Colour accentColour { 0xff35c2f0 };

    // EQ knobs are laid out high to low, matching every DJ mixer ever built,
    // while the engine numbers its bands low to high.
    constexpr int bandForRow (int row) noexcept { return 2 - row; }

    // The strips are short on height, so a knob row is no taller than it must
    // be: every pixel saved here is one more pixel of channel fader.
    constexpr int knobRowHeight  = 36;
    constexpr int comboRowHeight = 20;

    const char* const rowNames[] { "Hi", "Mid", "Low", "Filter", "Echo", "Beats", "Reverb", "X-fade" };

    void styleCaption (juce::Label& label, const juce::String& text,
                       juce::Justification justification = juce::Justification::centred)
    {
        label.setText (text, juce::dontSendNotification);
        label.setJustificationType (justification);
        label.setColour (juce::Label::textColourId, juce::Colours::grey);
        label.setFont (juce::FontOptions (11.0f));
    }
}

MixerComponent::MixerComponent (AudioEngine& engineToUse, Mixer& mixerToControl)
    : engine (engineToUse), mixer (mixerToControl)
{
    const char* headings[] = { "A", "B", "C", "D" };

    for (size_t c = 0; c < strips.size(); ++c)
    {
        auto& strip = strips[c];
        const auto channel = static_cast<int> (c);

        strip.heading.setText (headings[c], juce::dontSendNotification);
        strip.heading.setJustificationType (juce::Justification::centred);
        strip.heading.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (strip.heading);

        for (size_t row = 0; row < strip.captions.size(); ++row)
        {
            styleCaption (strip.captions[row], rowNames[row], juce::Justification::centredRight);
            strip.captions[row].setMinimumHorizontalScale (0.6f);
            strip.captions[row].setInterceptsMouseClicks (false, false);
            addAndMakeVisible (strip.captions[row]);
        }

        for (int row = 0; row < 3; ++row)
        {
            static const char* bandNames[] = { "High", "Mid", "Low" };

            auto& knob = strip.eq[(size_t) row];
            configureKnob (knob);
            knob.setValue (0.5, juce::dontSendNotification);
            knob.setTooltip (juce::String (bandNames[row]) + " EQ: centre is flat, double-click to reset");
            knob.onValueChange = [this, channel, row, &knob]
            {
                mixer.setChannelEq (channel, bandForRow (row), static_cast<float> (knob.getValue()));
            };
            addAndMakeVisible (knob);
        }

        configureKnob (strip.filter);
        strip.filter.setValue (0.5, juce::dontSendNotification);
        strip.filter.setDoubleClickReturnValue (true, 0.5);
        strip.filter.setTooltip ("Filter: down is a low pass, up is a high pass, centre is off");
        strip.filter.onValueChange = [this, channel, &strip]
        {
            mixer.setChannelFilter (channel, static_cast<float> (strip.filter.getValue()));
        };
        addAndMakeVisible (strip.filter);

        configureKnob (strip.echo);
        strip.echo.setValue (0.0, juce::dontSendNotification);
        strip.echo.setDoubleClickReturnValue (true, 0.0);
        strip.echo.setTooltip ("Echo: beat-synced repeats, off at the bottom");
        strip.echo.onValueChange = [this, channel, &strip]
        {
            mixer.setChannelEcho (channel, static_cast<float> (strip.echo.getValue()));
        };
        addAndMakeVisible (strip.echo);

        // The lengths a DJ echo is actually set to. A bar at the top, an
        // eighth at the bottom, and the whole thing sweeps when it changes.
        strip.echoBeats.addItemList ({ "1/8", "1/4", "1/2", "1", "2", "4" }, 1);
        strip.echoBeats.setSelectedId (4, juce::dontSendNotification);   // one beat
        strip.echoBeats.setTooltip ("How long one echo repeat lasts, in beats");
        strip.echoBeats.onChange = [this, channel, &strip]
        {
            static constexpr double lengths[] = { 0.125, 0.25, 0.5, 1.0, 2.0, 4.0 };
            const auto index = juce::jlimit (0, 5, strip.echoBeats.getSelectedId() - 1);
            engine.setEchoBeats (channel, lengths[index]);
        };
        addAndMakeVisible (strip.echoBeats);

        configureKnob (strip.reverb);
        strip.reverb.setValue (0.0, juce::dontSendNotification);
        strip.reverb.setDoubleClickReturnValue (true, 0.0);
        strip.reverb.setTooltip ("Reverb: off at the bottom");
        strip.reverb.onValueChange = [this, channel, &strip]
        {
            mixer.setChannelReverb (channel, static_cast<float> (strip.reverb.getValue()));
        };
        addAndMakeVisible (strip.reverb);

        // Three positions rather than a switch with two, because the useful
        // answer for a third deck is neither side of the crossfader.
        strip.assign.addItemList ({ "X:A", "X:-", "X:B" }, 1);
        strip.assign.setSelectedId (channel == 0 ? 1 : channel == 1 ? 3 : 2,
                                    juce::dontSendNotification);
        strip.assign.setTooltip ("Which side of the crossfader this channel answers to");
        strip.assign.onChange = [this, channel, &strip]
        {
            mixer.setChannelCrossfaderAssign (channel,
                strip.assign.getSelectedId() == 1 ? Mixer::CrossfaderAssign::a
              : strip.assign.getSelectedId() == 3 ? Mixer::CrossfaderAssign::b
                                                  : Mixer::CrossfaderAssign::thru);
        };
        addAndMakeVisible (strip.assign);

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

        styleCaption (label, text);
        addAndMakeVisible (label);
    };

    setUpMasterKnob (masterKnob, masterLabel, "Master", 0.8,
                     [this] (float v) { mixer.setMasterGain (v); });
    setUpMasterKnob (cueKnob, cueLabel, "Phones", 0.7,
                     [this] (float v) { mixer.setCueGain (v); });
    setUpMasterKnob (cueMixKnob, cueMixLabel, "Cue/Mix", 0.0,
                     [this] (float v) { mixer.setCueMix (v); });

    styleCaption (crossfaderLabel, "Crossfader");
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

    // Follow the engine, so a hardware knob moves the one on screen. Without
    // this a controller appears to do nothing at all, whatever it is really
    // doing to the audio. Notifications are suppressed, or setting a slider
    // here would be dispatched straight back to the mixer.
    const auto follow = [] (juce::Slider& slider, double value)
    {
        if (! slider.isMouseButtonDown() && std::abs (slider.getValue() - value) > 1.0e-4)
            slider.setValue (value, juce::dontSendNotification);
    };

    for (int channel = 0; channel < Mixer::numChannels; ++channel)
    {
        auto& strip = strips[(size_t) channel];

        follow (strip.fader, mixer.getChannelFader (channel));

        // The knobs read high, mid, low down the strip; the mixer numbers the
        // bands the other way up.
        for (int row = 0; row < 3; ++row)
            follow (strip.eq[(size_t) row], mixer.getChannelEq (channel, 2 - row));

        follow (strip.filter, mixer.getChannelFilter (channel));
        follow (strip.echo, mixer.getChannelEcho (channel));
        follow (strip.reverb, mixer.getChannelReverb (channel));

        strip.cue.setToggleState (mixer.isChannelCued (channel), juce::dontSendNotification);

        const auto assign = mixer.getChannelCrossfaderAssign (channel);
        const auto assignId = assign == Mixer::CrossfaderAssign::a ? 1
                            : assign == Mixer::CrossfaderAssign::b ? 3 : 2;

        if (strip.assign.getSelectedId() != assignId)
            strip.assign.setSelectedId (assignId, juce::dontSendNotification);
    }

    follow (crossfader, mixer.getCrossfaderPosition());
    follow (masterKnob, mixer.getMasterGain());
    follow (cueKnob, mixer.getCueGain());
    follow (cueMixKnob, mixer.getCueMix());

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
    strip.heading.setBounds (area.removeFromTop (16));
    area.removeFromTop (4);

    // Every strip names its own controls, so a knob on channel D can be read
    // without looking all the way across to channel A.
    const auto captionWidth = area.getWidth() * 2 / 5;
    auto row = strip.captions.begin();

    const auto placeRow = [&] (juce::Component& control, int height)
    {
        auto bounds = area.removeFromTop (height);
        (row++)->setBounds (bounds.removeFromLeft (captionWidth));
        control.setBounds (bounds.reduced (2, height == comboRowHeight ? 1 : 2));
    };

    for (auto& knob : strip.eq)
        placeRow (knob, knobRowHeight);

    placeRow (strip.filter, knobRowHeight);
    placeRow (strip.echo, knobRowHeight);
    placeRow (strip.echoBeats, comboRowHeight);
    placeRow (strip.reverb, knobRowHeight);
    placeRow (strip.assign, comboRowHeight);

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
