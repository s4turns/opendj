/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "core/Deck.h"
#include "core/Mixer.h"

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

    /** Opens the default device. Returns an error string, empty on success. */
    juce::String initialise();

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
    juce::AudioFormatManager& getFormatManager() noexcept { return formatManager; }

    Deck& getDeck (int deckIndex) noexcept { return *decks[(size_t) deckIndex]; }
    Mixer& getMixer() noexcept { return mixer; }

    /** Decodes and analyses a file on a background thread, then swaps it onto
        the deck. Decoding a long track and finding its beat grid takes a second
        or two, which is far too long to spend on the message thread. The
        callback runs on the message thread. */
    void loadTrackAsync (int deckIndex, const juce::File& file,
                         std::function<void (bool)> onComplete = {});

    /** Matches one deck's tempo and beat phase to the other. Returns false when
        either deck has no usable beat grid, which is the honest answer for
        material the analyser could not read. */
    bool syncDeck (int followerIndex, int leaderIndex);

    /** The deck's analysed tempo scaled by its tempo fader, or 0 with no grid. */
    double getEffectiveBpm (int deckIndex) const;

    bool isDeckLoading (int deckIndex) const noexcept
    {
        return juce::isPositiveAndBelow (deckIndex, numDecks)
            && loading[(size_t) deckIndex].load (std::memory_order_relaxed);
    }

    /** True when the open device has a second output pair for the cue bus. */
    bool hasCueOutput() const noexcept { return cueOutputAvailable.load (std::memory_order_relaxed); }

    /** A one line summary of the open device, for the status bar. */
    juce::String getDeviceDescription() const;

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

    std::array<juce::AudioBuffer<float>, numDecks> deckBuffers;
    juce::AudioBuffer<float> masterBuffer;
    juce::AudioBuffer<float> cueBuffer;

    std::atomic<bool> cueOutputAvailable { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace opendj
