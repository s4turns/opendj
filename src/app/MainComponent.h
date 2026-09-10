/*
    This file is part of OpenDJ.
    Copyright (C) 2026 The OpenDJ contributors.

    OpenDJ is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. See the LICENSE file at the root of this repository.
*/

#pragma once

#include <JuceHeader.h>

namespace opendj
{

/** Top level window content.

    For now this is the audio setup shell: it owns the device manager, exposes
    JUCE's device selector, and can emit a sine tone so a new install can be
    verified end to end before any deck code exists. The deck and mixer views
    replace the placeholder area as those land.
*/
class MainComponent final : public juce::AudioAppComponent
{
public:
    MainComponent();
    ~MainComponent() override;

    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void updateDeviceStatusText();

    juce::AudioDeviceSelectorComponent deviceSelector;
    juce::TextButton testToneButton { "Test tone" };
    juce::Label statusLabel;
    juce::Label placeholderLabel;

    // Touched by the audio thread, so keep them lock free.
    std::atomic<bool> testToneEnabled { false };
    std::atomic<float> testToneGain { 0.0f };

    double currentSampleRate = 44100.0;
    double tonePhase = 0.0;

    static constexpr double toneFrequencyHz = 440.0;
    static constexpr float toneTargetGain = 0.125f;   // about -18 dBFS

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace opendj
