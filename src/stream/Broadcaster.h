/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include "stream/IcecastConnection.h"

#include <atomic>
#include <memory>
#include <mutex>

namespace opendj

{

/** Sends the master output to an Icecast server while you play.

    The same shape as `SetRecorder`, and for the same reason. The audio thread
    hands a block to JUCE's `ThreadedWriter`, which copies it into a FIFO and
    returns; a background thread does the Vorbis encoding and the socket write.
    Nothing here can stall the audio callback, and a network that stalls costs
    the broadcast samples rather than costing the room its music.

    That trade is deliberate and it is the whole design: the people in the room
    paid to be there, and the stream is the thing that gives way.
*/
class Broadcaster
{
public:
    /** What the interface shows. A broadcast that is trying to come back is
        worth distinguishing from one that has given up. */
    enum class State
    {
        offline,
        connecting,
        live,
        reconnecting,
        failed
    };

    Broadcaster();
    ~Broadcaster();

    //==========================================================================
    // Message thread
    //==========================================================================

    /** Connects and starts encoding. Returns false with `error` filled in when
        the server refused or could not be reached. */
    bool start (const BroadcastSettings& settings, double sampleRate, juce::String& error);

    void stop();

    State getState() const noexcept { return state.load (std::memory_order_relaxed); }
    bool isBroadcasting() const noexcept { return getState() != State::offline; }

    /** A sentence for the status bar, including why a broadcast failed. */
    juce::String getStatusMessage() const;

    /** Seconds since the broadcast started, whether or not it is connected at
        this moment: a set is still a set across a reconnect. */
    double getSecondsLive() const;

    juce::int64 getBytesSent() const noexcept;

    /** Samples the network could not keep up with, missing from what listeners
        heard. Worth showing, for the same reason the recorder shows its own. */
    juce::int64 getDroppedSamples() const noexcept
    {
        return droppedSamples.load (std::memory_order_relaxed);
    }

    /** Tells listeners what is playing. Does nothing when offline, and nothing
        on an Ogg mount either: see `IcecastConnection::sendMetadata`. */
    void noteTrack (const juce::String& title);

    //==========================================================================
    // Audio thread
    //==========================================================================

    /** Hands one block to the encoder. Realtime safe: a copy into a FIFO and
        nothing else. */
    void write (const juce::AudioBuffer<float>& master, int numSamples);

private:
    /** Where the encoder's bytes go: straight at the socket, and on to the
        reconnect logic when that socket has gone. */
    class ConnectionStream;

    /** Reconnects with a backoff, so a dropped connection comes back on its own
        without anybody watching the screen. */
    class Reconnector;

    void openWriter (double sampleRate);
    void closeWriter();

    juce::AudioFormatManager formats;

    // Shared rather than unique so a metadata send in flight keeps the
    // connection alive even if the broadcast is stopped underneath it.
    std::shared_ptr<IcecastConnection> connection;
    ConnectionStream* stream = nullptr;            // owned by the encoder
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    juce::TimeSliceThread encoderThread { "OpenDJ broadcast" };

    // Swapped in one store, the way the recorder does it, so the audio thread
    // never holds a writer that the message thread is taking apart.
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> activeWriter { nullptr };
    std::mutex writerMutex;

    BroadcastSettings settings;
    std::atomic<State> state { State::offline };
    std::atomic<juce::int64> droppedSamples { 0 };
    std::atomic<juce::int64> startedAtMs { 0 };

    juce::String failureReason;
    std::unique_ptr<Reconnector> reconnector;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Broadcaster)
};

} // namespace opendj
