/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "analysis/TrackDecoder.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace opendj
{

/** Eight slots holding short sounds, triggered while the decks play.

    A sampler is not a third deck. It has no tempo, no beat grid and no
    crossfader position: a slot is a sound you drop on top of whatever is
    already playing, which is why its output joins the master after the mixer
    rather than passing through a channel.

    The same handover rule as the decks applies. Loading happens on the message
    thread and ends in one atomic store; the sound it displaced is retired and
    freed later by cleanUp(), once the audio thread can no longer be holding it.
*/
class Sampler final
{
public:
    /** Eight, because that is a row of pads on every controller worth mapping,
        the DJ-202 included. */
    static constexpr int numSlots = 8;

    Sampler();
    ~Sampler();

    void prepare (double sampleRate);

    /** Reads a file into a slot. Message thread: it decodes and allocates.
        Returns false with `failureReason` filled in when the file cannot be
        read. Loading over a slot that is playing stops it first. */
    bool loadSlot (int slot, juce::AudioFormatManager& formatManager,
                   const juce::File& file, juce::String* failureReason = nullptr);

    /** Takes an already decoded sound, which is how a background thread hands
        one over: it decodes, and the message thread does nothing but install.
        Message thread. */
    bool installSlot (int slot, std::unique_ptr<DecodedAudio> decoded,
                      const juce::String& name, juce::String* failureReason = nullptr,
                      const juce::File& sourceFile = {});

    /** Empties a slot. Message thread. */
    void clearSlot (int slot);

    bool isSlotLoaded (int slot) const noexcept;
    juce::String getSlotName (int slot) const;

    /** The file a slot was loaded from, so a set of pads can be saved and put
        back. An invalid file means the slot is empty. */
    juce::File getSlotFile (int slot) const;
    double getSlotLengthSeconds (int slot) const noexcept;

    /** Starts a slot from its beginning, whether or not it was already
        playing. Retriggering is the point of a sampler, so this never queues
        and never refuses. Safe from any thread. */
    void trigger (int slot);

    /** Stops a slot, over a few milliseconds so it does not click. */
    void stop (int slot);
    void stopAll();

    bool isSlotPlaying (int slot) const noexcept;

    /** How far into the sound a slot is, in seconds, or 0 when it is idle.
        For the interface; the audio thread writes it once a block. */
    double getSlotPositionSeconds (int slot) const noexcept;

    void setSlotGain (int slot, float gain);
    float getSlotGain (int slot) const noexcept;

    /** A looping slot runs until it is stopped. For a beat or a drone; a
        one-shot stabs once and falls silent. */
    void setSlotLooping (int slot, bool shouldLoop);
    bool isSlotLooping (int slot) const noexcept;

    /** The level of the whole sampler, after the slot gains. */
    void setGain (float gain);
    float getGain() const noexcept { return masterGain.load (std::memory_order_relaxed); }

    /** Whether the sampler is also sent to the headphones, so a slot can be
        found before the room hears it. */
    void setCueEnabled (bool shouldCue) { cueEnabled.store (shouldCue, std::memory_order_relaxed); }
    bool isCueEnabled() const noexcept { return cueEnabled.load (std::memory_order_relaxed); }

    /** Adds every playing slot to master, and to cue when cueing is on. Both
        buffers are stereo and already hold the mixer's output. Audio thread:
        allocates nothing, locks nothing, opens nothing. */
    void processBlock (juce::AudioBuffer<float>& master,
                       juce::AudioBuffer<float>& cue,
                       int numSamples);

    /** Frees sounds displaced by a load. Message thread, on a timer. */
    void cleanUp();

private:
    /** One decoded sound. Immutable once built, which is what lets the audio
        thread read it through a bare pointer with no lock at all. */
    struct Sound
    {
        juce::AudioBuffer<float> audio;
        double sampleRate = 44100.0;
        juce::String name;
        juce::File file;
    };

    struct Slot
    {
        std::atomic<const Sound*> active { nullptr };
        std::shared_ptr<const Sound> owned;              // message thread only

        std::atomic<bool> playing { false };
        std::atomic<bool> looping { false };
        std::atomic<float> gain { 1.0f };
        std::atomic<double> positionSeconds { 0.0 };

        // Bumped by trigger(), compared by the audio thread. A counter rather
        // than a flag so two taps closer together than one block still restart
        // the sound twice, instead of the second being swallowed.
        std::atomic<juce::uint32> triggerCount { 0 };
        std::atomic<bool> stopRequested { false };

        // Audio thread only.
        juce::uint32 seenTriggers = 0;
        double position = 0.0;
        float envelope = 0.0f;
        bool releasing = false;
    };

    std::array<Slot, numSlots> slots;

    std::atomic<float> masterGain { 0.8f };
    std::atomic<bool> cueEnabled { false };
    std::atomic<juce::uint32> blocksProcessed { 0 };

    double deviceSampleRate = 44100.0;
    float envelopeStep = 1.0f;   // per sample, for the few millisecond fades

    // The sound and the block it was displaced at, so cleanUp() knows when
    // the audio thread can no longer be looking at it.
    std::vector<std::pair<std::shared_ptr<const Sound>, juce::uint32>> retired;

    void retire (int slot);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Sampler)
};

} // namespace opendj
