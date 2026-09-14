/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/MicComponent.h"

#include <cmath>

namespace opendj
{

namespace
{
    const juce::Colour panelColour  { 0xff1c1c22 };
    const juce::Colour accentColour { 0xff35c2f0 };
    const juce::Colour liveColour   { 0xffe0433b };

    constexpr float meterFloorDecibels = -60.0f;
}

//==============================================================================

void MicComponent::Meter::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.fillRoundedRectangle (bounds, 2.0f);

    // In decibels rather than a straight line, so a quiet voice still shows.
    const auto decibels = juce::Decibels::gainToDecibels (level, meterFloorDecibels);
    const auto proportion = juce::jlimit (0.0f, 1.0f, 1.0f - decibels / meterFloorDecibels);

    g.setColour (level >= 0.9f ? liveColour : accentColour);
    g.fillRoundedRectangle (bounds.removeFromBottom (bounds.getHeight() * proportion), 2.0f);
}

//==============================================================================

MicComponent::MicComponent (AudioEngine& engineToUse) : engine (engineToUse)
{
    heading.setText ("Mic", juce::dontSendNotification);
    heading.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
    heading.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (heading);

    onButton.setClickingTogglesState (true);
    onButton.setColour (juce::TextButton::buttonOnColourId, liveColour);
    onButton.setTooltip ("Put the mic on air");
    onButton.onClick = [this]
    {
        const auto on = onButton.getToggleState();
        engine.getMic().setEnabled (on);

        if (on && engine.getNumInputChannels() == 0 && onMessage != nullptr)
            onMessage ("The mic is on, but no input is selected. Choose one in Audio setup.");
    };
    addAndMakeVisible (onButton);

    level.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    level.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    level.setRange (0.0, 2.0, 0.001);
    level.setDoubleClickReturnValue (true, 1.0);
    level.setTooltip ("The mic's level. Double-click for unity");
    level.setColour (juce::Slider::rotarySliderFillColourId, accentColour);
    level.onValueChange = [this] { engine.getMic().setGain ((float) level.getValue()); };
    addAndMakeVisible (level);

    meter.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (meter);

    talkoverButton.setClickingTogglesState (true);
    talkoverButton.setColour (juce::TextButton::buttonOnColourId, accentColour);
    talkoverButton.setTooltip ("Drop the music by 10 dB while the mic is on");
    talkoverButton.onClick = [this] { engine.getMic().setTalkover (talkoverButton.getToggleState()); };
    addAndMakeVisible (talkoverButton);

    routingButton.setClickingTogglesState (true);
    routingButton.setColour (juce::TextButton::buttonOnColourId, accentColour);
    routingButton.setTooltip ("Everywhere: the speakers, the recording and the stream. "
                              "Stream only: the recording and the stream, never the speakers");
    routingButton.onClick = [this]
    {
        engine.getMic().setRouting (routingButton.getToggleState() ? MicInput::Routing::recordingOnly
                                                                   : MicInput::Routing::everywhere);
        updateControls();
    };
    addAndMakeVisible (routingButton);

    updateControls();
    startTimerHz (30);
}

MicComponent::~MicComponent() { stopTimer(); }

void MicComponent::paint (juce::Graphics& g)
{
    g.setColour (panelColour);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 5.0f);
}

void MicComponent::resized()
{
    auto area = getLocalBounds().reduced (6, 4);

    heading.setBounds (area.removeFromLeft (30));
    onButton.setBounds (area.removeFromLeft (48).reduced (2, 6));
    level.setBounds (area.removeFromLeft (44).reduced (2));
    meter.setBounds (area.removeFromLeft (8).reduced (0, 6));
    area.removeFromLeft (6);
    talkoverButton.setBounds (area.removeFromLeft (72).reduced (2, 6));
    routingButton.setBounds (area.reduced (2, 6));
}

//==============================================================================

void MicComponent::timerCallback()
{
    // Holds the loudest moment and falls from it, about 20 dB a second.
    const auto peak = engine.getMic().getAndResetPeak();
    const auto fallen = meter.level * 0.92f;
    const auto next = juce::jmax (peak, fallen < 0.001f ? 0.0f : fallen);

    if (next != meter.level)
    {
        meter.level = next;
        meter.repaint();
    }

    updateControls();
}

void MicComponent::updateControls()
{
    auto& mic = engine.getMic();

    if (onButton.getToggleState() != mic.isEnabled())
        onButton.setToggleState (mic.isEnabled(), juce::dontSendNotification);

    if (talkoverButton.getToggleState() != mic.isTalkoverEnabled())
        talkoverButton.setToggleState (mic.isTalkoverEnabled(), juce::dontSendNotification);

    const auto streamOnly = mic.getRouting() == MicInput::Routing::recordingOnly;

    if (routingButton.getToggleState() != streamOnly)
        routingButton.setToggleState (streamOnly, juce::dontSendNotification);

    if (const juce::String text = streamOnly ? "Stream only" : "Everywhere"; routingButton.getButtonText() != text)
        routingButton.setButtonText (text);

    if (! level.isMouseButtonDown() && std::abs (level.getValue() - (double) mic.getGain()) > 1.0e-4)
        level.setValue (mic.getGain(), juce::dontSendNotification);
}

} // namespace opendj
