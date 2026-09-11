/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "analysis/AnalysisCache.h"
#include "analysis/Stems.h"
#include "analysis/TrackAnalysis.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace RubberBand { class RubberBandStretcher; }

namespace opendj
{

/** One playback deck.

    Tracks are decoded to memory in full when they load. That costs roughly
    10 MB per stereo minute, which is a price worth paying: it means the audio
    thread does random access reads at a fractional position with no disk, no
    read-ahead buffer and no lock, which is exactly what instant hot cues and
    jog wheel scratching need.

    The audio thread only ever reads an atomic pointer to a finished Track.
    Loading builds the replacement on a background thread and publishes it with
    one store; the displaced track is retired and freed later by cleanUp(), once
    the audio thread has demonstrably moved past it.
*/
class Deck
{
public:
    Deck (int deckIndex, juce::AudioFormatManager& formatManagerToUse);
    ~Deck();

    int getIndex() const noexcept { return index; }

    //==========================================================================
    // Message thread
    //==========================================================================

    /** Decodes a file and swaps it in. Returns false if it could not be read,
        leaving whatever was loaded before untouched. Pass what the library
        already knows to skip the tempo pass and use the tagged title. */
    bool loadFile (const juce::File& file, const KnownTrack* known = nullptr);

    void unload();

    /** Frees tracks the audio thread has finished with. Call periodically from
        a timer on the message thread. */
    void cleanUp();

    juce::File getLoadedFile() const;
    juce::String getTrackTitle() const;

    /** Why the last loadFile() returned false, in words a user can act on.
        Written by whichever thread did the loading and read by the interface
        afterwards, so it takes a lock; it is never touched by the audio thread. */
    juce::String getLastLoadError() const;

    /** Waveform peaks and the beat grid for the loaded track, or null when
        nothing is loaded. Safe to call from the interface: the returned object
        is immutable and keeps itself alive for as long as the caller holds it. */
    std::shared_ptr<const TrackAnalysis> getAnalysis() const;

    //==========================================================================
    // Transport, callable from any thread
    //==========================================================================

    void play();
    void pause();
    void togglePlay();

    /** Press and release of the cue button, following the familiar behaviour:
        while stopped, pressing sets the cue point and previews from it, and
        releasing jumps back and stops. While playing, pressing returns to the
        cue point and stops there. */
    void cuePressed();
    void cueReleased();

    void seekToSeconds (double seconds);
    void seekToFraction (double proportion);

    /** 1.0 plays at the recorded speed. Pitch follows tempo unless key lock is on. */
    void setTempoRatio (double ratio);
    double getTempoRatio() const noexcept { return tempoRatio.load (std::memory_order_relaxed); }

    /** With key lock on, a tempo change goes through a time stretcher and the
        pitch stays where the record put it. It is bypassed while a hand is on
        the platter: a stretcher cannot follow a scratch, and nobody expects a
        scratch to be in key. */
    void setKeyLock (bool shouldLock);
    void toggleKeyLock();
    bool isKeyLockEnabled() const noexcept { return keyLock.load (std::memory_order_relaxed); }

    void setTrim (float linearGain);
    float getTrim() const noexcept { return trimGain.load (std::memory_order_relaxed); }

    //==========================================================================
    // Stems
    //==========================================================================

    /** Hands the deck a separated copy of the track it is already playing.
        Ignored if it does not match what is loaded, so a separation that
        finishes after the deck moved on is discarded rather than played. */
    void setSeparation (std::shared_ptr<const SeparatedTrack> separation, const juce::File& forFile);

    bool hasStems() const noexcept { return activeSeparation.load (std::memory_order_acquire) != nullptr; }

    /** 1 is the stem as recorded, 0 removes it. Takes effect on the next block,
        smoothed, so a pad does not click. */
    void setStemGain (Stem stem, float gain);
    float getStemGain (Stem stem) const;

    //==========================================================================
    // Hot cues
    //==========================================================================

    static constexpr int numHotCues = 8;

    /** Sets the hot cue to the current position if it is empty, or jumps to it
        if it is not. This is the one-button behaviour pads want. */
    void hotCuePressed (int slot);
    void clearHotCue (int slot);
    double getHotCueSeconds (int slot) const;
    bool hasHotCue (int slot) const;

    //==========================================================================
    // Loops
    //==========================================================================

    /** Sets the start of a loop at the playhead, leaving the end alone. */
    void setLoopIn();

    /** Sets the end at the playhead and starts looping, which is what pressing
        loop out on any piece of DJ gear does. */
    void setLoopOut();

    /** A loop of a given number of beats starting from the beat at or behind
        the playhead, so it lands in time rather than wherever the finger did.
        Returns false when the track has no beat grid to snap to, because an
        auto loop without one is just a guess. */
    bool setLoopBeats (double beats);

    /** Halving and doubling keep the start where it is, so a loop can be closed
        in on a bar without it wandering off the beat. */
    void halveLoop();
    void doubleLoop();

    void setLoopEnabled (bool shouldLoop);
    void toggleLoop();
    void clearLoop();

    /** Jumps back to the start of the last loop and starts it again. */
    void reloop();

    bool hasLoop() const noexcept;
    bool isLoopEnabled() const noexcept { return loopEnabled.load (std::memory_order_relaxed); }
    double getLoopStartSeconds() const noexcept { return loopStartSeconds.load (std::memory_order_relaxed); }
    double getLoopEndSeconds() const noexcept   { return loopEndSeconds.load (std::memory_order_relaxed); }

    /** The length in beats of the last automatic loop, or 0 for one set by hand. */
    double getLoopBeats() const noexcept { return loopBeats.load (std::memory_order_relaxed); }

    /** A momentary loop. While it is held the deck repeats, and underneath the
        track keeps running, so letting go drops you where you would have been
        rather than where the loop left you. That is the whole difference
        between a roll and a loop, and it is why a roll can be used mid-phrase
        without losing the mix. */
    bool beginLoopRoll (double beats);
    void endLoopRoll();
    bool isLoopRolling() const noexcept { return rollActive.load (std::memory_order_relaxed); }

    //==========================================================================
    // Jog wheel
    //==========================================================================

    /** A hand on the platter. While touched the wheel drives playback outright:
        the transport rate comes from the ticks, so the track stops when the hand
        stops and runs backwards when the hand does. */
    void setJogTouched (bool touched);
    bool isJogTouched() const noexcept { return jogTouched.load (std::memory_order_relaxed); }

    /** Relative movement since the last call, in controller ticks. Positive is
        forwards. Accumulates until the audio thread consumes it. */
    void addJogTicks (double ticks);

    /** How many ticks the controller reports for one full turn of the platter.
        The DJ-202 reports 512. */
    void setJogTicksPerRevolution (int ticks);

    //==========================================================================
    // State for the UI
    //==========================================================================

    bool isLoaded() const noexcept  { return activeTrack.load (std::memory_order_acquire) != nullptr; }
    bool isPlaying() const noexcept { return playing.load (std::memory_order_relaxed); }
    double getPositionSeconds() const noexcept { return positionSeconds.load (std::memory_order_relaxed); }
    double getLengthSeconds() const noexcept   { return lengthSeconds.load (std::memory_order_relaxed); }
    double getCueSeconds() const noexcept      { return cuePointSeconds.load (std::memory_order_relaxed); }

    /** Peak level of the last block, for meters. Reading clears the hold. */
    float readAndResetPeak() noexcept;

    //==========================================================================
    // Audio thread
    //==========================================================================

    void prepare (double sampleRate, int blockSize);
    void releaseResources();

    /** Renders this deck into a stereo buffer, replacing its contents. */
    void processBlock (juce::AudioBuffer<float>& destination);

private:
    /** One decoded file. Immutable once published. */
    struct Track
    {
        juce::File file;
        juce::String title;
        juce::AudioBuffer<float> audio;   // always two channels
        double sampleRate = 44100.0;
        juce::uint32 retiredAtBlock = 0;
    };

    void publish (std::unique_ptr<Track> newTrack);

    const int index;
    juce::AudioFormatManager& formatManager;

    mutable std::mutex loadErrorMutex;
    juce::String lastLoadError;

    std::atomic<Track*> activeTrack { nullptr };

    // Stems live beside the track rather than inside it: a separation arrives
    // long after the track it belongs to, and swapping it in must not disturb a
    // deck that is already playing.
    //
    // Published the same way a track is, by storing one raw pointer. An
    // atomic<shared_ptr> would be the obvious thing and is exactly wrong here:
    // the standard library implements it with a spin lock, and the audio thread
    // must not wait on one. The owning pointer stays on the message thread and
    // the displaced one is retired until the audio thread has moved past it.
    std::atomic<const SeparatedTrack*> activeSeparation { nullptr };
    std::shared_ptr<const SeparatedTrack> ownedSeparation;
    std::vector<std::pair<std::shared_ptr<const SeparatedTrack>, juce::uint32>> retiredSeparations;

    std::array<std::atomic<float>, numStems> stemGains;
    std::array<juce::SmoothedValue<float>, numStems> stemGainRamps;   // audio thread
    std::atomic<std::shared_ptr<const TrackAnalysis>> analysisData;
    std::unique_ptr<Track> owned;                       // the live track
    std::vector<std::unique_ptr<Track>> retired;        // message thread only

    std::atomic<bool> playing { false };
    std::atomic<double> tempoRatio { 1.0 };
    std::atomic<double> cuePointSeconds { 0.0 };
    std::atomic<double> positionSeconds { 0.0 };
    std::atomic<double> lengthSeconds { 0.0 };
    std::atomic<float> trimGain { 1.0f };
    std::atomic<float> peakLevel { 0.0f };
    std::atomic<juce::uint32> blocksProcessed { 0 };

    // Seek requests cross to the audio thread as one atomic; negative means none.
    std::atomic<double> pendingSeekSeconds { -1.0 };

    // True while the cue button is previewing, so releasing it stops playback.
    std::atomic<bool> previewingFromCue { false };

    std::array<std::atomic<double>, numHotCues> hotCues;   // negative means unset

    // Loop bounds are worked out on the message thread, where the beat grid can
    // be read, and cross to the audio thread as plain numbers. The audio thread
    // never touches the analysis: reading a shared pointer there would mean
    // taking a reference count, and that is not a realtime operation.
    std::atomic<double> loopStartSeconds { -1.0 };
    std::atomic<double> loopEndSeconds { -1.0 };
    std::atomic<double> loopBeats { 0.0 };                 // 0 when set by hand
    std::atomic<bool> loopEnabled { false };
    std::atomic<bool> rollActive { false };

    // Where the track would have been if the roll had never happened.
    double rollReturnPosition = 0.0;                       // audio thread, file samples
    bool rollReturnValid = false;                          // audio thread only

    /** Wraps a read head back to the loop start when it runs past the end.
        Returns true when it moved. Audio thread only. */
    bool wrapIntoLoop (double& position, double rate, const Track& track) const;

    std::atomic<bool> jogTouched { false };
    std::atomic<double> jogTicks { 0.0 };
    std::atomic<int> jogTicksPerRevolution { 512 };
    double pitchBend = 0.0;                                // audio thread only
    double scratchTarget = 0.0;                            // audio thread, in file samples
    bool scratchTargetValid = false;                       // audio thread only

    // Key lock. The stretcher is built in prepare(), so the audio thread only
    // ever uses it and never allocates it. It is fed from feedPosition, which
    // runs ahead of the audible readPosition by the stretcher's own delay.
    std::atomic<bool> keyLock { false };
    std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;
    juce::AudioBuffer<float> stretchInput;                 // one chunk fed to the stretcher
    juce::AudioBuffer<float> stretchDiscard;               // where its warm-up output goes
    double feedPosition = 0.0;                             // audio thread, in file samples
    bool stretchPrimed = false;                            // audio thread only

    void renderStretched (const Track& track, juce::AudioBuffer<float>& destination,
                          double rate, double fileToDevice,
                          const SeparatedTrack* separation, const float* stemGains);
    void feedStretcher (const Track& track, double fileToDevice,
                        const SeparatedTrack* separation, const float* stemGains);

    double readPosition = 0.0;                          // audio thread, in file samples
    double deviceSampleRate = 44100.0;
    juce::SmoothedValue<float> transportGain { 0.0f };  // ramps to kill play/pause clicks

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Deck)
};

} // namespace opendj
