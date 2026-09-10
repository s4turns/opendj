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

#include <iostream>

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
    if (const auto opened = library.open (Library::defaultFile()); opened.failed())
        libraryError = opened.getErrorMessage();
    else
        engine.setAnalysisCache (&library);

    dispatcher.onStateChanged = [safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        // Actions arrive on the MIDI thread, so bounce the redraw to the message
        // thread rather than touching components from there.
        juce::MessageManager::callAsync ([safe]
        {
            if (safe == nullptr)
                return;

            for (auto& view : safe->deckViews)
                if (view != nullptr)
                    view->refresh();

            // The mixer too: a hardware EQ knob or crossfader is an action like
            // any other, and the interface has to show it moved.
            if (safe->mixerView != nullptr)
                safe->mixerView->refresh();
        });
    };

    for (const auto& folder : findMappingsFolders())
    {
        const auto loaded = midi.loadMappingsFromFolder (folder);

        if (juce::SystemStats::getEnvironmentVariable ("OPENDJ_MIDI_TRACE", {}).getIntValue() != 0)
            std::cerr << "[opendj midi] " << loaded << " mapping(s) from "
                      << folder.getFullPathName() << std::endl;
    }

    midi.openFirstRecognisedDevice();

    // Audio comes after MIDI on purpose. The mappings name the controllers this
    // build knows, and a DJ controller's audio interface carries the same name
    // as its MIDI port, so the list that finds the knobs also finds the outputs
    // they belong to. Playing into the machine's default output instead means
    // no headphone cue at all and a jog wheel felt through desktop latency.
    startupError = engine.initialise (midi.getDeviceNameHints());

    if (juce::SystemStats::getEnvironmentVariable ("OPENDJ_MIDI_TRACE", {}).getIntValue() != 0)
        std::cerr << "[opendj audio] "
                  << (startupError.isNotEmpty()
                        ? "FAILED: " + startupError
                        : engine.getDeviceDescription() + "  |  " + engine.getDeviceChoiceReason())
                  << std::endl;

    addAndMakeVisible (deckRow);

    for (int i = 0; i < AudioEngine::numDecks; ++i)
    {
        deckViews[(size_t) i] = std::make_unique<DeckComponent> (
            engine, i, juce::String::charToString ('A' + (juce::juce_wchar) i));
        deckRow.addAndMakeVisible (*deckViews[(size_t) i]);
    }

    mixerView = std::make_unique<MixerComponent> (engine.getMixer());
    deckRow.addAndMakeVisible (*mixerView);

    deckRow.onResized = [this] (juce::Rectangle<int> area)
    {
        const auto mixerWidth = juce::jlimit (220, 300, area.getWidth() / 4);
        const auto deckWidth = (area.getWidth() - mixerWidth - 16) / 2;

        deckViews[0]->setBounds (area.removeFromLeft (deckWidth));
        area.removeFromLeft (8);
        deckViews[1]->setBounds (area.removeFromRight (deckWidth));
        area.removeFromRight (8);
        mixerView->setBounds (area);
    };

    browser = std::make_unique<BrowserComponent> (library, scanner);
    browser->onLoad = [this] (const juce::File& file, int deckIndex) { loadOntoDeck (file, deckIndex); };
    addAndMakeVisible (*browser);

    // The controller's browse encoder and load buttons reach the browser through
    // the dispatcher, the same way every other input does.
    dispatcher.selectedFileProvider = [this] (int) { return browser->getSelectedFile(); };
    dispatcher.browseScrollHandler = [safe = juce::Component::SafePointer<MainComponent> (this)] (int rowsToMove)
    {
        juce::MessageManager::callAsync ([safe, rowsToMove]
        {
            if (safe != nullptr && safe->browser != nullptr)
                safe->browser->moveSelection (rowsToMove);
        });
    };

    // Decks and mixer on top, browser underneath, and a bar between to drag.
    verticalLayout.setItemLayout (0, 320, -1.0, -0.62);
    verticalLayout.setItemLayout (1, 6, 6, 6);
    verticalLayout.setItemLayout (2, 140, -1.0, -0.38);
    resizerBar = std::make_unique<juce::StretchableLayoutResizerBar> (&verticalLayout, 1, false);
    addAndMakeVisible (*resizerBar);

    if (! library.getFolders().empty())
        scanner.start();

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
    setSize (1280, 960);
}

MainComponent::~MainComponent()
{
    stopTimer();
    dispatcher.onStateChanged = nullptr;
    dispatcher.selectedFileProvider = nullptr;
    dispatcher.browseScrollHandler = nullptr;
    removeKeyListener (this);

    scanner.stop();
    engine.setAnalysisCache (nullptr);
}

void MainComponent::loadOntoDeck (const juce::File& file, int deckIndex)
{
    if (deckIndex < 0)
    {
        // A double-click goes to a deck that is not playing. With both decks in
        // the mix there is no safe answer, so nothing happens rather than the
        // wrong thing.
        for (int i = 0; i < AudioEngine::numDecks; ++i)
        {
            if (! engine.getDeck (i).isPlaying())
            {
                deckIndex = i;
                break;
            }
        }

        if (deckIndex < 0)
            return;
    }

    if (juce::isPositiveAndBelow (deckIndex, AudioEngine::numDecks))
        deckViews[(size_t) deckIndex]->load (file);
}

juce::Array<juce::File> MainComponent::findMappingsFolders() const
{
    juce::Array<juce::File> folders;

    const auto add = [&folders] (const juce::File& folder)
    {
        if (folder.isDirectory() && ! folders.contains (folder))
            folders.add (folder);
    };

    const auto dataFolder = [] (const juce::File& root) { return root.getChildFile ("opendj/mappings"); };

   #if JUCE_LINUX || JUCE_BSD
    // The user's own mappings come first, so one written here replaces a
    // shipped mapping of the same name instead of competing with it.
    const auto dataHome = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_HOME", {});

    add (dataFolder (dataHome.isNotEmpty()
                        ? juce::File (dataHome)
                        : juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                              .getChildFile (".local/share")));
   #endif

    // Beside the executable while developing, so a freshly built binary finds
    // the mappings in the source tree, and under the prefix once installed,
    // where /usr/local/bin/opendj must reach /usr/local/share/opendj/mappings.
    const auto executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

    for (auto directory = executable.getParentDirectory();
         directory.exists() && directory != directory.getParentDirectory();
         directory = directory.getParentDirectory())
    {
        add (directory.getChildFile ("mappings"));
        add (dataFolder (directory.getChildFile ("share")));
    }

   #if JUCE_LINUX || JUCE_BSD
    auto dataDirs = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_DIRS", {});

    if (dataDirs.isEmpty())
        dataDirs = "/usr/local/share:/usr/share";

    for (const auto& directory : juce::StringArray::fromTokens (dataDirs, ":", {}))
        if (directory.isNotEmpty())
            add (dataFolder (juce::File (directory)));
   #endif

    return folders;
}

void MainComponent::loadInitialTracks (const juce::StringArray& paths)
{
    auto deckIndex = 0;

    for (const auto& path : paths)
    {
        if (deckIndex >= AudioEngine::numDecks)
            break;

        // Tokens keep the quotes that held a path with spaces together, so they
        // have to come off before the name means anything.
        if (const juce::File file (path.unquoted()); file.existsAsFile())
            deckViews[(size_t) deckIndex++]->load (file);
    }
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

    if (libraryError.isNotEmpty())
        status << "  |  Library error: " << libraryError;

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
        if (deckViews[(size_t) i]->getBounds().contains (deckRow.getLocalPoint (this, dropPoint)))
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

    juce::Component* rows[] = { &deckRow, resizerBar.get(), browser.get() };
    verticalLayout.layOutComponents (rows, 3, area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                                     true, true);
}

} // namespace opendj
