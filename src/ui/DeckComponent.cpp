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

DeckComponent::DeckComponent (AudioEngine& engineToUse, int deckIndex, const juce::String& deckName)
    : engine (engineToUse), index (deckIndex), name (deckName),
      deck (engineToUse.getDeck (deckIndex)), platter (deck, deckName)
{
    addAndMakeVisible (platter);

    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    titleLabel.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    addAndMakeVisible (titleLabel);

    timeLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    timeLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (timeLabel);

    bpmLabel.setColour (juce::Label::textColourId, accentColour);
    bpmLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    bpmLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (bpmLabel);

    loadButton.onClick = [this] { loadButtonClicked(); };
    addAndMakeVisible (loadButton);

    playButton.onClick = [this] { deck.togglePlay(); refresh(); };
    addAndMakeVisible (playButton);

    cueButton.setColour (juce::TextButton::buttonColourId, cueColour.darker (0.6f));
    cueButton.onPress = [this] { deck.cuePressed(); refresh(); };
    cueButton.onRelease = [this] { deck.cueReleased(); refresh(); };
    addAndMakeVisible (cueButton);

    syncButton.onClick = [this]
    {
        // With two decks the other one is always the leader.
        if (engine.syncDeck (index, 1 - index))
            updateTempoReadout();
    };
    addAndMakeVisible (syncButton);

    keyLockButton.setTooltip ("Key lock: the tempo fader stops changing the pitch");
    keyLockButton.onClick = [this] { deck.toggleKeyLock(); refresh(); };
    addAndMakeVisible (keyLockButton);

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

    tempoLabel.setJustificationType (juce::Justification::centred);
    tempoLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible (tempoLabel);

    addAndMakeVisible (scrollingWave);

    overviewWave.onSeek = [this] (double seconds) { deck.seekToSeconds (seconds); refresh(); };
    addAndMakeVisible (overviewWave);

    refresh();
}

DeckComponent::~DeckComponent() = default;

void DeckComponent::load (const juce::File& file)
{
    engine.loadTrackAsync (index, file, [safe = juce::Component::SafePointer<DeckComponent> (this),
                                         file] (bool succeeded)
    {
        if (safe == nullptr)
            return;

        if (! succeeded)
            juce::NativeMessageBox::showMessageBoxAsync (
                juce::MessageBoxIconType::WarningIcon,
                "Could not load track",
                "OpenDJ could not decode " + file.getFileName()
                    + ".\n\nIt may be an unsupported format, longer than 30 minutes, or damaged.");

        safe->refresh();
    });

    refresh();
}

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

        if (file != juce::File())
            load (file);
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
    deck.setTempoRatio (1.0 + tempoSlider.getValue() * rangePercent / 100.0);
    updateTempoReadout();
}

void DeckComponent::updateTempoReadout()
{
    const auto percent = (deck.getTempoRatio() - 1.0) * 100.0;
    tempoLabel.setText (juce::String (percent, 1) + "%", juce::dontSendNotification);

    const auto bpm = engine.getEffectiveBpm (index);
    bpmLabel.setText (bpm > 0.0 ? juce::String (bpm, 1) + " BPM" : juce::String ("-- BPM"),
                      juce::dontSendNotification);
}

void DeckComponent::refresh()
{
    const auto loading = engine.isDeckLoading (index);
    const auto length = deck.getLengthSeconds();
    const auto position = deck.getPositionSeconds();

    if (auto analysis = deck.getAnalysis(); analysis != shownAnalysis)
    {
        shownAnalysis = std::move (analysis);
        scrollingWave.setAnalysis (shownAnalysis);
        overviewWave.setAnalysis (shownAnalysis);
    }

    platter.refresh (shownAnalysis);
    scrollingWave.setPosition (position, length);
    overviewWave.setPosition (position, length);
    scrollingWave.setCuePoint (deck.getCueSeconds());
    overviewWave.setCuePoint (deck.getCueSeconds());

    const auto title = loading ? juce::String ("analysing...")
                               : (deck.isLoaded() ? deck.getTrackTitle() : juce::String ("empty"));

    titleLabel.setText ("Deck " + name + "  |  " + title, juce::dontSendNotification);
    timeLabel.setText (formatTime (position) + " / " + formatTime (length), juce::dontSendNotification);

    playButton.setButtonText (deck.isPlaying() ? "Pause" : "Play");
    playButton.setColour (juce::TextButton::buttonColourId,
                          deck.isPlaying() ? accentColour.darker (0.4f) : juce::Colour (0xff2c2c34));

    syncButton.setEnabled (engine.getEffectiveBpm (index) > 0.0
                           && engine.getEffectiveBpm (1 - index) > 0.0);

    keyLockButton.setColour (juce::TextButton::buttonColourId,
                             deck.isKeyLockEnabled() ? accentColour.darker (0.4f) : juce::Colour (0xff2c2c34));

    updateTempoReadout();

    if (loading != wasLoading)
    {
        wasLoading = loading;
        loadButton.setEnabled (! loading);
    }
}

bool DeckComponent::isInterestedInDragSource (const SourceDetails& details)
{
    // The browser hands over a path; anything else is someone else's drag.
    return details.description.isString() && juce::File (details.description.toString()).existsAsFile();
}

void DeckComponent::itemDragEnter (const SourceDetails&)
{
    dragHovering = true;
    repaint();
}

void DeckComponent::itemDragExit (const SourceDetails&)
{
    dragHovering = false;
    repaint();
}

void DeckComponent::itemDropped (const SourceDetails& details)
{
    dragHovering = false;
    repaint();
    load (juce::File (details.description.toString()));
}

void DeckComponent::paint (juce::Graphics& g)
{
    g.setColour (panelColour);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);

    if (dragHovering)
    {
        g.setColour (accentColour);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 6.0f, 3.0f);
    }
}

void DeckComponent::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto header = area.removeFromTop (24);
    timeLabel.setBounds (header.removeFromRight (130));
    bpmLabel.setBounds (header.removeFromRight (90));
    titleLabel.setBounds (header);

    area.removeFromTop (8);
    scrollingWave.setBounds (area.removeFromTop (96));
    area.removeFromTop (4);
    overviewWave.setBounds (area.removeFromTop (34));

    area.removeFromTop (10);

    auto tempoColumn = area.removeFromRight (74);
    tempoRangeBox.setBounds (tempoColumn.removeFromTop (24));
    tempoLabel.setBounds (tempoColumn.removeFromBottom (20));
    tempoSlider.setBounds (tempoColumn.reduced (12, 4));

    area.removeFromRight (10);

    // The platter takes whatever square it can get, leaving room beside it for
    // the transport column, and the window can be made small enough that it has
    // to give up rather than push the buttons off the panel.
    const auto transportWidth = 88;
    const auto platterSize = juce::jlimit (0,
                                           juce::jmax (0, area.getWidth() - transportWidth - 10),
                                           juce::jmin (240, area.getHeight()));

    if (platterSize > 60)
    {
        auto platterArea = area.removeFromLeft (platterSize);
        platter.setBounds (platterArea.removeFromTop (platterSize));
        area.removeFromLeft (10);
        platter.setVisible (true);
    }
    else
    {
        platter.setVisible (false);
    }

    auto transport = area.removeFromLeft (juce::jmin (transportWidth, area.getWidth()));

    // Spelled out, because the cue button is a different type to the others.
    const std::initializer_list<juce::Button*> transportButtons
    {
        &loadButton, &cueButton, &playButton, &syncButton, &keyLockButton
    };

    for (auto* button : transportButtons)
    {
        button->setBounds (transport.removeFromTop (36).reduced (0, 3));

        if (transport.getHeight() <= 0)
            break;
    }
}

} // namespace opendj
