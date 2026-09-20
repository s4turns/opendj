/*
    This file is part of OpenDJ.
    Copyright (C) 2026 The OpenDJ contributors.

    OpenDJ is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. See the LICENSE file at the root of this repository.
*/

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "control/ActionDispatcher.h"
#include "control/MidiControlSurface.h"
#include "core/AudioEngine.h"
#include "library/Library.h"
#include "library/LibraryScanner.h"
#include "ui/BrowserComponent.h"
#include "ui/DeckComponent.h"
#include "ui/MixerComponent.h"
#include "app/Settings.h"
#include "ui/SamplerComponent.h"
#include "ui/MicComponent.h"
#include "ui/VisualizerComponent.h"

#include <array>
#include <memory>

namespace opendj
{

/** The application shell: two decks either side of the mixer, the browser
    underneath, a status bar, and buttons for the audio and controller setup.

    Keyboard control is wired here rather than inside the deck views, because it
    is one of several inputs into the action dispatcher and they should all meet
    in the same place.
*/
class MainComponent final : public juce::Component,
                            public juce::DragAndDropContainer,
                            private juce::Timer,
                            private juce::KeyListener,
                            private juce::FileDragAndDropTarget
{
public:
    MainComponent();
    ~MainComponent() override;

    /** Loads files onto successive decks. Used by the command line, so a build
        can be started with two tracks already on the decks. */
    void loadInitialTracks (const juce::StringArray& paths);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    bool keyPressed (const juce::KeyPress& key, juce::Component* origin) override;
    bool keyStateChanged (bool isKeyDown, juce::Component* origin) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    /** Puts a deck on screen in place of the one it shares a side with, and
        tells both of them which letter their swap button should carry. */
    void showDeck (int deckIndex);

    /** Moves every deck's scrolling waveform a number of rungs up or down the
        zoom ladder, positive to zoom in. One setting for all of them: two
        waveforms side by side are there to be compared, and two scales make
        that harder rather than easier. */
    void stepWaveformZoom (int steps);
    void applyWaveformZoom();

    /** Swaps one side for the deck behind it. */
    void swapSide (int side);

    /** Swaps both sides at once: A and B out, C and D in, or back again. A
        controller with a single deck-toggle button means this, and so does a
        DJ who says "the other pair". */
    void swapBothSides();

    /** Puts last session's settings back, and files them away again on the way
        out. Everything here is best effort: a settings file that cannot be
        read or written costs the user their preferences, never their set. */
    void restoreSettings();
    void saveSettings();

    void showAudioSettings();
    void showMidiSettings();
    void toggleRecording();

    /** Opens the broadcast dialog, or stops a broadcast already running. */
    void toggleBroadcast();
    void showBroadcastSettings();

    /** Same pair, for the RTMP target: YouTube, Twitch, or anything else that
        takes the same protocol. */
    void toggleRtmpBroadcast();
    void showRtmpBroadcastSettings();

    /** Starts the visuals and opens their window, or stops them. Closing
        the window on its own leaves them running, since a broadcast may be
        carrying them. */
    void toggleVisuals();
    void showVisualsWindow();

    /** Restarts running visuals at the broadcast's size if that changed. */
    void applyVisualSettings();
    VisualizerSettings visualSettingsForBroadcast() const;
    juce::Array<juce::File> findPresetFolders() const;

    /** Where the images the presets load live. */
    juce::Array<juce::File> findTextureFolders() const;

    void loadOntoDeck (const juce::File& file, int deckIndex);
    /** Every folder that may hold a kind of data file -- "mappings", or
        "presets" for the visualiser -- in precedence order: the user's own
        under their data home, then beside the executable while developing,
        then under the prefix once installed. */
    juce::Array<juce::File> findDataFolders (const juce::String& name) const;

    // Declared before the engine so they outlive it: its loader threads use
    // the library as their analysis cache right up until they are joined.
    Library library;
    LibraryScanner scanner { library };
    juce::String libraryError;

    AudioEngine engine;
    ActionDispatcher dispatcher { engine };
    MidiControlSurface midi { engine, dispatcher };

    /** The decks and mixer as one component, so the shell can split the window
        between them and the browser with a draggable bar. */
    struct DeckRow final : public juce::Component
    {
        std::function<void (juce::Rectangle<int>)> onResized;
        void resized() override { if (onResized != nullptr) onResized (getLocalBounds()); }
    };

    DeckRow deckRow;
    std::array<std::unique_ptr<DeckComponent>, AudioEngine::numDecks> deckViews;

    /** Which deck is on screen on each side. A and B to start with, which is
        where somebody who never presses the swap button stays. */
    std::array<int, 2> visibleDecks { 0, 1 };
    std::unique_ptr<MixerComponent> mixerView;
    std::unique_ptr<BrowserComponent> browser;
    std::unique_ptr<SamplerComponent> samplerView;
    std::unique_ptr<MicComponent> micView;

    juce::StretchableLayoutManager verticalLayout;
    std::unique_ptr<juce::StretchableLayoutResizerBar> resizerBar;

    juce::TextButton audioSettingsButton { "Audio setup" };
    juce::TextButton midiSettingsButton { "Controller" };
    juce::TextButton recordButton { "Record" };
    juce::TextButton streamButton { "Stream" };
    juce::TextButton rtmpButton { "RTMP" };
    juce::TextButton visualsButton { "Visuals" };

    /** A window of its own rather than a panel in this one, because a
        projector on a second screen is where the visuals belong, and the
        main window's layout is spoken for. */
    class VisualsWindow;
    std::unique_ptr<VisualsWindow> visualsWindow;
    juce::Label statusLabel;
    juce::String startupError;

    /** Read before the device is opened, written when the window closes. */
    SessionState settings;

    // Cue keys are momentary, so their press and release have to be paired up.
    // The key code is kept rather than a flag, because a deck can be swapped
    // out from under a held key and still has to be released by the right one.
    std::array<int, AudioEngine::numDecks> cueKeyHeld {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace opendj
