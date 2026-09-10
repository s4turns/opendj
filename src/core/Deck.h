/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "analysis/TrackAnalysis.h"

#include <atomic>
#include <memory>
#include <vector>

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
        leaving whatever was loaded before untouched. */
    bool loadFile (const juce::File& file);

    void unload();

    /** Frees tracks the audio thread has finished with. Call periodically from
        a timer on the message thread. */
    void cleanUp();

    juce::File getLoadedFile() const;
    juce::String getTrackTitle() const;

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

    /** 1.0 plays at the recorded speed. Pitch follows tempo for now; key lock
        arrives with the time stretcher. */
    void setTempoRatio (double ratio);
    double getTempoRatio() const noexcept { return tempoRatio.load (std::memory_order_relaxed); }

    void setTrim (float linearGain);
    float getTrim() const noexcept { return trimGain.load (std::memory_order_relaxed); }

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

    std::atomic<Track*> activeTrack { nullptr };
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

    double readPosition = 0.0;                          // audio thread, in file samples
    double deviceSampleRate = 44100.0;
    juce::SmoothedValue<float> transportGain { 0.0f };  // ramps to kill play/pause clicks

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Deck)
};

} // namespace opendj
