/*
    This file is part of OpenDJ.
    Copyright (C) 2026 The OpenDJ contributors.

    OpenDJ is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. See the LICENSE file at the root of this repository.
*/

#include "app/MainComponent.h"

namespace opendj
{

namespace
{
    // Two output pairs: master on 1-2 and headphone cue on 3-4. Controllers with
    // a built in interface, such as the Roland DJ-202, expose exactly this.
    constexpr int minOutputChannels = 2;
    constexpr int maxOutputChannels = 4;
}

MainComponent::MainComponent()
    : deviceSelector (deviceManager,
                      0, 0,                                  // no inputs yet
                      minOutputChannels, maxOutputChannels,
                      false,                                 // no MIDI input list yet
                      false,                                 // no MIDI output selector yet
                      true,                                  // stereo pair channel display
                      false)
{
    setAudioChannels (0, maxOutputChannels);

    addAndMakeVisible (deviceSelector);

    testToneButton.setClickingTogglesState (true);
    testToneButton.setTooltip ("Emit a 440 Hz sine at -18 dBFS on the master pair.");
    testToneButton.onClick = [this]
    {
        testToneEnabled.store (testToneButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (testToneButton);

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible (statusLabel);

    placeholderLabel.setText ("Decks land here.", juce::dontSendNotification);
    placeholderLabel.setJustificationType (juce::Justification::centred);
    placeholderLabel.setColour (juce::Label::textColourId, juce::Colours::darkgrey);
    addAndMakeVisible (placeholderLabel);

    updateDeviceStatusText();
    setSize (1100, 700);
}

MainComponent::~MainComponent()
{
    shutdownAudio();
}

void MainComponent::prepareToPlay (int, double sampleRate)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    tonePhase = 0.0;

    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safe != nullptr)
            safe->updateDeviceStatusText();
    });
}

void MainComponent::getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill)
{
    bufferToFill.clearActiveBufferRegion();

    // Ramp the gain rather than gating the tone, so toggling never clicks.
    const bool wanted = testToneEnabled.load (std::memory_order_relaxed);
    const float target = wanted ? toneTargetGain : 0.0f;
    float gain = testToneGain.load (std::memory_order_relaxed);

    if (juce::approximatelyEqual (gain, 0.0f) && ! wanted)
        return;

    const auto numSamples = bufferToFill.numSamples;
    const auto phaseDelta = juce::MathConstants<double>::twoPi * toneFrequencyHz / currentSampleRate;
    const float gainStep = 1.0f / static_cast<float> (juce::jmax (1, static_cast<int> (currentSampleRate * 0.01)));

    // Master is the first output pair only; the cue pair stays silent for now.
    const int channelsToFill = juce::jmin (2, bufferToFill.buffer->getNumChannels());

    for (int i = 0; i < numSamples; ++i)
    {
        gain = target > gain ? juce::jmin (target, gain + gainStep)
                             : juce::jmax (target, gain - gainStep);

        const auto sample = static_cast<float> (std::sin (tonePhase)) * gain;
        tonePhase += phaseDelta;

        for (int ch = 0; ch < channelsToFill; ++ch)
            bufferToFill.buffer->addSample (ch, bufferToFill.startSample + i, sample);
    }

    if (tonePhase > juce::MathConstants<double>::twoPi)
        tonePhase -= juce::MathConstants<double>::twoPi * std::floor (tonePhase / juce::MathConstants<double>::twoPi);

    testToneGain.store (gain, std::memory_order_relaxed);
}

void MainComponent::releaseResources()
{
    testToneGain.store (0.0f, std::memory_order_relaxed);
}

void MainComponent::updateDeviceStatusText()
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const auto latencySamples = device->getOutputLatencyInSamples();
        const auto sampleRate = device->getCurrentSampleRate();
        const auto latencyMs = sampleRate > 0.0 ? (latencySamples / sampleRate) * 1000.0 : 0.0;

        statusLabel.setText (device->getTypeName() + " | " + device->getName()
                                 + " | " + juce::String (sampleRate, 0) + " Hz"
                                 + " | buffer " + juce::String (device->getCurrentBufferSizeSamples())
                                 + " | output latency " + juce::String (latencyMs, 1) + " ms",
                             juce::dontSendNotification);
    }
    else
    {
        statusLabel.setText ("No audio device open.", juce::dontSendNotification);
    }
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff141418));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (12);

    auto footer = area.removeFromBottom (28);
    testToneButton.setBounds (footer.removeFromLeft (110));
    footer.removeFromLeft (12);
    statusLabel.setBounds (footer);

    area.removeFromBottom (8);
    deviceSelector.setBounds (area.removeFromLeft (juce::jmin (460, area.getWidth() / 2)));
    placeholderLabel.setBounds (area.reduced (12));
}

} // namespace opendj
