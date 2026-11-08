/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/AudioEngine.h"

#include <functional>

namespace opendj
{

/** A strip for one of the device's inputs, at the end of the sampler row:
    on, level, a meter, which pair of inputs it reads, and whether the
    speakers hear it. The mic's strip also has talkover; the aux strip, for a
    stereo line such as browser audio, does not.

    On is the one control here that matters in the middle of a set, so it turns
    red while the input is live. Everything reads the engine back on a timer,
    because a controller can change it too.
*/
class MicComponent final : public juce::Component,
                           private juce::Timer
{
public:
    MicComponent (AudioEngine& engineToUse, MicInput& inputToShow, const juce::String& title);

    /** The width the strip needs to show all of its controls. */
    int getPreferredWidth() const noexcept;
    ~MicComponent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    /** For the status bar, as the sampler's is. */
    std::function<void (const juce::String&)> onMessage;

private:
    /** A level that holds its peak for a moment and then falls, so a voice
        reads as a level rather than a flicker. */
    struct Meter final : public juce::Component
    {
        void paint (juce::Graphics& g) override;

        float level = 0.0f;
    };

    void timerCallback() override;

    /** Brings every control into line with the engine without firing it. */
    void updateControls();

    /** Steps on to the next pair the device offers, round to the first. */
    void nextInputPair();

    AudioEngine& engine;
    MicInput& input;
    const juce::String name;
    const bool hasTalkover;

    juce::Label heading;
    juce::TextButton onButton { "On" };
    juce::Slider level;
    juce::Label levelLabel;
    Meter meter;
    juce::TextButton inputButton;
    juce::TextButton talkoverButton { "Talkover" };
    juce::TextButton routingButton { "Everywhere" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MicComponent)
};

} // namespace opendj
