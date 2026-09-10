/*
    This file is part of OpenDJ.
    Copyright (C) 2026 The OpenDJ contributors.

    OpenDJ is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. See the LICENSE file at the root of this repository.
*/

#include "app/MainComponent.h"

namespace opendj
{

namespace
{
    constexpr int refreshRateHz = 30;

    // Deck A on the left of the keyboard, deck B on the right, which is the
    // layout every DJ application converges on.
    struct KeyBinding { int keyCode; int deck; enum Action { play, cue } action; };

    const KeyBinding keyBindings[] =
    {
        { 'Q', 0, KeyBinding::cue },
        { 'W', 0, KeyBinding::play },
        { 'O', 1, KeyBinding::cue },
        { 'P', 1, KeyBinding::play }
    };
}

MainComponent::MainComponent()
{
    startupError = engine.initialise();

    for (int i = 0; i < AudioEngine::numDecks; ++i)
    {
        deckViews[(size_t) i] = std::make_unique<DeckComponent> (engine.getDeck (i),
                                                                 juce::String::charToString ('A' + (juce::juce_wchar) i));
        addAndMakeVisible (*deckViews[(size_t) i]);
    }

    mixerView = std::make_unique<MixerComponent> (engine.getMixer());
    addAndMakeVisible (*mixerView);

    settingsButton.onClick = [this] { showAudioSettings(); };
    addAndMakeVisible (settingsButton);

    statusLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (statusLabel);

    addKeyListener (this);
    setWantsKeyboardFocus (true);

    startTimerHz (refreshRateHz);
    setSize (1200, 720);
}

MainComponent::~MainComponent()
{
    stopTimer();
    removeKeyListener (this);
}

void MainComponent::timerCallback()
{
    for (auto& view : deckViews)
        view->refresh();

    mixerView->refresh();

    statusLabel.setText (startupError.isNotEmpty() ? "Audio error: " + startupError
                                                   : engine.getDeviceDescription(),
                         juce::dontSendNotification);
}

bool MainComponent::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    for (const auto& binding : keyBindings)
    {
        if (! key.isKeyCode (binding.keyCode))
            continue;

        auto& deck = engine.getDeck (binding.deck);

        if (binding.action == KeyBinding::play)
            deck.togglePlay();
        else
            deck.cuePressed();   // a key repeat is not a hold, so no preview release

        return true;
    }

    return false;
}

bool MainComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
        if (engine.getFormatManager().findFormatForFileExtension (juce::File (path).getFileExtension()) != nullptr)
            return true;

    return false;
}

void MainComponent::filesDropped (const juce::StringArray& files, int x, int y)
{
    if (files.isEmpty())
        return;

    const juce::Point<int> dropPoint (x, y);

    for (int i = 0; i < AudioEngine::numDecks; ++i)
    {
        if (! deckViews[(size_t) i]->getBounds().contains (dropPoint))
            continue;

        if (engine.getDeck (i).loadFile (juce::File (files[0])))
            deckViews[(size_t) i]->refresh();

        return;
    }
}

void MainComponent::showAudioSettings()
{
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
        engine.getDeviceManager(),
        0, 0,      // no inputs yet
        2, 8,      // enough outputs for a master pair plus a cue pair
        false,     // MIDI input list arrives with the mapping engine
        false,
        true,
        false);

    selector->setSize (500, 420);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (selector.release());
    options.dialogTitle = "Audio setup";
    options.dialogBackgroundColour = juce::Colour (0xff1c1c22);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff111116));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto footer = area.removeFromBottom (26);
    settingsButton.setBounds (footer.removeFromLeft (110));
    footer.removeFromLeft (12);
    statusLabel.setBounds (footer);

    area.removeFromBottom (8);

    const auto mixerWidth = juce::jlimit (220, 300, area.getWidth() / 4);
    const auto deckWidth = (area.getWidth() - mixerWidth - 16) / 2;

    deckViews[0]->setBounds (area.removeFromLeft (deckWidth));
    area.removeFromLeft (8);
    deckViews[1]->setBounds (area.removeFromRight (deckWidth));
    area.removeFromRight (8);
    mixerView->setBounds (area);
}

} // namespace opendj
