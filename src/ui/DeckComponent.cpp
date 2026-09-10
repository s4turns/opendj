/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/DeckComponent.h"

namespace opendj
{

namespace
{
    juce::String formatTime (double seconds)
    {
        if (seconds < 0.0 || ! std::isfinite (seconds))
            seconds = 0.0;

        const auto total = static_cast<int> (seconds);
        return juce::String::formatted ("%d:%02d.%d",
                                        total / 60,
                                        total % 60,
                                        static_cast<int> ((seconds - total) * 10.0));
    }

    const juce::Colour panelColour   { 0xff1c1c22 };
    const juce::Colour accentColour  { 0xff35c2f0 };
    const juce::Colour cueColour     { 0xffe8a33d };
}

DeckComponent::DeckComponent (Deck& deckToControl, const juce::String& deckName)
    : deck (deckToControl), name (deckName)
{
    titleLabel.setText ("Deck " + name + "  |  empty", juce::dontSendNotification);
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    titleLabel.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    addAndMakeVisible (titleLabel);

    timeLabel.setText ("0:00.0 / 0:00.0", juce::dontSendNotification);
    timeLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    timeLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (timeLabel);

    loadButton.onClick = [this] { loadButtonClicked(); };
    addAndMakeVisible (loadButton);

    playButton.onClick = [this] { deck.togglePlay(); refresh(); };
    addAndMakeVisible (playButton);

    cueButton.setColour (juce::TextButton::buttonColourId, cueColour.darker (0.6f));
    cueButton.onPress = [this] { deck.cuePressed(); refresh(); };
    cueButton.onRelease = [this] { deck.cueReleased(); refresh(); };
    addAndMakeVisible (cueButton);

    tempoSlider.setSliderStyle (juce::Slider::LinearVertical);
    tempoSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    tempoSlider.setRange (-1.0, 1.0, 0.0);
    tempoSlider.setValue (0.0, juce::dontSendNotification);
    tempoSlider.setDoubleClickReturnValue (true, 0.0);
    tempoSlider.onValueChange = [this] { applyTempoFromSlider(); };
    addAndMakeVisible (tempoSlider);

    tempoRangeBox.addItemList ({ "8%", "16%", "50%" }, 1);
    tempoRangeBox.setSelectedId (1, juce::dontSendNotification);
    tempoRangeBox.onChange = [this] { applyTempoFromSlider(); };
    addAndMakeVisible (tempoRangeBox);

    tempoLabel.setText ("0.0%", juce::dontSendNotification);
    tempoLabel.setJustificationType (juce::Justification::centred);
    tempoLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible (tempoLabel);
}

DeckComponent::~DeckComponent() = default;

void DeckComponent::loadButtonClicked()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Load a track onto deck " + name,
                                                       juce::File(),
                                                       "*.wav;*.aiff;*.aif;*.flac;*.mp3;*.ogg");

    const auto browserFlags = juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles;

    fileChooser->launchAsync (browserFlags, [this] (const juce::FileChooser& chooser)
    {
        const auto file = chooser.getResult();

        if (file == juce::File())
            return;

        if (! deck.loadFile (file))
        {
            juce::NativeMessageBox::showMessageBoxAsync (
                juce::MessageBoxIconType::WarningIcon,
                "Could not load track",
                "OpenDJ could not decode " + file.getFileName()
                    + ".\n\nIt may be an unsupported format, longer than 30 minutes, or damaged.");
            return;
        }

        refresh();
    });
}

void DeckComponent::applyTempoFromSlider()
{
    const auto rangePercent = [this]
    {
        switch (tempoRangeBox.getSelectedId())
        {
            case 2:  return 16.0;
            case 3:  return 50.0;
            default: return 8.0;
        }
    }();

    // The fader reads the way a DJ expects: up is faster.
    const auto percent = tempoSlider.getValue() * rangePercent;
    deck.setTempoRatio (1.0 + percent / 100.0);

    tempoLabel.setText (juce::String (percent, 1) + "%", juce::dontSendNotification);
}

void DeckComponent::refresh()
{
    const auto length = deck.getLengthSeconds();
    const auto position = deck.getPositionSeconds();

    positionProportion = length > 0.0 ? position / length : 0.0;
    cueProportion = length > 0.0 ? deck.getCueSeconds() / length : 0.0;

    const auto title = deck.isLoaded() ? deck.getTrackTitle() : juce::String ("empty");
    titleLabel.setText ("Deck " + name + "  |  " + title, juce::dontSendNotification);

    timeLabel.setText (formatTime (position) + " / " + formatTime (length),
                       juce::dontSendNotification);

    playButton.setButtonText (deck.isPlaying() ? "Pause" : "Play");
    playButton.setColour (juce::TextButton::buttonColourId,
                          deck.isPlaying() ? accentColour.darker (0.4f)
                                           : juce::Colour (0xff2c2c34));

    repaint (seekStripBounds);
}

void DeckComponent::paint (juce::Graphics& g)
{
    g.setColour (panelColour);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);

    // Seek strip: the waveform view takes this over once analysis lands.
    auto strip = seekStripBounds.toFloat();
    g.setColour (juce::Colour (0xff101014));
    g.fillRoundedRectangle (strip, 3.0f);

    if (deck.isLoaded())
    {
        auto played = strip.withWidth (strip.getWidth() * static_cast<float> (positionProportion));
        g.setColour (accentColour.withAlpha (0.55f));
        g.fillRoundedRectangle (played, 3.0f);

        const auto cueX = strip.getX() + strip.getWidth() * static_cast<float> (cueProportion);
        g.setColour (cueColour);
        g.fillRect (cueX - 1.0f, strip.getY(), 2.0f, strip.getHeight());
    }
    else
    {
        g.setColour (juce::Colours::darkgrey);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Drop a track here or press Load", strip, juce::Justification::centred);
    }
}

void DeckComponent::mouseDown (const juce::MouseEvent& e)
{
    seekFromMouse (e);
}

void DeckComponent::mouseDrag (const juce::MouseEvent& e)
{
    seekFromMouse (e);
}

void DeckComponent::seekFromMouse (const juce::MouseEvent& e)
{
    if (! deck.isLoaded() || ! seekStripBounds.contains (e.getPosition()))
        return;

    const auto proportion = (e.position.x - seekStripBounds.getX()) / seekStripBounds.getWidth();
    deck.seekToFraction (proportion);
    refresh();
}

void DeckComponent::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto header = area.removeFromTop (24);
    timeLabel.setBounds (header.removeFromRight (150));
    titleLabel.setBounds (header);

    area.removeFromTop (8);
    seekStripBounds = area.removeFromTop (54);

    area.removeFromTop (10);

    auto tempoColumn = area.removeFromRight (74);
    tempoRangeBox.setBounds (tempoColumn.removeFromTop (24));
    tempoLabel.setBounds (tempoColumn.removeFromBottom (20));
    tempoSlider.setBounds (tempoColumn.reduced (12, 4));

    area.removeFromRight (10);

    auto transport = area.removeFromTop (40);
    loadButton.setBounds (transport.removeFromLeft (70).reduced (0, 2));
    transport.removeFromLeft (8);
    cueButton.setBounds (transport.removeFromLeft (70).reduced (0, 2));
    transport.removeFromLeft (8);
    playButton.setBounds (transport.removeFromLeft (90).reduced (0, 2));
}

} // namespace opendj
