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

    // What a DJ set needs out of a buffer size: small enough that a scratch
    // feels attached to the hand, large enough to survive a laptop deciding to
    // do something else for a moment. About five milliseconds at 48 kHz.
    constexpr int preferredBufferSize = 256;
    constexpr int smallestSafeBufferSize = 128;

    /** The available types, best first. */
    juce::Array<juce::AudioIODeviceType*> typesByPreference (juce::AudioDeviceManager& manager)
    {
        juce::Array<juce::AudioIODeviceType*> types;

        for (auto* type : manager.getAvailableDeviceTypes())
            if (type != nullptr)
                types.add (type);

        std::stable_sort (types.begin(), types.end(), [] (auto* a, auto* b)
        {
            return AudioEngine::preferenceForDeviceType (a->getTypeName())
                 < AudioEngine::preferenceForDeviceType (b->getTypeName());
        });

        return types;
    }
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

int AudioEngine::preferenceForDeviceType (const juce::String& typeName)
{
    // A driver written for the job, where one is installed at all.
    if (typeName == "ASIO" || typeName == "JACK" || typeName == "CoreAudio")
        return 0;

    // On Windows this is the default, and deliberately: the low latency mode
    // is what makes a stock machine with no extra drivers playable. It opened
    // at 7 ms here against DirectSound's 87.
    if (typeName.containsIgnoreCase ("Low Latency"))
        return 1;

    if (typeName.containsIgnoreCase ("Windows Audio"))
        return typeName.containsIgnoreCase ("Exclusive") ? 3 : 2;

    if (typeName == "ALSA")
        return 2;

    // Last, always. DirectSound is the one backend on a Windows machine that
    // cannot do low latency, whatever it is asked for.
    if (typeName.containsIgnoreCase ("DirectSound"))
        return 9;

    return 5;
}

AudioEngine::~AudioEngine()
{
    stop();
}

void AudioEngine::stop()
{
    stopTimer();

    // The callback goes first and the device with it, so nothing below can be
    // reached from the device thread while it is being pulled apart.
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();

    // Then the background work, which reaches the decks and the sampler.
    // Waiting here is the point: a decode still running when the decks are
    // freed is the same crash by a different route.
    loaderPool.removeAllJobs (true, 5000);
    separatorPool.removeAllJobs (true, 5000);

    // A recording still open would otherwise be left without its final flush.
    if (recorder.isRecording())
        recorder.stop();

    broadcaster.stop();
    rtmpBroadcaster.stop();
}

juce::String AudioEngine::initialise (const juce::StringArray& preferredDeviceNames,
                                      const juce::XmlElement* savedDeviceState)
{
    // A device chosen by hand last time wins over anything found by searching:
    // the search exists to make a good first guess, not to overrule a person
    // who has already answered the question.
    if (savedDeviceState != nullptr)
    {
        const auto saved = deviceManager.initialise (0, preferredOutputChannels,
                                                     savedDeviceState, true);

        if (saved.isEmpty() && deviceManager.getCurrentAudioDevice() != nullptr)
        {
            deviceChoiceReason = "chosen last time";
            deviceManager.addAudioCallback (this);
            startTimer (retirementSweepMs);
            return {};
        }
    }

    // The device manager has to be initialised before its device types can be
    // enumerated, so start with the default and then look for something better.
    auto error = deviceManager.initialiseWithDefaultDevices (0, preferredOutputChannels);

    if (error.isNotEmpty())
        return error;

    deviceChoiceReason = "default device";

    // The first initialise above opens whatever type JUCE starts on, which is
    // DirectSound on Windows. Move to the best available type straight away, so
    // a run that never gets past this point is still on a playable backend
    // rather than one that cannot do low latency.
    if (const auto types = typesByPreference (deviceManager); ! types.isEmpty())
    {
        const auto& best = types.getFirst()->getTypeName();

        if (deviceManager.getCurrentAudioDeviceType() != best)
        {
            deviceManager.setCurrentAudioDeviceType (best, true);
            deviceChoiceReason = "default device on " + best;
        }
    }

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
    for (auto* type : typesByPreference (deviceManager))
    {
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
                tightenBufferSize();
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
        for (auto* type : typesByPreference (deviceManager))
        {
            for (const auto& name : type->getDeviceNames (false))
            {
                if (openNamed (*type, name).isNotEmpty())
                    continue;

                if (auto* opened = deviceManager.getCurrentAudioDevice();
                    opened != nullptr && opened->getOutputChannelNames().size() >= preferredOutputChannels)
                {
                    deviceChoiceReason = "has a cue bus";
                    tightenBufferSize();
                    deviceManager.addAudioCallback (this);
                    startTimer (retirementSweepMs);
                    return {};
                }
            }
        }

        // Nothing with a cue bus was found, so take the default again rather
        // than leaving whichever device the search happened to stop on. The
        // best available type is asked first: its default output is a better
        // answer than DirectSound's, and asking by name is also what gives the
        // settings file something to remember.
        auto opened = false;

        for (auto* type : typesByPreference (deviceManager))
        {
            if (const auto names = type->getDeviceNames (false); ! names.isEmpty())
            {
                const auto index = juce::jmax (0, type->getDefaultDeviceIndex (false));
                const auto name = names[juce::jmin (index, names.size() - 1)];

                if (openNamed (*type, name).isEmpty()
                    && deviceManager.getCurrentAudioDevice() != nullptr)
                {
                    deviceChoiceReason = "best available default";
                    opened = true;
                    break;
                }
            }
        }

        if (! opened)
        {
            error = deviceManager.initialiseWithDefaultDevices (0, preferredOutputChannels);

            if (error.isNotEmpty())
                return error;
        }
    }

    tightenBufferSize();
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
        {
            const auto title = decks[(size_t) deckIndex]->getTrackTitle();
            recorder.noteTrack (title);

            // Listeners see the same thing the tracklist records.
            broadcaster.noteTrack (title);
        }

        if (onComplete != nullptr)
            juce::MessageManager::callAsync ([onComplete, succeeded] { onComplete (succeeded); });
    });
}

void AudioEngine::loadSampleAsync (int slot, const juce::File& file,
                                   std::function<void (juce::String)> onComplete)
{
    loaderPool.addJob ([this, slot, file, onComplete = std::move (onComplete)]
    {
        juce::String failureReason;
        auto decoded = TrackDecoder::decode (formatManager, file, &failureReason);

        // Decoding is the slow part and happens here; installing is one atomic
        // store and happens on the message thread, which owns the slot.
        juce::MessageManager::callAsync (
            [this, slot, file, name = file.getFileNameWithoutExtension(),
             decoded = std::shared_ptr<DecodedAudio> (std::move (decoded)),
             failureReason, onComplete]() mutable
            {
                auto error = failureReason;

                if (decoded != nullptr)
                {
                    auto owned = std::make_unique<DecodedAudio> (std::move (*decoded));
                    error = {};

                    if (! sampler.installSlot (slot, std::move (owned), name, &error, file)
                        && error.isEmpty())
                        error = "That sound could not be loaded.";
                }
                else if (error.isEmpty())
                {
                    error = "That sound could not be loaded.";
                }

                if (onComplete != nullptr)
                    onComplete (error);
            });
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

int AudioEngine::findSyncLeader (int followerIndex) const
{
    if (! juce::isPositiveAndBelow (followerIndex, numDecks))
        return -1;

    auto best = -1;
    auto bestDistance = numDecks + 1;

    for (int i = 0; i < numDecks; ++i)
    {
        if (i == followerIndex || ! decks[(size_t) i]->isPlaying())
            continue;

        const auto analysis = decks[(size_t) i]->getAnalysis();

        if (analysis == nullptr || ! analysis->hasTempo())
            continue;

        // Ties go to the lower deck, which is the one on the left.
        if (const auto distance = std::abs (i - followerIndex); distance < bestDistance)
        {
            bestDistance = distance;
            best = i;
        }
    }

    return best;
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

void AudioEngine::tightenBufferSize()
{
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return;

    const auto sizes = device->getAvailableBufferSizes();

    if (sizes.isEmpty() || device->getCurrentBufferSizeSamples() <= preferredBufferSize)
        return;

    // The smallest size at or above the target, so a device offering 128, 256
    // and 512 lands on 256 rather than on the smallest thing it will admit to.
    auto chosen = 0;

    for (const auto size : sizes)
        if (size >= smallestSafeBufferSize && (chosen == 0 || std::abs (size - preferredBufferSize)
                                                            < std::abs (chosen - preferredBufferSize)))
            chosen = size;

    if (chosen == 0 || chosen == device->getCurrentBufferSizeSamples())
        return;

    auto setup = deviceManager.getAudioDeviceSetup();
    setup.bufferSize = chosen;

    // A device that refuses keeps what it had, which is worse but still works.
    deviceManager.setAudioDeviceSetup (setup, true);
}

std::unique_ptr<juce::XmlElement> AudioEngine::getDeviceState()
{
    // JUCE only writes state for a device that was asked for by name, so a
    // session that took the default has nothing to save and would open the
    // default again next time even after the hardware changed underneath it.
    // Saying what is open, explicitly, is what makes it worth remembering.
    if (auto state = deviceManager.createStateXml(); state != nullptr)
        return state;

    if (deviceManager.getCurrentAudioDevice() == nullptr)
        return nullptr;

    deviceManager.setAudioDeviceSetup (deviceManager.getAudioDeviceSetup(), true);
    return deviceManager.createStateXml();
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
                << "  |  cue bus: "
                << (! hasCueOutput()                             ? "unavailable"
                  : getOutputMode() == OutputMode::splitStereo   ? "split across outputs 1-2"
                                                                 : "outputs 3-4");

    return description;
}

//==============================================================================

void AudioEngine::prepareToPlay (double sampleRate, int blockSize)
{
    for (auto& buffer : deckBuffers)
        buffer.setSize (2, blockSize, false, true, true);

    masterBuffer.setSize (2, blockSize, false, true, true);
    cueBuffer.setSize (2, blockSize, false, true, true);
    recordingBuffer.setSize (2, blockSize, false, true, true);
    inputBuffer.setSize (2, blockSize, false, true, true);

    for (auto& deck : decks)
        deck->prepare (sampleRate, blockSize);

    mixer.prepare (sampleRate, blockSize);
    sampler.prepare (sampleRate);
    mic.prepare (sampleRate);
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    prepareToPlay (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());

    deviceOutputChannels.store (device->getActiveOutputChannels().countNumberOfSetBits(),
                                std::memory_order_relaxed);
    deviceInputChannels.store (device->getActiveInputChannels().countNumberOfSetBits(),
                               std::memory_order_relaxed);
}

void AudioEngine::audioDeviceStopped()
{
    for (auto& deck : decks)
        deck->releaseResources();

    mixer.reset();
    sampler.stopAll();
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    renderNextBlock (outputChannelData, numOutputChannels, numSamples,
                     inputChannelData, numInputChannels);
}

void AudioEngine::renderNextBlock (float* const* outputs, int numOutputChannels, int numSamples,
                                  const float* const* inputs, int numInputChannels)
{
    // The mic's inputs are copied before the outputs are cleared, in case a
    // backend hands over input and output channels that share memory. Two at
    // most, which is all the mic reads.
    const float* micChannels[2] = { nullptr, nullptr };
    auto numMicChannels = 0;

    if (inputs != nullptr && numSamples > 0 && numSamples <= inputBuffer.getNumSamples())
    {
        for (int ch = 0; ch < numInputChannels && numMicChannels < 2; ++ch)
        {
            if (inputs[ch] == nullptr)
                continue;

            inputBuffer.copyFrom (numMicChannels, 0, inputs[ch], numSamples);
            micChannels[numMicChannels] = inputBuffer.getReadPointer (numMicChannels);
            ++numMicChannels;
        }
    }

    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputs[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputs[ch], numSamples);

    // The device can hand us a shorter block than it advertised, so never grow
    // a buffer here; that would allocate on the audio thread.
    if (numSamples <= 0 || numSamples > masterBuffer.getNumSamples())
        return;

    // Views of exactly this block's length over storage that already exists.
    // Default construction allocates nothing and neither does setDataToReferTo,
    // where assigning a buffer would copy and therefore allocate. Every deck
    // gets one: building this list by hand is how decks C and D came to render
    // into nothing at all.
    std::array<juce::AudioBuffer<float>, numDecks> deckViews;
    std::array<juce::AudioBuffer<float>*, numDecks> deckPointers {};

    for (size_t i = 0; i < (size_t) numDecks; ++i)
    {
        deckViews[i].setDataToReferTo (deckBuffers[i].getArrayOfWritePointers(), 2, numSamples);
        decks[i]->processBlock (deckViews[i]);
        deckPointers[i] = &deckViews[i];
    }

    juce::AudioBuffer<float> masterView (masterBuffer.getArrayOfWritePointers(), 2, numSamples);
    juce::AudioBuffer<float> cueView (cueBuffer.getArrayOfWritePointers(), 2, numSamples);

    mixer.processBlock (deckPointers, masterView, cueView);

    // After the mixer and so after the crossfader, which is what a sampler is:
    // a sound laid over the mix rather than one of the things being mixed.
    sampler.processBlock (masterView, cueView, numSamples);

    // The mic joins last, over the music and the sampler, and ducks them for
    // talkover. When it is kept out of the speakers it builds the mix to record
    // and broadcast in a buffer of its own, and the taps below read that.
    juce::AudioBuffer<float> recordingView (recordingBuffer.getArrayOfWritePointers(), 2, numSamples);
    const auto& tap = mic.processBlock (micChannels, numMicChannels, masterView, recordingView, numSamples)
                    ? recordingView
                    : masterView;

    // Recorded after the mixer and before the device, so the file holds exactly
    // what the room heard: crossfader, master gain, soft clip and all. Plus the
    // mic, including a mic the room was kept from hearing.
    recorder.write (tap, numSamples);

    // The same tap feeds the broadcast, so a listener hears what the room
    // hears. Both drop samples rather than stall, and neither can block here.
    broadcaster.write (tap, numSamples);

    // And the same tap again for an RTMP target, if one is live. A third
    // silent branch when neither broadcast is running, which is the common
    // case, and exactly as cheap as the other two: an atomic load and a
    // comparison against nullptr.
    rtmpBroadcaster.write (tap, numSamples);

    routeOutputs (masterView, cueView, outputs, numOutputChannels, numSamples,
                  outputMode.load (std::memory_order_relaxed));
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

    sampler.cleanUp();

    // The tempo fader moves, tracks change, and the echo has to follow both.
    updateEchoTimes();
}

} // namespace opendj
