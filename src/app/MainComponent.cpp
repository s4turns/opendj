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

    for (const auto& folder : findDataFolders ("mappings"))
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

    // Before the device opens, so the routing is already right when the first
    // block is rendered and when the startup line is written.
    engine.setOutputMode (settings.outputMode);

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
        deckViews[(size_t) i]->onZoomRequested = [this] (int steps) { stepWaveformZoom (steps); };
    }

    showDeck (0);
    showDeck (1);

    mixerView = std::make_unique<MixerComponent> (engine, engine.getMixer());
    deckRow.addAndMakeVisible (*mixerView);

    // Across the top rather than inside a deck: the whole point of it is the two
    // decks against each other, which needs one axis, not one each.
    beatStrip = std::make_unique<BeatMatchComponent> (engine);
    addAndMakeVisible (*beatStrip);

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

    micView = std::make_unique<MicComponent> (engine);
    micView->onMessage = [this] (const juce::String& message)
    {
        statusLabel.setText (message, juce::dontSendNotification);
    };
    addAndMakeVisible (*micView);

    restoreSettings();

    browser = std::make_unique<BrowserComponent> (library, scanner);
    browser->onLoad = [this] (const juce::File& file, int deckIndex) { loadOntoDeck (file, deckIndex); };
    addAndMakeVisible (*browser);

    // The controller's browse encoder and load buttons reach the browser through
    // the dispatcher, the same way every other input does.
    dispatcher.selectedFileProvider = [this] (int) { return browser->getSelectedFile(); };
    dispatcher.waveformZoomHandler = [safe = juce::Component::SafePointer<MainComponent> (this)] (int steps)
    {
        juce::MessageManager::callAsync ([safe, steps]
        {
            if (safe != nullptr)
                safe->stepWaveformZoom (steps);
        });
    };

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

    streamButton.setTooltip ("Broadcast the master output to an Icecast server");
    streamButton.onClick = [this] { toggleBroadcast(); };
    addAndMakeVisible (streamButton);

    rtmpButton.setTooltip ("Broadcast to YouTube, Twitch, or anything else that takes RTMP, through ffmpeg");
    rtmpButton.onClick = [this] { toggleRtmpBroadcast(); };
    addAndMakeVisible (rtmpButton);

    visualsButton.setTooltip ("MilkDrop visuals, in a window of their own and in the RTMP broadcast");
    visualsButton.onClick = [this] { toggleVisuals(); };
    addAndMakeVisible (visualsButton);

    midiSettingsButton.onClick = [this] { showMidiSettings(); };
    addAndMakeVisible (midiSettingsButton);

    recordButton.onClick = [this] { toggleRecording(); };
    recordButton.onSecondaryClick = [this] { showRecordingSettings(); };
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
    // Settings first, while the device is still open: asking a closed device
    // what it was gets an empty answer, and the whole point is to reopen it.
    // Everything read here is an atomic or message thread state, so a running
    // audio thread does not make it unsafe.
    saveSettings();

    // Before the engine stops: the window's panel reads the visualiser on a
    // timer, and the visualiser is the engine's.
    visualsWindow.reset();

    engine.stop();

    stopTimer();
    dispatcher.onStateChanged = nullptr;
    dispatcher.selectedFileProvider = nullptr;
    dispatcher.browseScrollHandler = nullptr;
    dispatcher.waveformZoomHandler = nullptr;
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

juce::Array<juce::File> MainComponent::findDataFolders (const juce::String& name) const
{
    juce::Array<juce::File> folders;

    const auto add = [&folders] (const juce::File& folder)
    {
        if (folder.isDirectory() && ! folders.contains (folder))
            folders.add (folder);
    };

    const auto dataFolder = [&name] (const juce::File& root) { return root.getChildFile ("opendj/" + name); };

   #if JUCE_LINUX || JUCE_BSD
    // The user's own mappings come first, so one written here replaces a
    // shipped mapping of the same name instead of competing with it.
    const auto dataHome = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_HOME", {});

    add (dataFolder (dataHome.isNotEmpty()
                        ? juce::File (dataHome)
                        : juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                              .getChildFile (".local/share")));
   #endif

   #if JUCE_WINDOWS
    // The same idea as the XDG block above, in the one place a Windows user
    // can write to without being an administrator, and where the settings and
    // the library already are. "OpenDJ" rather than "opendj" so these sit
    // beside settings.json instead of in a second folder differing from it
    // only in case; scripts/get-presets.ps1 installs here.
    add (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
            .getChildFile ("OpenDJ").getChildFile (name));
   #endif

    // Beside the executable while developing, so a freshly built binary finds
    // the mappings in the source tree, and under the prefix once installed,
    // where /usr/local/bin/opendj must reach /usr/local/share/opendj/mappings.
    const auto executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

    for (auto directory = executable.getParentDirectory();
         directory.exists() && directory != directory.getParentDirectory();
         directory = directory.getParentDirectory())
    {
        add (directory.getChildFile (name));
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

    beatStrip->refresh();
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

    auto& broadcaster = engine.getBroadcaster();

    switch (broadcaster.getState())
    {
        case Broadcaster::State::live:
        {
            const auto seconds = (int) broadcaster.getSecondsLive();
            streamButton.setButtonText ("On air " + juce::String (seconds / 60) + ":"
                                        + juce::String (seconds % 60).paddedLeft ('0', 2));
            streamButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff27ae60));
            break;
        }

        case Broadcaster::State::connecting:
        case Broadcaster::State::reconnecting:
            streamButton.setButtonText ("Connecting");
            streamButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffb9770e));
            break;

        case Broadcaster::State::failed:
            streamButton.setButtonText ("Stream");
            streamButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffc0392b));
            break;

        case Broadcaster::State::offline:
        default:
            streamButton.setButtonText ("Stream");
            streamButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2c2c34));
            break;
    }

    if (const auto broadcastStatus = broadcaster.getStatusMessage(); broadcastStatus.isNotEmpty())
        status << "  |  " << broadcastStatus;

    auto& rtmpBroadcaster = engine.getRtmpBroadcaster();

    switch (rtmpBroadcaster.getState())
    {
        case RtmpBroadcaster::State::live:
        {
            const auto seconds = (int) rtmpBroadcaster.getSecondsLive();
            rtmpButton.setButtonText ("On air " + juce::String (seconds / 60) + ":"
                                      + juce::String (seconds % 60).paddedLeft ('0', 2));
            rtmpButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff27ae60));
            break;
        }

        case RtmpBroadcaster::State::connecting:
        case RtmpBroadcaster::State::reconnecting:
            rtmpButton.setButtonText ("Connecting");
            rtmpButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffb9770e));
            break;

        case RtmpBroadcaster::State::failed:
            rtmpButton.setButtonText ("RTMP");
            rtmpButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffc0392b));
            break;

        case RtmpBroadcaster::State::offline:
        default:
            rtmpButton.setButtonText ("RTMP");
            rtmpButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2c2c34));
            break;
    }

    if (const auto rtmpStatus = rtmpBroadcaster.getStatusMessage(); rtmpStatus.isNotEmpty())
        status << "  |  " << rtmpStatus;

    auto& visualizer = engine.getVisualizer();
    visualsButton.setColour (juce::TextButton::buttonColourId,
                             visualizer.isRunning() ? juce::Colour (0xff8e44ad) : juce::Colour (0xff2c2c34));

    if (const auto visualsStatus = visualizer.getStatusMessage(); visualsStatus.isNotEmpty())
        status << "  |  " << visualsStatus;

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
    settings.applyTo (engine.getMic());

    for (int deck = 0; deck < AudioEngine::numDecks; ++deck)
        dispatcher.setTempoRange (deck, settings.tempoRanges[(size_t) deck]);

    showDeck (settings.visibleDecks[0]);
    showDeck (settings.visibleDecks[1]);

    settings.waveformZoomSeconds = waveform::nearestZoom (settings.waveformZoomSeconds);
    applyWaveformZoom();

    updateRecordingTooltip();

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
    state.captureFrom (engine.getMic());
    state.outputMode = engine.getOutputMode();

    for (int deck = 0; deck < AudioEngine::numDecks; ++deck)
        state.tempoRanges[(size_t) deck] = dispatcher.getTempoRange (deck);

    state.visibleDecks = visibleDecks;
    state.waveformZoomSeconds = settings.waveformZoomSeconds;

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

    if (beatStrip != nullptr)
        beatStrip->setDecks (visibleDecks[0], visibleDecks[1]);

    deckRow.resized();
}

void MainComponent::stepWaveformZoom (int steps)
{
    const auto wanted = waveform::stepZoom (settings.waveformZoomSeconds, steps);

    if (juce::approximatelyEqual (wanted, settings.waveformZoomSeconds))
        return;

    settings.waveformZoomSeconds = wanted;
    applyWaveformZoom();
}

void MainComponent::applyWaveformZoom()
{
    for (auto& view : deckViews)
        view->setWaveformZoom (settings.waveformZoomSeconds);
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
    /** The device picker with the cue routing underneath it. Routing is not
        something JUCE's selector knows about, and it belongs here rather than
        on the mixer: it is a fact about how the hardware is plugged in, which
        is exactly what the rest of this dialog is about. */
    struct AudioSetup final : public juce::Component
    {
        AudioSetup (AudioEngine& engineToUse) : engine (engineToUse)
        {
            selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
                engine.getDeviceManager(),
                0, 2,      // a mic, from one input or a pair
                2, 8,      // enough outputs for a master pair plus a cue pair
                false,     // MIDI has its own panel
                false,
                true,
                false);

            addAndMakeVisible (*selector);

            label.setText ("Headphone cue", juce::dontSendNotification);
            label.setColour (juce::Label::textColourId, juce::Colours::white);
            addAndMakeVisible (label);

            routing.addItem ("Separate outputs: master on 1-2, cue on 3-4", 1);
            routing.addItem ("Split one stereo output: master on 1, cue on 2", 2);
            routing.setSelectedId (engine.getOutputMode() == OutputMode::splitStereo ? 2 : 1,
                                   juce::dontSendNotification);
            routing.onChange = [this]
            {
                engine.setOutputMode (routing.getSelectedId() == 2 ? OutputMode::splitStereo
                                                                   : OutputMode::separatePairs);
                updateExplanation();
            };
            addAndMakeVisible (routing);

            explanation.setColour (juce::Label::textColourId, juce::Colours::grey);
            explanation.setFont (juce::FontOptions (12.0f));
            explanation.setJustificationType (juce::Justification::topLeft);
            addAndMakeVisible (explanation);

            updateExplanation();
        }

        void updateExplanation()
        {
            const auto outputs = engine.getNumOutputChannels();
            const auto split = engine.getOutputMode() == OutputMode::splitStereo;

            juce::String text;

            if (split)
                text << "Both busses are mono. Use a splitter cable: one side to the speakers, "
                        "the other to the headphones.";
            else if (outputs >= 4)
                text << "The cue bus is on outputs 3 and 4, in stereo.";
            else
                text << "This device has " << outputs << " output"
                     << (outputs == 1 ? "" : "s") << ", so there is nowhere for a separate cue "
                        "bus to go. Split the output, or use an interface with four.";

            explanation.setText (text, juce::dontSendNotification);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (8);

            auto footer = area.removeFromBottom (96);
            label.setBounds (footer.removeFromTop (20));
            routing.setBounds (footer.removeFromTop (24));
            footer.removeFromTop (6);
            explanation.setBounds (footer);

            area.removeFromBottom (8);
            selector->setBounds (area);
        }

        AudioEngine& engine;
        std::unique_ptr<juce::AudioDeviceSelectorComponent> selector;
        juce::Label label, explanation;
        juce::ComboBox routing;
    };

    auto content = std::make_unique<AudioSetup> (engine);
    content->setSize (520, 540);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (content.release());
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

    if (engine.startRecording (settings.recording, error) == juce::File())
        juce::NativeMessageBox::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon, "Could not start recording", error);
}

void MainComponent::updateRecordingTooltip()
{
    const auto& recording = settings.recording;

    auto format = SetRecorder::nameFor (recording.format);

    if (recording.format == RecordingFormat::mp3)
        format << (recording.mp3Bitrate > 0
                       ? " at " + juce::String (recording.mp3Bitrate) + " kbps"
                       : " at V0");

    recordButton.setTooltip ("Record the master output to " + format
                             + ", with a tracklist beside it."
                               "  Right-click for the format and the folder.");
}

void MainComponent::showRecordingSettings()
{
    // Zero is V0, which is variable. 320 leads because a DJ who picks MP3 at
    // all usually means the one everybody recognises. Out here rather than
    // inside the panel because a class declared inside a function cannot have
    // static members of its own.
    static constexpr int bitrates[] = { 320, 256, 192, 0 };

    /** Format, bitrate and folder. No start button, unlike the broadcast
        panel: the record button is right there behind this dialog and already
        does that job. */
    struct RecordingSetup final : public juce::Component
    {
        RecordingSetup (RecordingSettings starting,
                        std::function<void (RecordingSettings)> onChanged)
            : settings (std::move (starting)), changed (std::move (onChanged))
        {
            addLabel (formatLabel, "Format");
            format.addItem ("WAV, 24-bit", 1 + (int) RecordingFormat::wav);
            format.addItem ("FLAC, 24-bit", 1 + (int) RecordingFormat::flac);
            format.addItem ("MP3", 1 + (int) RecordingFormat::mp3);
            format.setSelectedId (1 + (int) settings.format, juce::dontSendNotification);
            format.onChange = [this] { refreshEnablement(); };
            addAndMakeVisible (format);

            addLabel (bitrateLabel, "Bitrate");

            for (int i = 0; i < (int) std::size (bitrates); ++i)
                bitrate.addItem (bitrates[i] > 0 ? juce::String (bitrates[i]) + " kbps"
                                                 : "V0, variable, about 245 kbps",
                                 i + 1);

            bitrate.setSelectedId (1 + indexOfBitrate (settings.mp3Bitrate),
                                   juce::dontSendNotification);
            addAndMakeVisible (bitrate);

            addLabel (folderLabel, "Folder");
            folder.setText (settings.folder, juce::dontSendNotification);
            folder.setTextToShowWhenEmpty (SetRecorder::defaultFolder().getFullPathName(),
                                           juce::Colours::grey);
            folder.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff101014));
            addAndMakeVisible (folder);

            browse.setButtonText ("Browse...");
            browse.onClick = [this] { chooseFolder(); };
            addAndMakeVisible (browse);

            note.setText ("WAV and FLAC both keep every bit of what the room heard, FLAC in "
                          "about half the space. MP3 is for sending somebody the set: what it "
                          "throws away is gone, so it is a poor thing to remaster from.",
                          juce::dontSendNotification);
            note.setColour (juce::Label::textColourId, juce::Colours::grey);
            note.setFont (juce::FontOptions (12.0f));
            note.setJustificationType (juce::Justification::topLeft);
            addAndMakeVisible (note);

            save.setButtonText ("Save");
            save.onClick = [this] { collectAndClose(); };
            addAndMakeVisible (save);

            refreshEnablement();
        }

        static int indexOfBitrate (int kbps)
        {
            for (int i = 0; i < (int) std::size (bitrates); ++i)
                if (bitrates[i] == kbps)
                    return i;

            return 0;
        }

        void addLabel (juce::Label& label, const juce::String& text)
        {
            label.setText (text, juce::dontSendNotification);
            label.setColour (juce::Label::textColourId, juce::Colours::white);
            label.setFont (juce::FontOptions (12.0f));
            addAndMakeVisible (label);
        }

        RecordingFormat chosenFormat() const
        {
            return (RecordingFormat) juce::jmax (0, format.getSelectedId() - 1);
        }

        /** The bitrate only means anything for MP3, so it says so by going
            grey rather than by sitting there inviting a change that does
            nothing. */
        void refreshEnablement()
        {
            const auto isMp3 = chosenFormat() == RecordingFormat::mp3;
            bitrate.setEnabled (isMp3);
            bitrateLabel.setEnabled (isMp3);
        }

        void chooseFolder()
        {
            const juce::File start (settings.folder.isNotEmpty()
                                        ? juce::File (settings.folder)
                                        : SetRecorder::defaultFolder());

            chooser = std::make_unique<juce::FileChooser> ("Where recordings go", start);

            chooser->launchAsync (juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectDirectories,
                                  [this] (const juce::FileChooser& fc)
                                  {
                                      if (const auto picked = fc.getResult();
                                          picked != juce::File())
                                      {
                                          folder.setText (picked.getFullPathName(),
                                                          juce::dontSendNotification);
                                      }
                                  });
        }

        void collectAndClose()
        {
            settings.format = chosenFormat();
            settings.mp3Bitrate = bitrates[juce::jlimit (0, (int) std::size (bitrates) - 1,
                                                         bitrate.getSelectedId() - 1)];
            settings.folder = folder.getText().trim();

            changed (settings);

            if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
                window->exitModalState (0);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (10);

            auto row = area.removeFromTop (26);
            formatLabel.setBounds (row.removeFromLeft (70));
            format.setBounds (row);
            area.removeFromTop (4);

            row = area.removeFromTop (26);
            bitrateLabel.setBounds (row.removeFromLeft (70));
            bitrate.setBounds (row);
            area.removeFromTop (4);

            row = area.removeFromTop (26);
            folderLabel.setBounds (row.removeFromLeft (70));
            browse.setBounds (row.removeFromRight (84));
            row.removeFromRight (6);
            folder.setBounds (row);

            area.removeFromTop (10);
            note.setBounds (area.removeFromTop (64));

            save.setBounds (area.removeFromBottom (28).removeFromRight (110));
        }

        RecordingSettings settings;
        std::function<void (RecordingSettings)> changed;

        juce::Label formatLabel, bitrateLabel, folderLabel, note;
        juce::ComboBox format, bitrate;
        juce::TextEditor folder;
        juce::TextButton browse, save;
        std::unique_ptr<juce::FileChooser> chooser;
    };

    auto content = std::make_unique<RecordingSetup> (settings.recording,
                                                     [this] (RecordingSettings updated)
                                                     {
                                                         settings.recording = std::move (updated);
                                                         updateRecordingTooltip();
                                                     });
    content->setSize (460, 230);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (content.release());
    options.dialogTitle = "Recording";
    options.dialogBackgroundColour = juce::Colour (0xff1c1c22);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void MainComponent::toggleBroadcast()
{
    auto& broadcaster = engine.getBroadcaster();

    if (broadcaster.isBroadcasting())
    {
        broadcaster.stop();
        statusLabel.setText ("Broadcast stopped.", juce::dontSendNotification);
        return;
    }

    showBroadcastSettings();
}

void MainComponent::showBroadcastSettings()
{
    /** The server details, and a button that goes live with them. One panel
        rather than a settings dialog and a separate switch: nobody fills a
        server in and then wants to be asked where it was. */
    struct BroadcastSetup final : public juce::Component
    {
        BroadcastSetup (AudioEngine& engineToUse, BroadcastSettings starting,
                        std::function<void (BroadcastSettings)> onChanged)
            : engine (engineToUse), settings (std::move (starting)), changed (std::move (onChanged))
        {
            addField (host, "Server", settings.host);
            addField (port, "Port", juce::String (settings.port));
            addField (mount, "Mount", settings.mount);
            addField (user, "User", settings.user);
            addField (password, "Password", settings.password);
            password.setPasswordCharacter ((juce::juce_wchar) 0x2022);
            addField (name, "Stream name", settings.name);
            addField (genre, "Genre", settings.genre);

            listed.setButtonText ("List this stream in public directories");
            listed.setToggleState (settings.isPublic, juce::dontSendNotification);
            listed.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
            addAndMakeVisible (listed);

            note.setText ("The password is kept in clear text in your settings file, as every "
                          "DJ application does. Anything that can read your user profile can "
                          "read it.", juce::dontSendNotification);
            note.setColour (juce::Label::textColourId, juce::Colours::grey);
            note.setFont (juce::FontOptions (12.0f));
            note.setJustificationType (juce::Justification::topLeft);
            addAndMakeVisible (note);

            status.setColour (juce::Label::textColourId, juce::Colours::white);
            status.setFont (juce::FontOptions (12.0f));
            addAndMakeVisible (status);

            goLive.setButtonText ("Go live");
            goLive.onClick = [this] { connect(); };
            addAndMakeVisible (goLive);
        }

        void addField (juce::TextEditor& editor, const juce::String& labelText,
                       const juce::String& value)
        {
            auto* label = labels.add (new juce::Label());
            label->setText (labelText, juce::dontSendNotification);
            label->setColour (juce::Label::textColourId, juce::Colours::white);
            label->setFont (juce::FontOptions (12.0f));
            addAndMakeVisible (*label);

            editor.setText (value, juce::dontSendNotification);
            editor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff101014));
            addAndMakeVisible (editor);
        }

        BroadcastSettings collect() const
        {
            auto result = settings;
            result.host = host.getText().trim();
            result.port = port.getText().getIntValue();
            result.mount = mount.getText().trim();
            result.user = user.getText().trim();
            result.password = password.getText();
            result.name = name.getText();
            result.genre = genre.getText();
            result.isPublic = listed.getToggleState();
            return result;
        }

        void connect()
        {
            settings = collect();
            changed (settings);

            auto* device = engine.getDeviceManager().getCurrentAudioDevice();
            const auto rate = device != nullptr ? device->getCurrentSampleRate() : 48000.0;

            juce::String error;

            if (engine.getBroadcaster().start (settings, rate, error))
            {
                if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
                    window->exitModalState (0);
            }
            else
            {
                status.setText (error, juce::dontSendNotification);
            }
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (10);

            juce::TextEditor* editors[] = { &host, &port, &mount, &user, &password, &name, &genre };

            for (int i = 0; i < (int) std::size (editors); ++i)
            {
                auto row = area.removeFromTop (26);
                labels[i]->setBounds (row.removeFromLeft (96));
                editors[i]->setBounds (row);
                area.removeFromTop (4);
            }

            area.removeFromTop (4);
            listed.setBounds (area.removeFromTop (24));
            area.removeFromTop (6);
            note.setBounds (area.removeFromTop (52));

            auto footer = area.removeFromBottom (28);
            goLive.setBounds (footer.removeFromRight (110));
            footer.removeFromRight (8);
            status.setBounds (footer);
        }

        AudioEngine& engine;
        BroadcastSettings settings;
        std::function<void (BroadcastSettings)> changed;

        juce::OwnedArray<juce::Label> labels;
        juce::TextEditor host, port, mount, user, password, name, genre;
        juce::ToggleButton listed;
        juce::Label note, status;
        juce::TextButton goLive;
    };

    auto content = std::make_unique<BroadcastSetup> (engine, settings.broadcast,
                                                     [this] (BroadcastSettings updated)
                                                     {
                                                         settings.broadcast = std::move (updated);
                                                     });
    content->setSize (460, 400);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (content.release());
    options.dialogTitle = "Broadcast";
    options.dialogBackgroundColour = juce::Colour (0xff1c1c22);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void MainComponent::toggleRtmpBroadcast()
{
    auto& rtmpBroadcaster = engine.getRtmpBroadcaster();

    if (rtmpBroadcaster.isBroadcasting())
    {
        rtmpBroadcaster.stop();
        statusLabel.setText ("RTMP broadcast stopped.", juce::dontSendNotification);
        return;
    }

    showRtmpBroadcastSettings();
}

void MainComponent::showRtmpBroadcastSettings()
{
    /** The same "fill it in, press Go live" panel `BroadcastSetup` is, with
        an RTMP target's much smaller set of fields: a server, a stream key,
        and the title that ends up on the static video frame. See the RTMP
        design note in ROADMAP.md for why there is no more to it than that. */
    struct RtmpSetup final : public juce::Component
    {
        RtmpSetup (AudioEngine& engineToUse, RtmpSettings starting,
                   std::function<void (RtmpSettings)> onChanged)
            : engine (engineToUse), settings (std::move (starting)), changed (std::move (onChanged))
        {
            addField (server, "Server", settings.server);
            addField (streamKey, "Stream key", settings.streamKey);
            streamKey.setPasswordCharacter ((juce::juce_wchar) 0x2022);
            addField (streamTitle, "Title", settings.streamTitle);

            // The sizes and rates YouTube's and Twitch's own guidelines list,
            // and nothing in between: a person picking from a menu wants the
            // handful that platforms actually recommend, not a number to
            // type and get wrong.
            addChoice (resolution, "Resolution",
                       { "640 x 360", "854 x 480", "1280 x 720", "1920 x 1080" },
                       juce::String (settings.videoWidth) + " x " + juce::String (settings.videoHeight));
            addChoice (bitrate, "Video bitrate",
                       { "1000 kbps", "1500 kbps", "2500 kbps", "4500 kbps", "6000 kbps" },
                       juce::String (settings.videoBitrateKbps) + " kbps");

            // 24 for anyone matching video footage, 25 for PAL, 30 and 60 as
            // the two both platforms list. 60 costs the visualiser twice the
            // GPU work and the encoder twice the frames, which is worth
            // saying here rather than in a manual nobody opens.
            addChoice (fps, "Frame rate",
                       { "24 fps", "25 fps", "30 fps", "60 fps" },
                       juce::String (settings.fps) + " fps");
            fps.setTooltip ("60 asks about twice as much of the GPU and the encoder as 30 does.");

            note.setText ("The stream key is kept in clear text in your settings file, the same way "
                          "the Icecast password is. With the visuals running, the picture is what "
                          "they draw; otherwise it is a still frame with this title on it.",
                          juce::dontSendNotification);
            note.setColour (juce::Label::textColourId, juce::Colours::grey);
            note.setFont (juce::FontOptions (12.0f));
            note.setJustificationType (juce::Justification::topLeft);
            addAndMakeVisible (note);

            status.setColour (juce::Label::textColourId, juce::Colours::white);
            status.setFont (juce::FontOptions (12.0f));
            addAndMakeVisible (status);

            goLive.setButtonText ("Go live");
            goLive.onClick = [this] { connect(); };
            addAndMakeVisible (goLive);
        }

        juce::Label* addLabel (const juce::String& text)
        {
            auto* label = labels.add (new juce::Label());
            label->setText (text, juce::dontSendNotification);
            label->setColour (juce::Label::textColourId, juce::Colours::white);
            label->setFont (juce::FontOptions (12.0f));
            addAndMakeVisible (*label);
            return label;
        }

        void addField (juce::TextEditor& editor, const juce::String& labelText, const juce::String& value)
        {
            addLabel (labelText);
            editor.setText (value, juce::dontSendNotification);
            editor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff101014));
            addAndMakeVisible (editor);
            rows.add (&editor);
        }

        void addChoice (juce::ComboBox& box, const juce::String& labelText,
                        const juce::StringArray& choices, const juce::String& current)
        {
            addLabel (labelText);
            box.addItemList (choices, 1);

            // A saved value that is not on the list, from a hand-edited
            // settings file or an older build, is kept rather than snapped
            // to the nearest: it is shown as its own entry so what is saved
            // and what is on screen agree.
            if (const auto index = choices.indexOf (current); index >= 0)
                box.setSelectedItemIndex (index, juce::dontSendNotification);
            else
            {
                box.addItem (current, choices.size() + 1);
                box.setSelectedItemIndex (choices.size(), juce::dontSendNotification);
            }

            addAndMakeVisible (box);
            rows.add (&box);
        }

        RtmpSettings collect() const
        {
            auto result = settings;
            result.server = server.getText().trim();
            result.streamKey = streamKey.getText().trim();
            result.streamTitle = streamTitle.getText();

            // "1280 x 720" and "2500 kbps" read back as their numbers. The
            // text is the source of truth so a kept-as-is custom entry
            // (see addChoice) round-trips exactly.
            const auto size = juce::StringArray::fromTokens (resolution.getText(), "x", "");

            if (size.size() == 2)
            {
                result.videoWidth = juce::jmax (160, size[0].trim().getIntValue());
                result.videoHeight = juce::jmax (90, size[1].trim().getIntValue());
            }

            result.videoBitrateKbps = juce::jmax (200, bitrate.getText().getIntValue());
            result.fps = juce::jlimit (1, 60, fps.getText().getIntValue());
            return result;
        }

        void connect()
        {
            settings = collect();
            changed (settings);

            auto* device = engine.getDeviceManager().getCurrentAudioDevice();
            const auto rate = device != nullptr ? device->getCurrentSampleRate() : 48000.0;

            // The picture is the visuals if they are on, and the still card
            // if not. Decided here, at the moment of going live, from what
            // is actually running rather than from a saved preference that
            // could be stale.
            settings.liveVideo = engine.getVisualizer().isRunning();

            juce::String error;

            if (engine.getRtmpBroadcaster().start (settings, rate, error))
            {
                if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
                    window->exitModalState (0);
            }
            else
            {
                status.setText (error, juce::dontSendNotification);
            }
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (10);

            for (int i = 0; i < rows.size(); ++i)
            {
                auto row = area.removeFromTop (26);
                labels[i]->setBounds (row.removeFromLeft (96));
                rows[i]->setBounds (row);
                area.removeFromTop (4);
            }

            area.removeFromTop (6);
            note.setBounds (area.removeFromTop (60));

            auto footer = area.removeFromBottom (28);
            goLive.setBounds (footer.removeFromRight (110));
            footer.removeFromRight (8);
            status.setBounds (footer);
        }

        AudioEngine& engine;
        RtmpSettings settings;
        std::function<void (RtmpSettings)> changed;

        juce::OwnedArray<juce::Label> labels;
        juce::Array<juce::Component*> rows;
        juce::TextEditor server, streamKey, streamTitle;
        juce::ComboBox resolution, bitrate, fps;
        juce::Label note, status;
        juce::TextButton goLive;
    };

    auto content = std::make_unique<RtmpSetup> (engine, settings.rtmp,
                                                [this] (RtmpSettings updated)
                                                {
                                                    settings.rtmp = std::move (updated);
                                                    applyVisualSettings();
                                                });
    content->setSize (460, 300);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (content.release());
    options.dialogTitle = "RTMP broadcast";
    options.dialogBackgroundColour = juce::Colour (0xff1c1c22);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

/** Holds the visuals panel. Closing it hides it rather than destroying it:
    the visuals may be on their way to a broadcast, and a window someone
    closed to get it out of the way is not a request to take the stream's
    picture down. The Visuals button is what stops them. */
class MainComponent::VisualsWindow final : public juce::DocumentWindow
{
public:
    explicit VisualsWindow (Visualizer& visualizer)
        : juce::DocumentWindow ("OpenDJ visuals", juce::Colours::black,
                                juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new VisualizerComponent (visualizer), false);
        setResizable (true, false);
        setResizeLimits (320, 180, 7680, 4320);
        centreWithSize (960, 540);
    }

    void closeButtonPressed() override { setVisible (false); }
};

juce::Array<juce::File> MainComponent::findPresetFolders() const
{
    auto folders = findDataFolders ("presets");

   #if JUCE_LINUX || JUCE_BSD
    // Where a distribution's own projectM preset pack goes, so one installed
    // from the package manager is found without being copied anywhere.
    auto dataDirs = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_DIRS", {});

    if (dataDirs.isEmpty())
        dataDirs = "/usr/local/share:/usr/share";

    for (const auto& directory : juce::StringArray::fromTokens (dataDirs, ":", {}))
        if (const auto folder = juce::File (directory).getChildFile ("projectM/presets");
            directory.isNotEmpty() && folder.isDirectory() && ! folders.contains (folder))
            folders.add (folder);
   #endif

    return folders;
}

juce::Array<juce::File> MainComponent::findTextureFolders() const
{
    auto folders = findDataFolders ("textures");

    // Every pack's own directory too, since the installer puts the images in
    // one below "textures" rather than loose in it.
    for (const auto& parent : findDataFolders ("textures"))
        for (const auto& child : parent.findChildFiles (juce::File::findDirectories, false))
            if (! folders.contains (child))
                folders.add (child);

   #if JUCE_LINUX || JUCE_BSD
    auto dataDirs = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_DIRS", {});

    if (dataDirs.isEmpty())
        dataDirs = "/usr/local/share:/usr/share";

    for (const auto& directory : juce::StringArray::fromTokens (dataDirs, ":", {}))
        if (const auto folder = juce::File (directory).getChildFile ("projectM/textures");
            directory.isNotEmpty() && folder.isDirectory() && ! folders.contains (folder))
            folders.add (folder);
   #endif

    return folders;
}

void MainComponent::toggleVisuals()
{
    auto& visualizer = engine.getVisualizer();

    if (visualizer.isRunning())
    {
        visualizer.stop();
        visualsWindow.reset();
        statusLabel.setText ("Visuals off.", juce::dontSendNotification);
        return;
    }

    juce::String error;

    if (! visualizer.start (visualSettingsForBroadcast(), error))
    {
        statusLabel.setText (error, juce::dontSendNotification);
        return;
    }

    showVisualsWindow();
}

VisualizerSettings MainComponent::visualSettingsForBroadcast() const
{
    // Drawn at the broadcast's size and rate, whether or not one is running,
    // so that starting one later finds frames already the right shape and
    // the window shows what a viewer would get.
    VisualizerSettings visualSettings;
    visualSettings.width = settings.rtmp.videoWidth;
    visualSettings.height = settings.rtmp.videoHeight;
    visualSettings.fps = settings.rtmp.fps;
    visualSettings.presetFolders = findPresetFolders();
    visualSettings.textureFolders = findTextureFolders();
    return visualSettings;
}

void MainComponent::applyVisualSettings()
{
    auto& visualizer = engine.getVisualizer();

    if (! visualizer.isRunning())
        return;

    if (visualizer.getFrameWidth() == settings.rtmp.videoWidth
        && visualizer.getFrameHeight() == settings.rtmp.videoHeight)
        return;

    // The broadcast's size changed under running visuals. Frames of the old
    // size would be refused by the feeder, and the stream would show the
    // black it started with for as long as they kept coming, so the visuals
    // are restarted at the new size, which costs a moment's black in the
    // window and nothing else.
    juce::String error;

    if (! visualizer.start (visualSettingsForBroadcast(), error))
        statusLabel.setText (error, juce::dontSendNotification);
}

void MainComponent::showVisualsWindow()
{
    if (visualsWindow == nullptr)
        visualsWindow = std::make_unique<VisualsWindow> (engine.getVisualizer());

    visualsWindow->setVisible (true);
    visualsWindow->toFront (true);
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

    // A fixed row rather than a share of the window, the same bargain the
    // sampler row makes: eight beats need the same height whatever else is on
    // screen, and it is cheaper taken from the browser than from a waveform.
    beatStrip->setBounds (area.removeFromTop (36));
    area.removeFromTop (8);

    auto footer = area.removeFromBottom (26);
    audioSettingsButton.setBounds (footer.removeFromLeft (100));
    footer.removeFromLeft (6);
    recordButton.setBounds (footer.removeFromLeft (110));
    footer.removeFromLeft (6);
    streamButton.setBounds (footer.removeFromLeft (110));
    footer.removeFromLeft (6);
    rtmpButton.setBounds (footer.removeFromLeft (110));
    footer.removeFromLeft (6);
    visualsButton.setBounds (footer.removeFromLeft (80));
    footer.removeFromLeft (6);
    midiSettingsButton.setBounds (footer.removeFromLeft (94));
    footer.removeFromLeft (12);
    statusLabel.setBounds (footer);

    area.removeFromBottom (8);

    // A fixed row rather than a share of the window: eight pads need the same
    // height whatever else is on screen, and taking it from the browser is
    // cheaper than taking it from a waveform.
    auto samplerRow = area.removeFromBottom (58);
    micView->setBounds (samplerRow.removeFromRight (300));
    samplerRow.removeFromRight (8);
    samplerView->setBounds (samplerRow);
    area.removeFromBottom (8);

    juce::Component* rows[] = { &deckRow, resizerBar.get(), browser.get() };
    verticalLayout.layOutComponents (rows, 3, area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                                     true, true);
}

} // namespace opendj
