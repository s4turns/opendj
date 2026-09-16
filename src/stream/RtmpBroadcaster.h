/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include "stream/RtmpConnection.h"

#include <atomic>
#include <memory>
#include <mutex>

namespace opendj
{

/** Sends the master output to an RTMP target, such as YouTube or Twitch,
    while you play.

    The same shape as `Broadcaster`, deliberately: the audio thread hands a
    block to JUCE's `ThreadedWriter`, which copies it into a FIFO and returns;
    a background thread turns it into 16-bit PCM and writes it to an ffmpeg
    subprocess. Nothing here can stall the audio callback, and ffmpeg falling
    behind costs the broadcast samples rather than costing the room its music,
    the same bargain `Broadcaster` makes with Icecast.

    What differs is what is on the other end of the FIFO. Icecast is a socket
    this process speaks Ogg Vorbis to directly; ffmpeg is a subprocess this
    process speaks raw PCM to over a pipe, because neither JUCE nor OpenDJ can
    produce the H.264 video track and RTMP handshake that YouTube and Twitch
    require, and the roadmap always scoped this as needing ffmpeg as a
    separate program.
*/
class RtmpBroadcaster
{
public:
    /** Same states as `Broadcaster::State`, for the same reason: a target
        that is trying to come back is worth showing differently from one
        that has given up. */
    enum class State
    {
        offline,
        connecting,
        live,
        reconnecting,
        failed
    };

    RtmpBroadcaster();
    ~RtmpBroadcaster();

    //==========================================================================
    // Message thread
    //==========================================================================

    /** Starts ffmpeg and begins encoding. Returns false with `error` filled
        in when ffmpeg could not be started or the target refused it. */
    bool start (const RtmpSettings& settings, double sampleRate, juce::String& error);

    void stop();

    State getState() const noexcept { return state.load (std::memory_order_relaxed); }
    bool isBroadcasting() const noexcept { return getState() != State::offline; }

    /** A sentence for the status bar, including why a broadcast failed. */
    juce::String getStatusMessage() const;

    /** Seconds since the broadcast started, whether or not it is connected at
        this moment: a set is still a set across a reconnect. */
    double getSecondsLive() const;

    juce::int64 getBytesSent() const noexcept;

    /** Samples ffmpeg could not keep up with, missing from what viewers
        heard. Worth showing, for the same reason `Broadcaster` shows its
        own. */
    juce::int64 getDroppedSamples() const noexcept
    {
        return droppedSamples.load (std::memory_order_relaxed);
    }

    /** Frames the video track had to show twice because no new one had
        arrived in time. A few a minute is the visualiser hiccupping and is
        invisible; a steady stream of them means it is not keeping up with
        the rate the broadcast was asked for. */
    juce::int64 getRepeatedFrames() const noexcept
    {
        return repeatedFrames.load (std::memory_order_relaxed);
    }

    //==========================================================================
    // Any thread
    //==========================================================================

    /** Hands in the newest frame for the video track, tightly packed RGB at
        the settings' `videoWidth` by `videoHeight`; a frame of any other
        size is ignored, since it cannot be what ffmpeg was told to expect.
        Copied and returned from at once, so the visualiser's render thread
        is never held up by the network. Only the newest frame is kept: if
        two arrive before the feeder sends one, the first is simply never
        seen, which for a picture is the right thing.

        Does nothing when the broadcast is not running with live video, so
        the visualiser can push unconditionally. */
    void pushVideoFrame (const unsigned char* rgb, int numBytes);

    //==========================================================================
    // Audio thread
    //==========================================================================

    /** Hands one block to the encoder. Realtime safe: a copy into a FIFO and
        nothing else. */
    void write (const juce::AudioBuffer<float>& master, int numSamples);

private:
    /** Turns float blocks into interleaved 16-bit PCM and hands them to
        ffmpeg's stdin. The `AudioFormatWriter` seam is what lets this reuse
        JUCE's `ThreadedWriter` unchanged, exactly as `Broadcaster` reuses it
        around the Ogg Vorbis encoder: the FIFO and the background thread are
        not this file's to reinvent. */
    class PcmWriter;

    /** Sends the newest pushed frame down the video pipe `fps` times a
        second of wall time, whether or not a new one has arrived, so the
        video track's clock advances in step with the audio track's however
        the visualiser is doing. Starts by sending black, which is also what
        gets the connection confirmed: ffmpeg will not open its output until
        every input has produced a packet, and this is the video input's
        first one. */
    class VideoFeeder;

    /** Reconnects with a backoff, so a dropped connection comes back on its
        own without anybody watching the screen. Same shape as
        `Broadcaster::Reconnector`, and the same reasoning: a reconnect
        relaunches ffmpeg from scratch rather than resuming it, because FLV
        carries its header at the front of the stream the same way Ogg does,
        and a server joining halfway through one has nothing to decode. */
    class Reconnector;

    void openWriter (double sampleRate);
    void closeWriter();

    void openVideoFeeder();
    void closeVideoFeeder();

    /** True once either pipe's writer has given up on ffmpeg, which is the
        reconnector's cue. */
    bool anyPipeHasFailed();

    /** Pushes a short burst of silence through the FIFO right after the
        writer opens, on whichever thread is doing the connecting rather than
        the audio thread. This exists for one reason: ffmpeg will not confirm
        the RTMP connection until real audio has started reaching its stdin
        pipe (see `RtmpConnection::waitForConfirmation`), and nothing
        guarantees the audio device's callback has fired even once by the
        time a broadcast is started, or immediately after a reconnect. A
        quarter second of silence is inaudible and is not something a real
        broadcast would ever need to lean on once the audio thread is already
        feeding blocks in, but it is what makes the connection able to
        confirm at all rather than gambling on timing. */
    void primeWithSilence (double sampleRate);

    // Shared rather than unique so a write in flight on the background thread
    // keeps the connection alive even if the broadcast is stopped underneath
    // it, exactly as `Broadcaster::connection` does for Icecast.
    std::shared_ptr<RtmpConnection> connection;
    PcmWriter* pcm = nullptr;                      // owned by the encoder, watched by the reconnector
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    juce::TimeSliceThread encoderThread { "OpenDJ RTMP broadcast" };

    // Swapped in one store, the way the recorder and `Broadcaster` both do
    // it, so the audio thread never holds a writer the message thread is
    // taking apart.
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> activeWriter { nullptr };
    std::mutex writerMutex;

    std::unique_ptr<VideoFeeder> videoFeeder;
    std::mutex videoFeederMutex;

    RtmpSettings settings;
    std::atomic<State> state { State::offline };
    std::atomic<juce::int64> droppedSamples { 0 };
    std::atomic<juce::int64> repeatedFrames { 0 };
    std::atomic<juce::int64> startedAtMs { 0 };

    juce::String failureReason;
    std::unique_ptr<Reconnector> reconnector;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RtmpBroadcaster)
};

} // namespace opendj
