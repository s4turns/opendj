/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "analysis/AnalysisCache.h"
#include "analysis/StemSeparator.h"
#include "core/Deck.h"
#include "core/MicInput.h"
#include "core/Mixer.h"
#include "core/OutputRouter.h"
#include "core/Sampler.h"
#include "core/SetRecorder.h"
#include "stream/Broadcaster.h"
#include "stream/RtmpBroadcaster.h"
#include "visual/Visualizer.h"

#include <array>
#include <atomic>
#include <functional>
#include <memory>

namespace opendj
{

/** Owns the audio device, the decks and the mixer, and does the per-block work.

    Output routing follows the convention every DJ interface uses: master on the
    first output pair, headphone cue on the second. A controller with a built in
    interface, such as the Roland DJ-202, presents exactly those four channels.
    On a plain two channel device the cue bus has nowhere to go and is dropped.
*/
class AudioEngine final : private juce::AudioIODeviceCallback,
                          private juce::Timer
{
public:
    static constexpr int numDecks = Mixer::numChannels;

    AudioEngine();
    ~AudioEngine() override;

    /** Opens an output device and starts the engine. Returns an error string,
        empty on success.

        A DJ controller almost always carries its own audio interface, and it is
        the one the music has to come out of: playing into the machine's default
        output means no headphone cue at all, and a jog wheel felt through the
        desktop's latency feels broken however good the MIDI is. Names to look
        for are passed in, and the default device is the fallback rather than
        the first choice. */
    juce::String initialise (const juce::StringArray& preferredDeviceNames = {},
                             const juce::XmlElement* savedDeviceState = nullptr);

    /** The open device as XML, to be handed back to initialise next time.
        Returns null before a device is open. */
    std::unique_ptr<juce::XmlElement> getDeviceState();

    /** How much a device type is worth reaching for, lower being better.

        Public because it is the one rule in the device search worth pinning
        down in a test: a build that quietly started preferring DirectSound
        again would sound broken and nothing would fail. */
    static int preferenceForDeviceType (const juce::String& typeName);

    /** Takes the device away before anything else is torn down.

        The destructor does this too, but by then the decks, the mixer and the
        sampler are minutes from being freed and the device thread is still
        calling into them. Shutting down explicitly, first, is the only ordering
        that is actually guaranteed. Safe to call twice. */
    void stop();

    /** The device that was opened, and why. For the status bar and the log. */
    juce::String getDeviceChoiceReason() const { return deviceChoiceReason; }

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
    juce::AudioFormatManager& getFormatManager() noexcept { return formatManager; }

    Deck& getDeck (int deckIndex) noexcept { return *decks[(size_t) deckIndex]; }
    Mixer& getMixer() noexcept { return mixer; }

    /** Eight slots of short sounds, mixed in after the mixer. They join the
        master rather than a channel because a sample is dropped on top of the
        mix, not faded into it. */
    Sampler& getSampler() noexcept { return sampler; }

    /** A microphone from the device's inputs, laid over the mix after the
        sampler, with talkover and a choice of whether the speakers hear it. */
    MicInput& getMic() noexcept { return mic; }

    /** Reads a file into a sampler slot on a background thread. The callback
        runs on the message thread, with an empty string on success and the
        reason on failure. */
    void loadSampleAsync (int slot, const juce::File& file,
                          std::function<void (juce::String)> onComplete = {});

    /** Decodes and analyses a file on a background thread, then swaps it onto
        the deck. Decoding a long track and finding its beat grid takes a second
        or two, which is far too long to spend on the message thread. The
        callback runs on the message thread. */
    void loadTrackAsync (int deckIndex, const juce::File& file,
                         std::function<void (bool)> onComplete = {});

    /** Somewhere to ask whether a file was analysed before, and to report the
        result when it was not. Optional; without one every load runs the full
        analysis. The cache must outlive the engine or be cleared with null. */
    void setAnalysisCache (AnalysisCache* cache) noexcept
    {
        analysisCache.store (cache, std::memory_order_release);
    }

    /** Separates the track on a deck into its four parts, in the background,
        and hands them to the deck when they are ready. Does nothing if the
        deck is empty or no separator is installed. */
    void separateDeckAsync (int deckIndex, std::function<void (bool)> onComplete = {});

    bool isDeckSeparating (int deckIndex) const noexcept
    {
        return juce::isPositiveAndBelow (deckIndex, numDecks)
            && separating[(size_t) deckIndex].load (std::memory_order_relaxed);
    }

    /** How far along a separation is, from 0 to 1. */
    float getSeparationProgress (int deckIndex) const noexcept
    {
        return juce::isPositiveAndBelow (deckIndex, numDecks)
            ? separationProgress[(size_t) deckIndex].load (std::memory_order_relaxed)
            : 0.0f;
    }

    StemSeparator& getStemSeparator() noexcept { return stemSeparator; }

    /** Matches one deck's tempo and beat phase to another. Returns false when
        either deck has no usable beat grid, which is the honest answer for
        material the analyser could not read. */
    bool syncDeck (int followerIndex, int leaderIndex);

    /** The deck a sync should follow, or -1 when there is nothing to follow.

        With two decks this was never a question. With four it is the whole of
        the feature: the answer is the deck that is playing, has a grid, and is
        nearest, which is what somebody pressing sync means by "the other one". */
    int findSyncLeader (int followerIndex) const;

    /** The deck's analysed tempo scaled by its tempo fader, or 0 with no grid. */
    double getEffectiveBpm (int deckIndex) const;

    bool isDeckLoading (int deckIndex) const noexcept
    {
        return juce::isPositiveAndBelow (deckIndex, numDecks)
            && loading[(size_t) deckIndex].load (std::memory_order_relaxed);
    }

    /** Where the master and cue busses go. Changing it takes effect on the
        next block; the audio thread reads it once per block and nothing is
        reopened, so this is safe to call while playing. */
    void setOutputMode (OutputMode mode) noexcept
    {
        outputMode.store (mode, std::memory_order_relaxed);
    }

    OutputMode getOutputMode() const noexcept
    {
        return outputMode.load (std::memory_order_relaxed);
    }

    /** How many outputs the open device is actually using, or 0 with none. */
    int getNumOutputChannels() const noexcept
    {
        return deviceOutputChannels.load (std::memory_order_relaxed);
    }

    /** How many inputs the open device is actually using, or 0 with none. The
        mic has nothing to hear until one is chosen in Audio setup. */
    int getNumInputChannels() const noexcept
    {
        return deviceInputChannels.load (std::memory_order_relaxed);
    }

    /** True when the cue bus can be heard: four outputs on separate pairs, or
        two on a split. */
    bool hasCueOutput() const noexcept
    {
        return cueIsAudible (getOutputMode(), getNumOutputChannels());
    }

    //==========================================================================
    // Driving the engine directly. The device callback is one caller of these
    // and not a privileged one: an offline render, a test, or some future
    // plugin wrapper is another. Keeping the per block work behind a private
    // callback is what let four decks ship with two of them silent, because
    // nothing outside a running sound card could reach it.
    //==========================================================================

    /** Sizes every buffer and prepares the decks, the mixer and the sampler. */
    void prepareToPlay (double sampleRate, int blockSize);

    /** Renders one block of the whole engine into the given device outputs,
        with the device's inputs for the mic when there are any. Realtime safe:
        allocates nothing, locks nothing, opens nothing. */
    void renderNextBlock (float* const* outputs, int numOutputChannels, int numSamples,
                          const float* const* inputs = nullptr, int numInputChannels = 0);

    /** A one line summary of the open device, for the status bar. */
    juce::String getDeviceDescription() const;

    /** How many beats one echo repeat lasts on a deck. The mixer is told the
        length in seconds; this is where that is worked out, because the tempo
        lives on the deck and the mixer has no idea decks exist. A deck with no
        beat grid falls back to half a second, which is a musical guess rather
        than a silent failure. */
    void setEchoBeats (int deckIndex, double beats);
    double getEchoBeats (int deckIndex) const noexcept
    {
        return juce::isPositiveAndBelow (deckIndex, numDecks)
            ? echoBeats[(size_t) deckIndex].load (std::memory_order_relaxed)
            : 1.0;
    }

    /** Records the master output, exactly what the room hears, including the
        crossfader and the master gain. */
    SetRecorder& getRecorder() noexcept { return recorder; }

    /** Sends that same master output to a broadcast server. */
    Broadcaster& getBroadcaster() noexcept { return broadcaster; }

    /** Sends that same master output to an RTMP target, such as YouTube or
        Twitch, through an ffmpeg subprocess. */
    RtmpBroadcaster& getRtmpBroadcaster() noexcept { return rtmpBroadcaster; }

    /** Draws MilkDrop visuals from that same master output. Its frames go
        to the RTMP broadcast by themselves, wired up here once, so a
        broadcast started with live video gets them without the interface
        having to know either end. */
    Visualizer& getVisualizer() noexcept { return visualizer; }

    /** Starts recording at the open device's sample rate. Returns the file, or
        an invalid file with `error` filled in. */
    juce::File startRecording (juce::String& error);

private:
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    void timerCallback() override;

    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;

    std::array<std::unique_ptr<Deck>, numDecks> decks;
    Mixer mixer;

    juce::ThreadPool loaderPool { juce::ThreadPoolOptions{}.withNumberOfThreads (numDecks) };
    std::array<std::atomic<bool>, numDecks> loading {};
    std::atomic<AnalysisCache*> analysisCache { nullptr };

    StemSeparator stemSeparator;
    juce::ThreadPool separatorPool { juce::ThreadPoolOptions{}.withNumberOfThreads (1) };
    std::array<std::atomic<bool>, numDecks> separating {};
    std::array<std::atomic<float>, numDecks> separationProgress {};

    std::array<juce::AudioBuffer<float>, numDecks> deckBuffers;
    juce::AudioBuffer<float> masterBuffer;
    juce::AudioBuffer<float> cueBuffer;

    // The mix to record and broadcast when the mic is kept out of the speakers,
    // and the mic's inputs, copied before the outputs are cleared.
    juce::AudioBuffer<float> recordingBuffer;
    juce::AudioBuffer<float> inputBuffer;

    std::atomic<int> deviceOutputChannels { 0 };
    std::atomic<int> deviceInputChannels { 0 };
    std::atomic<OutputMode> outputMode { OutputMode::separatePairs };
    juce::String deviceChoiceReason;

    Sampler sampler;
    MicInput mic;
    SetRecorder recorder;
    Broadcaster broadcaster;
    RtmpBroadcaster rtmpBroadcaster;
    Visualizer visualizer;

    std::array<std::atomic<double>, numDecks> echoBeats {};

    /** Pushes each deck's beat length into the mixer's echo. Cheap, and done on
        the timer rather than per block: a tempo fader does not move fast enough
        for a fiftieth of a second to matter. */
    void updateEchoTimes();

    /** Asks the open device for a buffer size a DJ can play on, if it is
        sitting on something far larger. Some backends open at a quarter of a
        second, which is unusable however good everything above it is. */
    void tightenBufferSize();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace opendj
