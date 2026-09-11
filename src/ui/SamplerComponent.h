/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/AudioEngine.h"

#include <array>
#include <memory>

namespace opendj

{

/** A row of eight pads with a level beside them.

    Loading is where a sampler is either usable or not, so there are three ways
    in: click an empty pad, drop a file on any pad, or take Load from the pad's
    own menu. A pad with a sound on it triggers instead of asking, because that
    is the whole point of it while a set is running.
*/
class SamplerComponent final : public juce::Component,
                               public juce::FileDragAndDropTarget,
                               private juce::Timer
{
public:
    explicit SamplerComponent (AudioEngine& engineToUse);
    ~SamplerComponent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    /** Reported so the shell can put it in the status bar, which is the one
        place a message is seen without a dialog interrupting a mix. */
    std::function<void (const juce::String&)> onMessage;

private:
    /** A pad draws its own state, since a plain button cannot show a name, a
        loop marker and a playing highlight at once. */
    struct Pad final : public juce::Component
    {
        std::function<void (const juce::MouseEvent&)> onClick;

        void mouseDown (const juce::MouseEvent& e) override { if (onClick != nullptr) onClick (e); }
        void paint (juce::Graphics& g) override;

        juce::String name;
        int number = 1;
        bool loaded = false;
        bool playing = false;
        bool looping = false;
    };

    void timerCallback() override;

    void padClicked (int slot, const juce::MouseEvent& e);
    void showPadMenu (int slot);
    void chooseFileFor (int slot);
    void loadInto (int slot, const juce::File& file);

    /** Which pad a point is over, or -1. Used by the file drop. */
    int padAt (juce::Point<int> position) const;

    AudioEngine& engine;

    juce::Label heading;
    std::array<Pad, Sampler::numSlots> pads;
    juce::Slider level;
    juce::TextButton cueButton { "Cue" };

    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerComponent)
};

} // namespace opendj
