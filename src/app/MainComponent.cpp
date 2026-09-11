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
    // The deck here is a side of the window, not a deck index: with the swap
    // button in play, Q and W mean whichever deck is on the left at the time.
    struct KeyBinding { int keyCode; int side; Action action; };

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
    settings = SessionState::readFrom (SessionState::defaultFile());

    const auto savedDevice = juce::XmlDocument::parse (
        SessionState::defaultFile().getSiblingFile ("audio-device.xml"));

    startupError = engine.initialise (midi.getDeviceNameHints(), savedDevice.get());

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
        deckRow.addChildComponent (*deckViews[(size_t) i]);

        // A deck can be swapped for the one behind it: A for C on the left, B
        // for D on the right. Four decks side by side would leave each of them
        // too narrow to read a waveform on, which is the thing a deck is for.
        deckViews[(size_t) i]->onSwapRequested = [this, i] { swapSide (i % 2); };
    }

    showDeck (0);
    showDeck (1);

    mixerView = std::make_unique<MixerComponent> (engine, engine.getMixer());
    deckRow.addAndMakeVisible (*mixerView);

    deckRow.onResized = [this] (juce::Rectangle<int> area)
    {
        // Four strips need about twice the centre section two did, but not at
        // the cost of the waveforms, so it grows with the window instead.
        const auto mixerWidth = juce::jlimit (320, 460, area.getWidth() * 3 / 8);
        const auto deckWidth = (area.getWidth() - mixerWidth - 16) / 2;

        deckViews[(size_t) visibleDecks[0]]->setBounds (area.removeFromLeft (deckWidth));
        area.removeFromLeft (8);
        deckViews[(size_t) visibleDecks[1]]->setBounds (area.removeFromRight (deckWidth));
        area.removeFromRight (8);
        mixerView->setBounds (area);
    };

    // A controller either names the deck it wants or asks for the other pair,
    // and either way the answer has to come back to the message thread before
    // anything on screen moves.
    dispatcher.deckSelectHandler = [this] (int deckIndex)
    {
        juce::MessageManager::callAsync ([this, deckIndex]
        {
            if (deckIndex < 0)
                swapBothSides();
            else
                showDeck (deckIndex);
        });
    };

    samplerView = std::make_unique<SamplerComponent> (engine);
    samplerView->onMessage = [this] (const juce::String& message)
    {
        statusLabel.setText (message, juce::dontSendNotification);
    };
    addAndMakeVisible (*samplerView);

    restoreSettings();

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

    recordButton.setTooltip ("Record the master output to a WAV file, with a tracklist beside it");
    recordButton.onClick = [this] { toggleRecording(); };
    addAndMakeVisible (recordButton);

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
    saveSettings();

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
        // A double-click goes to a deck that is not playing, and to one that is
        // on screen before one that is not: loading a track onto something the
        // user cannot see would look like nothing happened. With every visible
        // deck in the mix there is no safe answer, so nothing happens rather
        // than the wrong thing.
        for (const auto candidate : visibleDecks)
            if (! engine.getDeck (candidate).isPlaying())
            {
                deckIndex = candidate;
                break;
            }

        for (int i = 0; i < AudioEngine::numDecks && deckIndex < 0; ++i)
            if (! engine.getDeck (i).isPlaying())
                deckIndex = i;

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
    // Noted while the window is alive and healthy, never asked for on the way
    // out: by the time this component is destroyed the window that owns it is
    // already half gone, and asking it anything then is an access violation.
    if (auto* window = getTopLevelComponent(); window != nullptr && window != this)
        settings.windowBounds = window->getBounds().toString();


    for (auto& view : deckViews)
        view->refresh();

    mixerView->refresh();

    auto status = startupError.isNotEmpty() ? "Audio error: " + startupError
                                            : engine.getDeviceDescription();

    if (midi.isOpen())
        status << "  |  " << midi.getOpenDeviceName();

    if (libraryError.isNotEmpty())
        status << "  |  Library error: " << libraryError;

    const auto& recorder = engine.getRecorder();

    if (recorder.isRecording())
    {
        const auto seconds = static_cast<int> (recorder.getRecordedSeconds());

        recordButton.setButtonText (juce::String (seconds / 60) + ":"
                                    + juce::String (seconds % 60).paddedLeft ('0', 2));
        recordButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffc0392b));

        status << "  |  recording" << (recorder.hadDropouts() ? " WITH DROPOUTS" : "");
    }
    else
    {
        recordButton.setButtonText ("Record");
        recordButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2c2c34));
    }

    statusLabel.setText (status, juce::dontSendNotification);
}

bool MainComponent::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    for (const auto& binding : keyBindings)
    {
        if (! key.isKeyCode (binding.keyCode))
            continue;

        const auto deckIndex = visibleDecks[(size_t) binding.side];

        if (binding.action == Action::deckCue)
        {
            // Key repeat would re-trigger cue over and over, so only the first
            // press counts; keyStateChanged sends the matching release.
            if (cueKeyHeld[(size_t) deckIndex] != 0)
                return true;

            cueKeyHeld[(size_t) deckIndex] = binding.keyCode;
        }

        dispatcher.dispatch ({ binding.action, deckIndex, 0, 1.0f });
        return true;
    }

    // The number row fires the sampler pads, so a machine with no controller on
    // it still has eight sounds under one hand.
    for (int slot = 0; slot < Sampler::numSlots; ++slot)
    {
        if (key.isKeyCode ('1' + slot))
        {
            dispatcher.dispatch ({ Action::samplerTrigger, 0, slot, 1.0f });
            return true;
        }
    }

    return false;
}

bool MainComponent::keyStateChanged (bool, juce::Component*)
{
    for (const auto& binding : keyBindings)
    {
        if (binding.action != Action::deckCue)
            continue;

        // Released against the deck it was pressed on, so swapping decks with
        // a cue key held down does not leave that deck stuck in preview.
        for (int deckIndex = 0; deckIndex < AudioEngine::numDecks; ++deckIndex)
        {
            auto& heldBy = cueKeyHeld[(size_t) deckIndex];

            if (heldBy == binding.keyCode && ! juce::KeyPress::isKeyCurrentlyDown (heldBy))
            {
                heldBy = 0;
                dispatcher.dispatch ({ Action::deckCue, deckIndex, 0, 0.0f });
            }
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

    // Only the decks on screen, since a hidden one keeps the bounds it had
    // when it was swapped out and would swallow a drop meant for its replacement.
    for (const auto i : visibleDecks)
    {
        if (deckViews[(size_t) i]->getBounds().contains (deckRow.getLocalPoint (this, dropPoint)))
        {
            deckViews[(size_t) i]->load (juce::File (files[0]));
            return;
        }
    }
}

void MainComponent::restoreSettings()
{
    settings.applyTo (engine.getMixer(), engine.getSampler());

    for (int deck = 0; deck < AudioEngine::numDecks; ++deck)
        dispatcher.setTempoRange (deck, settings.tempoRanges[(size_t) deck]);

    showDeck (settings.visibleDecks[0]);
    showDeck (settings.visibleDecks[1]);

    // Pads are reloaded rather than remembered, because the audio behind them
    // lives in a file that may have moved. One that has is left empty, which is
    // the truth, instead of a pad that looks loaded and plays nothing.
    for (int slot = 0; slot < Sampler::numSlots; ++slot)
    {
        const juce::File file (settings.samplerFiles[(size_t) slot]);

        if (file.existsAsFile())
            engine.loadSampleAsync (slot, file);
    }
}

void MainComponent::saveSettings()
{
    auto state = settings;
    state.captureFrom (engine.getMixer(), engine.getSampler());

    for (int deck = 0; deck < AudioEngine::numDecks; ++deck)
        state.tempoRanges[(size_t) deck] = dispatcher.getTempoRange (deck);

    state.visibleDecks = visibleDecks;

    state.writeTo (SessionState::defaultFile());

    // The device is JUCE's own XML rather than anything of ours, so it is kept
    // beside the settings instead of being folded into them.
    if (const auto device = engine.getDeviceState(); device != nullptr)
        device->writeTo (SessionState::defaultFile().getSiblingFile ("audio-device.xml"));
}

void MainComponent::showDeck (int deckIndex)
{
    if (! juce::isPositiveAndBelow (deckIndex, AudioEngine::numDecks))
        return;

    const auto side = deckIndex % 2;
    const auto other = deckIndex < 2 ? deckIndex + 2 : deckIndex - 2;

    visibleDecks[(size_t) side] = deckIndex;

    for (int i = 0; i < AudioEngine::numDecks; ++i)
        deckViews[(size_t) i]->setVisible (i == visibleDecks[0] || i == visibleDecks[1]);

    // The button names the deck it would bring on, so it reads as an answer
    // rather than as a label for where you already are.
    deckViews[(size_t) deckIndex]->setSwapTarget (
        juce::String::charToString ('A' + (juce::juce_wchar) other));

    deckRow.resized();
}

void MainComponent::swapSide (int side)
{
    if (! juce::isPositiveAndBelow (side, 2))
        return;

    const auto showing = visibleDecks[(size_t) side];
    showDeck (showing < 2 ? showing + 2 : showing - 2);
}

void MainComponent::swapBothSides()
{
    swapSide (0);
    swapSide (1);
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

void MainComponent::toggleRecording()
{
    auto& recorder = engine.getRecorder();

    if (recorder.isRecording())
    {
        const auto file = recorder.stop();

        juce::String message;
        message << "Saved to " << file.getFullPathName() << "." << juce::newLine << juce::newLine
                << "The tracklist is beside it as "
                << file.withFileExtension (".txt").getFileName() << ".";

        // A recording with holes in it is worth saying out loud, now, while
        // there is still a chance to do something about the cause.
        if (recorder.hadDropouts())
            message << juce::newLine << juce::newLine
                    << "Some audio was dropped because the disk could not keep up, "
                       "so the recording has gaps. The tracklist file says how much.";

        juce::NativeMessageBox::showMessageBoxAsync (
            recorder.hadDropouts() ? juce::MessageBoxIconType::WarningIcon
                                   : juce::MessageBoxIconType::InfoIcon,
            "Recording saved", message);
        return;
    }

    juce::String error;

    if (engine.startRecording (error) == juce::File())
        juce::NativeMessageBox::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon, "Could not start recording", error);
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
    recordButton.setBounds (footer.removeFromLeft (110));
    footer.removeFromLeft (6);
    midiSettingsButton.setBounds (footer.removeFromLeft (94));
    footer.removeFromLeft (12);
    statusLabel.setBounds (footer);

    area.removeFromBottom (8);

    // A fixed row rather than a share of the window: eight pads need the same
    // height whatever else is on screen, and taking it from the browser is
    // cheaper than taking it from a waveform.
    samplerView->setBounds (area.removeFromBottom (58));
    area.removeFromBottom (8);

    juce::Component* rows[] = { &deckRow, resizerBar.get(), browser.get() };
    verticalLayout.layOutComponents (rows, 3, area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                                     true, true);
}

} // namespace opendj
