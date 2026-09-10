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

    void showAudioSettings();
    void showMidiSettings();
    void loadOntoDeck (const juce::File& file, int deckIndex);
    /** Every folder that may hold controller mappings, in precedence order. */
    juce::Array<juce::File> findMappingsFolders() const;

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
    std::unique_ptr<MixerComponent> mixerView;
    std::unique_ptr<BrowserComponent> browser;

    juce::StretchableLayoutManager verticalLayout;
    std::unique_ptr<juce::StretchableLayoutResizerBar> resizerBar;

    juce::TextButton audioSettingsButton { "Audio setup" };
    juce::TextButton midiSettingsButton { "Controller" };
    juce::Label statusLabel;
    juce::String startupError;

    // Cue keys are momentary, so their press and release have to be paired up.
    std::array<bool, AudioEngine::numDecks> cueKeyHeld { false, false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace opendj
