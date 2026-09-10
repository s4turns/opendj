/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "core/AudioEngine.h"

namespace opendj
{

namespace
{
    constexpr int retirementSweepMs = 500;
    constexpr int preferredOutputChannels = 4;   // master pair plus cue pair
}

AudioEngine::AudioEngine()
{
    formatManager.registerBasicFormats();

    for (int i = 0; i < numDecks; ++i)
        decks[(size_t) i] = std::make_unique<Deck> (i, formatManager);
}

AudioEngine::~AudioEngine()
{
    stopTimer();
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

juce::String AudioEngine::initialise()
{
    const auto error = deviceManager.initialiseWithDefaultDevices (0, preferredOutputChannels);

    if (error.isNotEmpty())
        return error;

    deviceManager.addAudioCallback (this);
    startTimer (retirementSweepMs);
    return {};
}

juce::String AudioEngine::getDeviceDescription() const
{
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return "No audio device open.";

    const auto sampleRate = device->getCurrentSampleRate();
    const auto bufferSize = device->getCurrentBufferSizeSamples();
    const auto latencyMs = sampleRate > 0.0
        ? (device->getOutputLatencyInSamples() / sampleRate) * 1000.0
        : 0.0;

    juce::String description;
    description << device->getTypeName() << "  |  " << device->getName()
                << "  |  " << juce::String (sampleRate, 0) << " Hz"
                << "  |  " << bufferSize << " samples"
                << "  |  " << juce::String (latencyMs, 1) << " ms out"
                << "  |  cue bus: " << (hasCueOutput() ? "outputs 3-4" : "unavailable");

    return description;
}

//==============================================================================

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const auto sampleRate = device->getCurrentSampleRate();
    const auto blockSize = device->getCurrentBufferSizeSamples();

    for (auto& buffer : deckBuffers)
        buffer.setSize (2, blockSize, false, true, true);

    masterBuffer.setSize (2, blockSize, false, true, true);
    cueBuffer.setSize (2, blockSize, false, true, true);

    for (auto& deck : decks)
        deck->prepare (sampleRate, blockSize);

    mixer.prepare (sampleRate, blockSize);

    cueOutputAvailable.store (device->getActiveOutputChannels().countNumberOfSetBits() >= 4,
                              std::memory_order_relaxed);
}

void AudioEngine::audioDeviceStopped()
{
    for (auto& deck : decks)
        deck->releaseResources();

    mixer.reset();
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const*,
                                                    int,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    // The device can hand us a shorter block than it advertised, so never grow
    // a buffer here; that would allocate on the audio thread.
    if (numSamples > masterBuffer.getNumSamples())
        return;

    // Wrap the preallocated storage in views of exactly this block's length.
    // AudioBuffer's pointer constructor does not allocate.
    std::array<juce::AudioBuffer<float>, numDecks> deckViews
    {
        juce::AudioBuffer<float> (deckBuffers[0].getArrayOfWritePointers(), 2, numSamples),
        juce::AudioBuffer<float> (deckBuffers[1].getArrayOfWritePointers(), 2, numSamples)
    };

    juce::AudioBuffer<float> masterView (masterBuffer.getArrayOfWritePointers(), 2, numSamples);
    juce::AudioBuffer<float> cueView (cueBuffer.getArrayOfWritePointers(), 2, numSamples);

    std::array<juce::AudioBuffer<float>*, numDecks> deckPointers {};

    for (size_t i = 0; i < (size_t) numDecks; ++i)
    {
        decks[i]->processBlock (deckViews[i]);
        deckPointers[i] = &deckViews[i];
    }

    mixer.processBlock (deckPointers, masterView, cueView);

    const auto copyPair = [outputChannelData, numOutputChannels, numSamples]
                          (const juce::AudioBuffer<float>& source, int firstOutput)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto destination = firstOutput + ch;

            if (destination < numOutputChannels && outputChannelData[destination] != nullptr)
                juce::FloatVectorOperations::copy (outputChannelData[destination],
                                                   source.getReadPointer (ch),
                                                   numSamples);
        }
    };

    copyPair (masterView, 0);

    if (numOutputChannels >= 4)
        copyPair (cueView, 2);
}

//==============================================================================

void AudioEngine::timerCallback()
{
    for (auto& deck : decks)
        deck->cleanUp();
}

} // namespace opendj
