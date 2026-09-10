/*
    This file is part of OpenDJ.
    Copyright (C) 2026 The OpenDJ contributors.

    OpenDJ is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. See the LICENSE file at the root of this repository.
*/

#include "app/MainComponent.h"

#include "ui/MidiSetupComponent.h"

namespace opendj
{

namespace
{
    constexpr int refreshRateHz = 30;

    // Deck A on the left of the keyboard, deck B on the right, which is where
    // every DJ application puts them.
    struct KeyBinding { int keyCode; int deck; Action action; };

    const KeyBinding keyBindings[] =
    {
        { 'Q', 0, Action::deckCue },
        { 'W', 0, Action::deckPlayToggle },
        { 'O', 1, Action::deckCue },
        { 'P', 1, Action::deckPlayToggle }
    };
}

MainComponent::MainComponent()
{
    startupError = engine.initialise();

    dispatcher.onStateChanged = [safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        // Actions arrive on the MIDI thread, so bounce the redraw to the message
        // thread rather than touching components from there.
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
                for (auto& view : safe->deckViews)
                    if (view != nullptr)
                        view->refresh();
        });
    };

    midi.loadMappingsFromFolder (findMappingsFolder());
    midi.openFirstRecognisedDevice();

    for (int i = 0; i < AudioEngine::numDecks; ++i)
    {
        deckViews[(size_t) i] = std::make_unique<DeckComponent> (
            engine, i, juce::String::charToString ('A' + (juce::juce_wchar) i));
        addAndMakeVisible (*deckViews[(size_t) i]);
    }

    mixerView = std::make_unique<MixerComponent> (engine.getMixer());
    addAndMakeVisible (*mixerView);

    audioSettingsButton.onClick = [this] { showAudioSettings(); };
    addAndMakeVisible (audioSettingsButton);

    midiSettingsButton.onClick = [this] { showMidiSettings(); };
    addAndMakeVisible (midiSettingsButton);

    statusLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (statusLabel);

    addKeyListener (this);
    setWantsKeyboardFocus (true);

    startTimerHz (refreshRateHz);
    setSize (1200, 760);
}

MainComponent::~MainComponent()
{
    stopTimer();
    dispatcher.onStateChanged = nullptr;
    removeKeyListener (this);
}

juce::File MainComponent::findMappingsFolder() const
{
    // Beside the executable in an installed copy, and somewhere up the tree from
    // the build directory while developing, so a freshly built binary finds them.
    const auto executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

    for (auto directory = executable.getParentDirectory();
         directory.exists() && directory != directory.getParentDirectory();
         directory = directory.getParentDirectory())
    {
        if (const auto candidate = directory.getChildFile ("mappings"); candidate.isDirectory())
            return candidate;
    }

    return {};
}

void MainComponent::timerCallback()
{
    for (auto& view : deckViews)
        view->refresh();

    mixerView->refresh();

    auto status = startupError.isNotEmpty() ? "Audio error: " + startupError
                                            : engine.getDeviceDescription();

    if (midi.isOpen())
        status << "  |  " << midi.getOpenDeviceName();

    statusLabel.setText (status, juce::dontSendNotification);
}

bool MainComponent::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    for (const auto& binding : keyBindings)
    {
        if (! key.isKeyCode (binding.keyCode))
            continue;

        if (binding.action == Action::deckCue)
        {
            // Key repeat would re-trigger cue over and over, so only the first
            // press counts; keyStateChanged sends the matching release.
            if (cueKeyHeld[(size_t) binding.deck])
                return true;

            cueKeyHeld[(size_t) binding.deck] = true;
        }

        dispatcher.dispatch ({ binding.action, binding.deck, 0, 1.0f });
        return true;
    }

    return false;
}

bool MainComponent::keyStateChanged (bool, juce::Component*)
{
    for (const auto& binding : keyBindings)
    {
        if (binding.action != Action::deckCue)
            continue;

        auto& held = cueKeyHeld[(size_t) binding.deck];

        if (held && ! juce::KeyPress::isKeyCurrentlyDown (binding.keyCode))
        {
            held = false;
            dispatcher.dispatch ({ Action::deckCue, binding.deck, 0, 0.0f });
        }
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
        if (deckViews[(size_t) i]->getBounds().contains (dropPoint))
        {
            deckViews[(size_t) i]->load (juce::File (files[0]));
            return;
        }
    }
}

void MainComponent::showAudioSettings()
{
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
        engine.getDeviceManager(),
        0, 0,      // no inputs yet
        2, 8,      // enough outputs for a master pair plus a cue pair
        false,     // MIDI has its own panel
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

void MainComponent::showMidiSettings()
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new MidiSetupComponent (midi));
    options.dialogTitle = "Controller setup";
    options.dialogBackgroundColour = juce::Colour (0xff1c1c22);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
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
    audioSettingsButton.setBounds (footer.removeFromLeft (100));
    footer.removeFromLeft (6);
    midiSettingsButton.setBounds (footer.removeFromLeft (94));
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
