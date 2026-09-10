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

#include "core/AudioEngine.h"
#include "ui/DeckComponent.h"
#include "ui/MixerComponent.h"

#include <array>
#include <memory>

namespace opendj
{

/** The application shell: two decks either side of the mixer, a status bar, and
    a button that opens the audio device settings.

    Keyboard control is deliberately wired here rather than inside the deck
    views, because it becomes one of several inputs into the action dispatcher
    once MIDI arrives, and all of them should meet in the same place.
*/
class MainComponent final : public juce::Component,
                            private juce::Timer,
                            private juce::KeyListener,
                            private juce::FileDragAndDropTarget
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    bool keyPressed (const juce::KeyPress& key, juce::Component* origin) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    void showAudioSettings();

    AudioEngine engine;

    std::array<std::unique_ptr<DeckComponent>, AudioEngine::numDecks> deckViews;
    std::unique_ptr<MixerComponent> mixerView;

    juce::TextButton settingsButton { "Audio setup" };
    juce::Label statusLabel;
    juce::String startupError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace opendj
