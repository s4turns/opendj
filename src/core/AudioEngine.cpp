/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "core/AudioEngine.h"

#include <algorithm>

#include <cmath>

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
    {
        decks[(size_t) i] = std::make_unique<Deck> (i, formatManager);
        echoBeats[(size_t) i].store (1.0, std::memory_order_relaxed);
    }
}

AudioEngine::~AudioEngine()
{
    stopTimer();

    // Let any decode in flight finish before the decks go away.
    loaderPool.removeAllJobs (true, 5000);

    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

juce::String AudioEngine::initialise (const juce::StringArray& preferredDeviceNames)
{
    // The device manager has to be initialised before its device types can be
    // enumerated, so start with the default and then look for something better.
    auto error = deviceManager.initialiseWithDefaultDevices (0, preferredOutputChannels);

    if (error.isNotEmpty())
        return error;

    deviceChoiceReason = "default device";

    const auto openNamed = [this] (juce::AudioIODeviceType& type, const juce::String& name)
    {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.outputDeviceName = name;
        setup.inputDeviceName = {};
        setup.useDefaultInputChannels = false;
        setup.inputChannels.clear();
        setup.useDefaultOutputChannels = true;

        deviceManager.setCurrentAudioDeviceType (type.getTypeName(), true);
        return deviceManager.setAudioDeviceSetup (setup, true);
    };

    // The controller's own interface first, wherever it turns up. Its four
    // outputs are exactly the master pair and the cue pair the mixer wants.
    for (auto* type : deviceManager.getAvailableDeviceTypes())
    {
        if (type == nullptr)
            continue;

        type->scanForDevices();

        for (const auto& name : type->getDeviceNames (false))
        {
            const auto matches = std::any_of (preferredDeviceNames.begin(), preferredDeviceNames.end(),
                                              [&name] (const juce::String& hint)
                                              {
                                                  return hint.isNotEmpty() && name.containsIgnoreCase (hint);
                                              });

            if (! matches)
                continue;

            if (openNamed (*type, name).isEmpty())
            {
                deviceChoiceReason = "matched the controller";
                deviceManager.addAudioCallback (this);
                startTimer (retirementSweepMs);
                return {};
            }
        }
    }

    // Failing that, anything that can carry a cue bus beats something that
    // cannot: two outputs means mixing with no way to hear what is coming.
    if (auto* device = deviceManager.getCurrentAudioDevice();
        device == nullptr || device->getOutputChannelNames().size() < preferredOutputChannels)
    {
        for (auto* type : deviceManager.getAvailableDeviceTypes())
        {
            if (type == nullptr)
                continue;

            for (const auto& name : type->getDeviceNames (false))
            {
                if (openNamed (*type, name).isNotEmpty())
                    continue;

                if (auto* opened = deviceManager.getCurrentAudioDevice();
                    opened != nullptr && opened->getOutputChannelNames().size() >= preferredOutputChannels)
                {
                    deviceChoiceReason = "has a cue bus";
                    deviceManager.addAudioCallback (this);
                    startTimer (retirementSweepMs);
                    return {};
                }
            }
        }

        // Nothing better was found, so go back to the default rather than
        // leaving whichever device the search happened to stop on.
        error = deviceManager.initialiseWithDefaultDevices (0, preferredOutputChannels);

        if (error.isNotEmpty())
            return error;
    }

    deviceManager.addAudioCallback (this);
    startTimer (retirementSweepMs);
    return {};
}

void AudioEngine::loadTrackAsync (int deckIndex, const juce::File& file,
                                  std::function<void (bool)> onComplete)
{
    if (! juce::isPositiveAndBelow (deckIndex, numDecks))
        return;

    // Ignore a second request for the same deck rather than queueing it; the
    // user pressing load twice should not analyse the same file twice.
    if (loading[(size_t) deckIndex].exchange (true, std::memory_order_relaxed))
        return;

    loaderPool.addJob ([this, deckIndex, file, onComplete = std::move (onComplete)]
    {
        auto& deck = *decks[(size_t) deckIndex];
        auto* cache = analysisCache.load (std::memory_order_acquire);

        const auto known = cache != nullptr ? cache->lookup (file) : std::nullopt;
        const auto succeeded = deck.loadFile (file, known.has_value() ? &*known : nullptr);

        // Only what was worked out here goes back; a cached answer is not
        // written over itself.
        if (succeeded && cache != nullptr && ! (known.has_value() && known->analysed))
            if (const auto analysis = deck.getAnalysis(); analysis != nullptr)
                cache->store (file, *analysis, deck.getLengthSeconds());

        // A track separated on some earlier day costs only a decode, so the
        // stems are there the moment it starts playing.
        if (succeeded && stemSeparator.isAvailable())
            if (auto cached = stemSeparator.loadFromCache (formatManager, file))
                deck.setSeparation (std::move (cached), file);

        loading[(size_t) deckIndex].store (false, std::memory_order_relaxed);

        // A loaded track goes into the tracklist, so a recorded set comes with
        // one rather than two unbroken hours nobody can navigate.
        if (succeeded)
            recorder.noteTrack (decks[(size_t) deckIndex]->getTrackTitle());

        if (onComplete != nullptr)
            juce::MessageManager::callAsync ([onComplete, succeeded] { onComplete (succeeded); });
    });
}

double AudioEngine::getEffectiveBpm (int deckIndex) const
{
    if (! juce::isPositiveAndBelow (deckIndex, numDecks))
        return 0.0;

    const auto& deck = *decks[(size_t) deckIndex];
    const auto analysis = deck.getAnalysis();

    if (analysis == nullptr || ! analysis->hasTempo())
        return 0.0;

    return analysis->bpm * deck.getTempoRatio();
}

void AudioEngine::separateDeckAsync (int deckIndex, std::function<void (bool)> onComplete)
{
    if (! juce::isPositiveAndBelow (deckIndex, numDecks) || ! stemSeparator.isAvailable())
        return;

    const auto file = decks[(size_t) deckIndex]->getLoadedFile();

    if (! file.existsAsFile())
        return;

    // One separation at a time, per deck and across the pool: it is minutes of
    // work on every core the machine has, and two at once would starve both.
    if (separating[(size_t) deckIndex].exchange (true, std::memory_order_relaxed))
        return;

    separationProgress[(size_t) deckIndex].store (0.0f, std::memory_order_relaxed);

    separatorPool.addJob ([this, deckIndex, file, onComplete = std::move (onComplete)]
    {
        auto separation = stemSeparator.separate (formatManager, file,
                                                  [this, deckIndex] (float progress)
        {
            separationProgress[(size_t) deckIndex].store (progress, std::memory_order_relaxed);
        });

        if (separation != nullptr)
            decks[(size_t) deckIndex]->setSeparation (separation, file);

        separating[(size_t) deckIndex].store (false, std::memory_order_relaxed);

        if (onComplete != nullptr)
        {
            const auto succeeded = separation != nullptr;
            juce::MessageManager::callAsync ([onComplete, succeeded] { onComplete (succeeded); });
        }
    });
}

bool AudioEngine::syncDeck (int followerIndex, int leaderIndex)
{
    if (! juce::isPositiveAndBelow (followerIndex, numDecks)
        || ! juce::isPositiveAndBelow (leaderIndex, numDecks)
        || followerIndex == leaderIndex)
        return false;

    auto& follower = *decks[(size_t) followerIndex];
    auto& leader = *decks[(size_t) leaderIndex];

    const auto followerAnalysis = follower.getAnalysis();
    const auto leaderAnalysis = leader.getAnalysis();

    if (followerAnalysis == nullptr || ! followerAnalysis->hasTempo()
        || leaderAnalysis == nullptr || ! leaderAnalysis->hasTempo())
        return false;

    const auto leaderBpm = leaderAnalysis->bpm * leader.getTempoRatio();
    follower.setTempoRatio (leaderBpm / followerAnalysis->bpm);

    // Match phase as well as tempo: work out how far through its beat the leader
    // is, then put the follower the same distance through one of its own.
    const auto leaderBeat = leaderAnalysis->secondsPerBeat();
    const auto followerBeat = followerAnalysis->secondsPerBeat();

    if (leaderBeat <= 0.0 || followerBeat <= 0.0)
        return true;

    auto phase = std::fmod (leader.getPositionSeconds() - leaderAnalysis->firstBeatSeconds, leaderBeat);

    if (phase < 0.0)
        phase += leaderBeat;

    const auto proportion = phase / leaderBeat;
    const auto nearestBeat = followerAnalysis->nearestBeatSeconds (follower.getPositionSeconds());

    follower.seekToSeconds (nearestBeat + proportion * followerBeat);
    return true;
}

juce::File AudioEngine::startRecording (juce::String& error)
{
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
    {
        error = "There is no audio device open to record from.";
        return {};
    }

    const auto file = recorder.start (SetRecorder::defaultFolder(),
                                      device->getCurrentSampleRate(), error);

    // Whatever is already playing belongs at the top of the tracklist. Recording
    // usually starts a minute into the first track, not before it, and a list
    // that begins with the second record is missing the one people ask about.
    if (file != juce::File())
        for (auto& deck : decks)
            if (deck->isLoaded() && deck->isPlaying())
                recorder.noteTrack (deck->getTrackTitle());

    return file;
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

    // Recorded after the mixer and before the device, so the file holds exactly
    // what the room heard: crossfader, master gain, soft clip and all.
    recorder.write (masterView, numSamples);

    copyPair (masterView, 0);

    if (numOutputChannels >= 4)
        copyPair (cueView, 2);
}

//==============================================================================

void AudioEngine::setEchoBeats (int deckIndex, double beats)
{
    if (! juce::isPositiveAndBelow (deckIndex, numDecks))
        return;

    echoBeats[(size_t) deckIndex].store (juce::jlimit (0.0625, 8.0, beats), std::memory_order_relaxed);
    updateEchoTimes();
}

void AudioEngine::updateEchoTimes()
{
    for (int i = 0; i < numDecks; ++i)
    {
        const auto bpm = getEffectiveBpm (i);

        // No grid, no tempo to sync to. Half a second is a musical length and a
        // better answer than switching the effect off.
        const auto beatSeconds = bpm > 0.0 ? 60.0 / bpm : 0.5;

        mixer.setChannelEchoTime (i, beatSeconds * echoBeats[(size_t) i].load (std::memory_order_relaxed));
    }
}

void AudioEngine::timerCallback()
{
    for (auto& deck : decks)
        deck->cleanUp();

    // The tempo fader moves, tracks change, and the echo has to follow both.
    updateEchoTimes();
}

} // namespace opendj
