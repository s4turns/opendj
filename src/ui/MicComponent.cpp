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

MicComponent::MicComponent (AudioEngine& engineToUse, MicInput& inputToShow, const juce::String& title)
    : engine (engineToUse),
      input (inputToShow),
      name (title),
      hasTalkover (inputToShow.getSource() == MicInput::Source::mono)
{
    heading.setText (name, juce::dontSendNotification);
    heading.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
    heading.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (heading);

    onButton.setClickingTogglesState (true);
    onButton.setColour (juce::TextButton::buttonOnColourId, liveColour);
    onButton.setTooltip ("Put the " + name.toLowerCase() + " on air");
    onButton.onClick = [this]
    {
        const auto on = onButton.getToggleState();
        input.setEnabled (on);

        if (! on || onMessage == nullptr)
            return;

        if (engine.getNumInputChannels() == 0)
            onMessage ("The " + name.toLowerCase() + " is on, but no input is selected. Choose one in Audio setup.");
        else if (2 * input.getInputPair() >= engine.getNumInputChannels())
            onMessage ("The " + name.toLowerCase() + " is on, but the device has no inputs "
                       + inputButton.getButtonText().fromFirstOccurrenceOf ("In ", false, false)
                       + ". Pick another pair, or turn more inputs on in Audio setup.");
    };
    addAndMakeVisible (onButton);

    level.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    level.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    level.setRange (0.0, 2.0, 0.001);
    level.setDoubleClickReturnValue (true, 1.0);
    level.setTooltip ("The " + name.toLowerCase() + "'s level. Double-click for unity");
    level.setColour (juce::Slider::rotarySliderFillColourId, accentColour);
    level.onValueChange = [this] { input.setGain ((float) level.getValue()); };
    addAndMakeVisible (level);

    levelLabel.setText ("Level", juce::dontSendNotification);
    levelLabel.setJustificationType (juce::Justification::centred);
    levelLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    levelLabel.setFont (juce::FontOptions (11.0f));
    levelLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (levelLabel);

    meter.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (meter);

    talkoverButton.setClickingTogglesState (true);
    talkoverButton.setColour (juce::TextButton::buttonOnColourId, accentColour);
    talkoverButton.setTooltip ("Drop the music by 10 dB while the mic is on");
    talkoverButton.onClick = [this] { input.setTalkover (talkoverButton.getToggleState()); };
    addChildComponent (talkoverButton);
    talkoverButton.setVisible (hasTalkover);

    inputButton.setTooltip ("Which pair of the audio device's inputs this reads. Click for the next pair. "
                            "Turn inputs on in Audio setup");
    inputButton.onClick = [this] { nextInputPair(); };
    addAndMakeVisible (inputButton);

    routingButton.setClickingTogglesState (true);
    routingButton.setColour (juce::TextButton::buttonOnColourId, accentColour);
    routingButton.setTooltip ("Everywhere: the speakers, the recording and the stream. "
                              "Stream only: the recording and the stream, never the speakers");
    routingButton.onClick = [this]
    {
        input.setRouting (routingButton.getToggleState() ? MicInput::Routing::recordingOnly
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

namespace
{
    constexpr int headingWidth = 30, onWidth = 44, knobWidth = 42, meterWidth = 8, meterGap = 6,
                  inputWidth = 56, talkoverWidth = 70, routingWidth = 80, marginWidth = 12;
}

int MicComponent::getPreferredWidth() const noexcept
{
    return marginWidth + headingWidth + onWidth + knobWidth + meterWidth + meterGap
         + inputWidth + (hasTalkover ? talkoverWidth : 0) + routingWidth;
}

void MicComponent::resized()
{
    auto area = getLocalBounds().reduced (marginWidth / 2, 4);

    heading.setBounds (area.removeFromLeft (headingWidth));
    onButton.setBounds (area.removeFromLeft (onWidth).reduced (2, 6));
    auto knobCell = area.removeFromLeft (knobWidth).reduced (2);
    levelLabel.setBounds (knobCell.removeFromBottom (12));
    level.setBounds (knobCell);
    meter.setBounds (area.removeFromLeft (meterWidth).reduced (0, 6));
    area.removeFromLeft (meterGap);
    inputButton.setBounds (area.removeFromLeft (inputWidth).reduced (2, 6));

    if (hasTalkover)
        talkoverButton.setBounds (area.removeFromLeft (talkoverWidth).reduced (2, 6));

    routingButton.setBounds (area.reduced (2, 6));
}

void MicComponent::nextInputPair()
{
    // Up to the pairs the device has on, and always at least the first, so
    // the button still says something useful before any input is chosen.
    const auto pairs = juce::jlimit (1, AudioEngine::maxInputChannels / 2,
                                     (engine.getNumInputChannels() + 1) / 2);
    input.setInputPair ((input.getInputPair() + 1) % pairs);
    updateControls();
}

//==============================================================================

void MicComponent::timerCallback()
{
    // Holds the loudest moment and falls from it, about 20 dB a second.
    const auto peak = input.getAndResetPeak();
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
    auto& mic = input;

    const auto first = 2 * mic.getInputPair() + 1;
    const auto pairText = "In " + juce::String (first) + "-" + juce::String (first + 1);

    if (inputButton.getButtonText() != pairText)
        inputButton.setButtonText (pairText);

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
