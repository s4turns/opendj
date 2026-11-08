/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "core/AudioEngine.h"

#include "core/TempoMath.h"
#include "analysis/TrackAnalyser.h"

#include <algorithm>

#include <cmath>
#include <new>

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

AudioEngine::AudioEngine (StemSeparator::Models stemModels)
    : stemSeparator (stemModels),
      stemQueue (makeStemQueueConfig())
{
    formatManager.registerBasicFormats();

    for (int i = 0; i < numDecks; ++i)
    {
        decks[(size_t) i] = std::make_unique<Deck> (i, formatManager);
        echoBeats[(size_t) i].store (1.0, std::memory_order_relaxed);

        // A pad can be pressed from the MIDI thread, which should not wait on
        // a database, so the cues are written from a loader thread instead.
        decks[(size_t) i]->onHotCuesChanged = [this] (const juce::File& file, const HotCues& cues)
        {
            loaderPool.addJob ([this, file, cues]
            {
                if (auto* cache = analysisCache.load (std::memory_order_acquire))
                    cache->storeHotCues (file, cues);
            });
        };

        // The same for a grid set by hand.
        decks[(size_t) i]->onBeatGridChanged = [this] (const juce::File& file, double bpm, double firstBeat)
        {
            loaderPool.addJob ([this, file, bpm, firstBeat]
            {
                if (auto* cache = analysisCache.load (std::memory_order_acquire))
                    cache->storeBeatGrid (file, bpm, firstBeat);
            });
        };
    }

    // Every frame the visualiser finishes is offered to the broadcast, which
    // ignores it unless it is running with live video. This is the one place
    // the two meet, so neither has to know the other exists.
    visualizer.setFrameSink ([this] (const unsigned char* rgb, int numBytes)
    {
        rtmpBroadcaster.pushVideoFrame (rgb, numBytes);
    });
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
    stemQueue.shutdown();

    // A recording still open would otherwise be left without its final flush.
    if (recorder.isRecording())
        recorder.stop();

    broadcaster.stop();
    rtmpBroadcaster.stop();
    visualizer.stop();
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

    // Left on, the new track would come up silent with nothing to say why.
    mixer.setChannelEchoOut (deckIndex, false);

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

        // A track analysed before loudness was measured has just been measured,
        // and that is the one part of this load worth keeping.
        if (succeeded && cache != nullptr && known.has_value() && known->analysed && known->loudnessLufs >= 0.0)
            if (const auto analysis = deck.getAnalysis(); analysis != nullptr && analysis->loudnessLufs < 0.0)
                cache->storeLoudness (file, analysis->loudnessLufs);

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

        // A loop or a phrase long enough to have a tempo gets one, so it can
        // be synced. Two bars at the slowest tempo worth syncing is about four
        // seconds; anything shorter is a stab, and a stab has no tempo.
        auto bpm = 0.0;

        if (decoded != nullptr && decoded->lengthSeconds() >= 4.0)
            if (const auto analysis = TrackAnalyser::analyse (decoded->audio, decoded->sampleRate);
                analysis->hasTempo() && analysis->confidence >= 0.3f)
                bpm = analysis->bpm;

        // Decoding is the slow part and happens here; installing is one atomic
        // store and happens on the message thread, which owns the slot.
        juce::MessageManager::callAsync (
            [this, slot, file, name = file.getFileNameWithoutExtension(),
             decoded = std::shared_ptr<DecodedAudio> (std::move (decoded)),
             failureReason, onComplete, bpm]() mutable
            {
                auto error = failureReason;

                if (decoded != nullptr)
                {
                    auto owned = std::make_unique<DecodedAudio> (std::move (*decoded));
                    error = {};

                    if (! sampler.installSlot (slot, std::move (owned), name, &error, file, bpm)
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

void AudioEngine::captureSampleAsync (int slot, int deckIndex, double beats, const juce::File& folder,
                                     std::function<void (juce::String)> onComplete)
{
    const auto fail = [&onComplete] (const juce::String& reason)
    {
        if (onComplete != nullptr)
            onComplete (reason);
    };

    if (! juce::isPositiveAndBelow (deckIndex, numDecks) || ! juce::isPositiveAndBelow (slot, Sampler::numSlots))
        return fail ("There is no such deck or pad.");

    auto& deck = *decks[(size_t) deckIndex];
    const auto analysis = deck.getAnalysis();
    const auto file = deck.getLoadedFile();

    if (! deck.isLoaded() || analysis == nullptr)
        return fail ("Load a track on the deck first.");

    if (! analysis->hasTempo())
        return fail ("The track has no beat grid, so there is no beat to record from. "
                     "Set one by clicking its BPM.");

    // The loop if one is running, which is what a DJ means by "that bit";
    // otherwise whole beats from the beat at or behind the playhead.
    auto start = analysis->beatAtOrBehindSeconds (deck.getPositionSeconds());
    auto length = beats * analysis->secondsPerBeat();

    if (deck.isLoopEnabled() && deck.hasLoop())
    {
        start = deck.getLoopStartSeconds();
        length = deck.getLoopEndSeconds() - start;
    }

    if (length <= 0.0 || length > 60.0)
        return fail ("A sample has to be under a minute long.");

    const auto bpm = analysis->bpm;
    const auto name = deck.getTrackTitle() + " " + juce::String (bpm, 1) + " BPM";

    loaderPool.addJob ([this, slot, file, start, length, bpm, name, folder, onComplete]
    {
        juce::String reason;
        auto decoded = TrackDecoder::decode (formatManager, file, &reason);
        juce::File written;

        if (decoded != nullptr)
        {
            const auto rate = decoded->sampleRate;
            const auto first = juce::jlimit (0, decoded->audio.getNumSamples(), (int) std::lround (start * rate));
            const auto count = juce::jlimit (0, decoded->audio.getNumSamples() - first, (int) std::lround (length * rate));

            if (count <= 0)
            {
                decoded = nullptr;
                reason = "That part of the track is empty.";
            }
            else
            {
                juce::AudioBuffer<float> slice (decoded->audio.getNumChannels(), count);

                for (int ch = 0; ch < slice.getNumChannels(); ++ch)
                    slice.copyFrom (ch, 0, decoded->audio, ch, first, count);

                decoded->audio = std::move (slice);

                // Kept as a file, so the pad comes back after a restart like
                // any other and can be dragged into another program.
                folder.createDirectory();
                written = folder.getNonexistentChildFile (juce::File::createLegalFileName (name), ".wav", false);

                juce::WavAudioFormat wav;
                std::unique_ptr<juce::OutputStream> stream (written.createOutputStream());

                if (stream != nullptr)
                {
                    const auto options = juce::AudioFormatWriterOptions()
                                             .withSampleRate (rate)
                                             .withNumChannels (decoded->audio.getNumChannels())
                                             .withBitsPerSample (24);

                    if (auto writer = wav.createWriterFor (stream, options); writer != nullptr)
                        writer->writeFromAudioSampleBuffer (decoded->audio, 0, decoded->audio.getNumSamples());
                }

                if (written.getSize() <= 0)
                    written = juce::File();   // the pad still gets the sound, just not after a restart
            }
        }

        juce::MessageManager::callAsync (
            [this, slot, name, bpm, written, reason, onComplete,
             decoded = std::shared_ptr<DecodedAudio> (std::move (decoded))]() mutable
            {
                auto error = reason;

                if (decoded != nullptr)
                {
                    error = {};

                    if (! sampler.installSlot (slot, std::make_unique<DecodedAudio> (std::move (*decoded)),
                                               name, &error, written, bpm)
                        && error.isEmpty())
                        error = "The sample could not be put on the pad.";
                }
                else if (error.isEmpty())
                {
                    error = "The track could not be read again to record from it.";
                }

                if (onComplete != nullptr)
                    onComplete (error);
            });
    });
}

double AudioEngine::getMasterTempo() const
{
    for (int i = 0; i < numDecks; ++i)
        if (decks[(size_t) i]->isPlaying())
            if (const auto bpm = getEffectiveBpm (i); bpm > 0.0)
                return bpm;

    return 0.0;
}

juce::File AudioEngine::defaultSampleFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("OpenDJ").getChildFile ("Samples");
}

void AudioEngine::setAutoGainEnabled (bool shouldLevel)
{
    for (auto& deck : decks)
        deck->setAutoGainEnabled (shouldLevel);
}

bool AudioEngine::matchDeckKey (int followerIndex, int leaderIndex)
{
    if (! juce::isPositiveAndBelow (followerIndex, numDecks) || ! juce::isPositiveAndBelow (leaderIndex, numDecks)
        || followerIndex == leaderIndex)
        return false;

    auto& follower = *decks[(size_t) followerIndex];
    const auto target = decks[(size_t) leaderIndex]->getEffectiveKey();
    const auto current = follower.getEffectiveKey();

    if (! target.isValid() || ! current.isValid())
        return false;

    // From where it would be with no shift at all, so that matching twice gives
    // the same answer and a shift already there is not added to.
    follower.setKeyShift (current.transposed (-follower.getKeyShift()).semitonesToMatch (target));
    return true;
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

StemQueue::Config AudioEngine::makeStemQueueConfig()
{
    StemQueue::Config config;

    config.work = [this] (const juce::File& track,
                          const std::function<void (float)>& onProgress,
                          const std::atomic<bool>& stop)
    {
        return stemSeparator.prepare (formatManager, track, onProgress, stop);
    };

    config.anyDeckPlaying = [this]
    {
        return std::any_of (decks.begin(), decks.end(), [] (const auto& deck) { return deck->isPlaying(); });
    };

    config.freeBytes = [this] { return stemSeparator.getFreeBytes(); };
    config.onReady = [this] (const juce::File& track) { giveStemsToDecks (track); };

    config.onADeck = [this] (const juce::File& track)
    {
        return std::any_of (decks.begin(), decks.end(), [&track] (const auto& deck) { return deck->holdsFile (track); });
    };

    return config;
}

void AudioEngine::giveStemsToDecks (const juce::File& track)
{
    // Decoded only if a deck wants it: a list prepared ahead of time would
    // otherwise pass through memory at 600 MB a track for nothing. A load racing
    // this finds the stems for itself, since they are in place by now.
    //
    // This is the queue's worker, so the decks are asked under their own lock:
    // a track retired by a load is freed on the message thread.
    std::shared_ptr<const SeparatedTrack> stems;

    for (auto& deck : decks)
    {
        if (deck->hasStems() || ! deck->holdsFile (track))
            continue;

        if (stems == nullptr)
        {
            auto broken = false;

            try
            {
                stems = stemSeparator.loadFromCache (formatManager, track, &broken);
            }
            catch (const std::bad_alloc&)
            {
                broken = false;
            }

            if (stems == nullptr)
            {
                // It said it was there and will not decode. Without this, asking
                // again would get the same entry for ever. Not for want of
                // memory, though, which is no fault of minutes of good work.
                if (broken)
                    stemSeparator.discardCacheEntry (track);

                return;
            }
        }

        deck->setSeparation (stems, track);
    }
}

void AudioEngine::separateDeckAsync (int deckIndex, std::function<void (bool)> onComplete, bool byHand)
{
    const auto tell = [onComplete = std::move (onComplete)] (bool succeeded)
    {
        if (onComplete != nullptr)
            juce::MessageManager::callAsync ([onComplete, succeeded] { onComplete (succeeded); });
    };

    const auto file = juce::isPositiveAndBelow (deckIndex, numDecks) ? decks[(size_t) deckIndex]->getLoadedFile()
                                                                     : juce::File();

    if (! file.existsAsFile() || ! stemSeparator.isAvailable())
    {
        tell (false);
        return;
    }

    // Nothing here says the deck is busy. That belonged to the job rather than
    // to the deck, and it stopped a new track on this deck asking for its stems
    // while the last one's were still running.
    //
    // A request given up on, because the deck moved on to another track before
    // its turn came or the engine is stopping, is not a failure to report.
    stemQueue.requestNow (file, [tell] (StemOutcome outcome)
    {
        if (outcome != StemOutcome::stopped)
            tell (outcome == StemOutcome::ready);
    }, byHand);
}

bool AudioEngine::isDeckSeparating (int deckIndex) const
{
    if (! juce::isPositiveAndBelow (deckIndex, numDecks))
        return false;

    const auto file = decks[(size_t) deckIndex]->getLoadedFile();
    return file != juce::File() && stemQueue.isRequested (file);
}

float AudioEngine::getSeparationProgress (int deckIndex) const
{
    return juce::isPositiveAndBelow (deckIndex, numDecks)
        ? stemQueue.getFractionFor (decks[(size_t) deckIndex]->getLoadedFile())
        : 0.0f;
}

void AudioEngine::prepareStems (const std::vector<juce::File>& tracks)
{
    if (! stemSeparator.isAvailable())
        return;

    // A playlist keeps tracks that have since moved or gone. Each would fail in
    // a moment, and three together would halt the lane as though the machine
    // were at fault, with the rest of the list behind them.
    std::vector<juce::File> there;
    there.reserve (tracks.size());

    for (const auto& track : tracks)
        if (track.existsAsFile())
            there.push_back (track);

    stemQueue.requestInBackground (there);
}

void AudioEngine::cancelStemPreparation()
{
    stemQueue.cancelBackground();
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

bool AudioEngine::syncDeck (int followerIndex, int leaderIndex, double maxStretch)
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
    const auto ratio = tempo::matchRatio (leaderBpm, followerAnalysis->bpm);

    if (! tempo::isGentle (ratio, maxStretch))
        return false;

    follower.setTempoRatio (ratio);

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

juce::File AudioEngine::startRecording (const RecordingSettings& settings, juce::String& error)
{
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
    {
        error = "There is no audio device open to record from.";
        return {};
    }

    const auto file = recorder.start (settings, device->getCurrentSampleRate(), error);

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
    inputBuffer.setSize (maxInputChannels, blockSize, false, true, true);

    for (auto& deck : decks)
        deck->prepare (sampleRate, blockSize);

    mixer.prepare (sampleRate, blockSize);
    sampler.prepare (sampleRate);
    mic.prepare (sampleRate);
    aux.prepare (sampleRate);
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
    // The inputs are copied before the outputs are cleared, in case a backend
    // hands over input and output channels that share memory. Only the active
    // ones, packed together, which is how the mic and aux count their pairs.
    const float* inputChannels[maxInputChannels] = {};
    auto numActiveInputs = 0;

    if (inputs != nullptr && numSamples > 0 && numSamples <= inputBuffer.getNumSamples())
    {
        for (int ch = 0; ch < numInputChannels && numActiveInputs < maxInputChannels; ++ch)
        {
            if (inputs[ch] == nullptr)
                continue;

            inputBuffer.copyFrom (numActiveInputs, 0, inputs[ch], numSamples);
            inputChannels[numActiveInputs] = inputBuffer.getReadPointer (numActiveInputs);
            ++numActiveInputs;
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

    // The aux joins next and the mic last, over the music and the sampler, so
    // talkover ducks the aux along with everything else. Either can be kept
    // out of the speakers, and then builds the mix to record and broadcast in
    // a buffer of its own; the mic adds itself to that one when the aux made
    // it, and the taps below read it.
    juce::AudioBuffer<float> recordingView (recordingBuffer.getArrayOfWritePointers(), 2, numSamples);
    const auto auxSeparate = aux.processBlock (inputChannels, numActiveInputs, masterView, recordingView, numSamples);
    const auto& tap = mic.processBlock (inputChannels, numActiveInputs, masterView, recordingView, numSamples, auxSeparate)
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

    // And once more for the visuals, which want to see what the room hears
    // for the same reason a listener does. Same bargain, same cost when off.
    visualizer.write (tap, numSamples);

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

        // The stem effect wants the beat as recorded and not at the fader. Under
        // a stretcher it sits ahead of what the fader does to the tempo, and the
        // deck knows that better than this does.
        auto& deck = *decks[(size_t) i];
        const auto analysis = deck.getAnalysis();
        deck.setStemFxBeatSeconds (analysis != nullptr && analysis->hasTempo() ? analysis->secondsPerBeat() : 0.5);
    }

    const auto tempoDeck = findMasterTempoDeck();
    mixer.getMasterEffects().setBeatSeconds (tempoDeck >= 0 ? 60.0 / getEffectiveBpm (tempoDeck) : 0.5);

    // With nothing to follow, the gate and the sweeps carry on from their own
    // clock at whatever tempo they were told last.
    if (tempoDeck >= 0)
    {
        const auto& deck = *decks[(size_t) tempoDeck];

        if (const auto analysis = deck.getAnalysis(); analysis != nullptr && analysis->hasTempo())
            mixer.getMasterEffects().setBeatPosition ((deck.getPositionSeconds() - analysis->firstBeatSeconds)
                                                      / analysis->secondsPerBeat());
    }
}

int AudioEngine::findMasterTempoDeck() const
{
    return findLoudestPlayingDeck (true);
}

juce::String AudioEngine::getNowPlayingTitle() const
{
    const auto deck = findLoudestPlayingDeck (false);
    return deck >= 0 ? decks[(size_t) deck]->getTrackTitle() : juce::String();
}

float AudioEngine::getDeckAudibility (int deckIndex) const
{
    // Where the crossfader leaves each side, on a straight line. The real curve
    // does not matter here: only which deck is loudest does.
    const auto x = (mixer.getCrossfaderPosition() + 1.0f) * 0.5f;
    const auto assign = mixer.getChannelCrossfaderAssign (deckIndex);
    const auto side = assign == Mixer::CrossfaderAssign::a ? 1.0f - x
                    : assign == Mixer::CrossfaderAssign::b ? x
                                                           : 1.0f;

    return mixer.getChannelFader (deckIndex) * side;
}

AudioEngine::DeckStatus AudioEngine::getDeckStatus (int deckIndex) const
{
    if (! juce::isPositiveAndBelow (deckIndex, numDecks))
        return {};

    const auto& deck = *decks[(size_t) deckIndex];
    return { deck.isLoaded(), deck.isPlaying(), getDeckAudibility (deckIndex), deck.getPositionSeconds() };
}

int AudioEngine::findNextUpDeck() const
{
    const auto onAir = findLoudestPlayingDeck (false);

    if (onAir < 0)
        return -1;

    auto best = -1;
    juce::uint64 bestSerial = 0;

    for (int i = 0; i < numDecks; ++i)
    {
        const auto& deck = *decks[(size_t) i];

        if (i == onAir || ! deck.isLoaded() || deck.isPlaying())
            continue;

        if (deck.getLoadSerial() > bestSerial)
        {
            bestSerial = deck.getLoadSerial();
            best = i;
        }
    }

    return best;
}

juce::String AudioEngine::getComingUpTitle() const
{
    const auto deck = findNextUpDeck();
    return deck >= 0 ? decks[(size_t) deck]->getTrackTitle() : juce::String();
}

int AudioEngine::findLoudestPlayingDeck (bool requireBeatGrid) const
{
    auto best = -1;
    auto bestLevel = 0.0f;

    for (int i = 0; i < numDecks; ++i)
    {
        if (! decks[(size_t) i]->isPlaying() || (requireBeatGrid && getEffectiveBpm (i) <= 0.0))
            continue;

        // Ties go to the lower deck, as they do for sync.
        if (const auto level = getDeckAudibility (i); level > bestLevel)
        {
            bestLevel = level;
            best = i;
        }
    }

    return best;
}

void AudioEngine::timerCallback()
{
    for (auto& deck : decks)
        deck->cleanUp();

    sampler.cleanUp();
    mixer.getMasterEffects().collectGarbage();

    // The tempo fader moves, tracks change, and the echo has to follow both.
    updateEchoTimes();

    // So do synced samples.
    sampler.setTempoReference (getMasterTempo());
}

} // namespace opendj
