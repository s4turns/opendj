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
#include "ui/DeckComponent.h"
#include "ui/MixerComponent.h"

#include <array>
#include <memory>

namespace opendj
{

/** The application shell: two decks either side of the mixer, a status bar, and
    buttons for the audio and controller setup.

    Keyboard control is wired here rather than inside the deck views, because it
    is one of several inputs into the action dispatcher and they should all meet
    in the same place.
*/
class MainComponent final : public juce::Component,
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
    juce::File findMappingsFolder() const;

    AudioEngine engine;
    ActionDispatcher dispatcher { engine };
    MidiControlSurface midi { engine, dispatcher };

    std::array<std::unique_ptr<DeckComponent>, AudioEngine::numDecks> deckViews;
    std::unique_ptr<MixerComponent> mixerView;

    juce::TextButton audioSettingsButton { "Audio setup" };
    juce::TextButton midiSettingsButton { "Controller" };
    juce::Label statusLabel;
    juce::String startupError;

    // Cue keys are momentary, so their press and release have to be paired up.
    std::array<bool, AudioEngine::numDecks> cueKeyHeld { false, false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace opendj
