/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>

namespace opendj
{

/** Everything needed to reach an RTMP ingest, for YouTube, Twitch, or anything
    else that speaks the same protocol.

    Unlike Icecast there is no mount point and no separate login: an RTMP
    target is a server URL and a stream key, and the key is the whole of the
    authentication. It is held in clear text, here and in the settings file,
    for exactly the reason `BroadcastSettings::password` is: that is what
    every broadcaster does with a stream key, and saying so beats pretending
    otherwise.
*/
struct RtmpSettings
{
    /** The ingest URL up to but not including the stream key, e.g.
        "rtmp://a.rtmp.youtube.com/live2" or "rtmp://live.twitch.tv/app". Both
        YouTube and Twitch publish theirs on their creator dashboards. */
    juce::String server = "rtmp://a.rtmp.youtube.com/live2";

    juce::String streamKey;

    /** Burned into the static video frame when the broadcast starts. This is
        not the currently playing track: see the RTMP design note in
        ROADMAP.md for why the video is static rather than something that
        would need to say that. */
    juce::String streamTitle = "OpenDJ";

    int videoWidth = 1280;
    int videoHeight = 720;
    int videoBitrateKbps = 2500;
    int audioBitrateKbps = 160;

    /** `server` and `streamKey` joined with exactly one slash, which is what
        ffmpeg wants as its output URL. */
    juce::String fullUrl() const;

    /** Empty when these settings could be used, or a sentence saying what is
        missing. Checked before ffmpeg is started, the same discipline
        `BroadcastSettings::validate` applies to Icecast. */
    juce::String validate() const;
};

/** The ffmpeg command line for one broadcast, as a plain argument list built
    by a function that starts no process. Building it here, away from
    `RtmpConnection`, is the same reasoning `buildSourceRequest` follows for
    Icecast: this is the part most likely to be subtly wrong, and the part
    that can be tested without ffmpeg installed at all.

    The video track is a solid colour with the stream title drawn on it,
    generated entirely by ffmpeg's own `lavfi` colour source and `drawtext`
    filter: nothing in OpenDJ renders or ships an image. That is the
    simplest track that satisfies "needs an H.264 video track", and it is a
    deliberately static one; see the design note in ROADMAP.md before
    reaching for a waveform or the album art.
*/
juce::StringArray buildFfmpegArguments (const RtmpSettings& settings, double sampleRate);

/** One ffmpeg subprocess carrying one broadcast.

    Nothing here runs on the audio thread. Starting the process blocks while
    ffmpeg spins up and opens the RTMP connection, and writing to its stdin
    pipe can block too; both are done from `RtmpBroadcaster`'s background
    thread, exactly as `IcecastConnection` keeps its socket off the audio
    thread.
*/
class RtmpConnection
{
public:
    RtmpConnection();
    ~RtmpConnection();

    /** Looks for an ffmpeg on this machine that can actually encode H.264,
        which `launch` uses and will not spawn anything without: a bare
        "ffmpeg" left to PATH can just as easily resolve to a stripped build
        with no libx264 as a capable one, and there is more than one ffmpeg
        on the machine this was written on for exactly that reason. Checks
        `OPENDJ_RTMP_FFMPEG` first, then a short list of the usual install
        locations, then PATH. Returns the path to use, or empty with
        `diagnostic` filled in when nothing on the machine qualifies. Public
        so a test can exercise the same search `launch` actually runs,
        rather than a second copy of it that could quietly drift. */
    static juce::String findFfmpeg (juce::String& diagnostic);

    /** Starts ffmpeg. Returns false with `error` filled in when no capable
        ffmpeg could be found (see `findFfmpeg`) or the one that was could
        not be started. A true result means the process exists and its
        stdin pipe is open for `send`, nothing more: unlike
        `IcecastConnection::connect`, this is not yet a confirmed connection,
        and there is a real reason it cannot be made one here. See
        `waitForConfirmation`. */
    bool launch (const RtmpSettings& settings, double sampleRate, juce::String& error);

    /** Waits for ffmpeg to either open the RTMP connection or fail outright,
        and must not be called until real audio is already flowing into the
        pipe `send` writes to: see the note below on why. Returns false with
        `error` filled in for an ffmpeg that gave up, whether because the
        target refused it or because it never confirmed within the timeout.

        There is no server response to check the way an HTTP status line
        confirms an Icecast handshake: what is checked instead is ffmpeg's own
        stderr, watched for the line it prints once it has opened its output
        (`Output #0, flv, to ...`). That line is itself the reason this cannot
        be folded into `launch`: ffmpeg's own ffmpeg.c defers opening its
        output, and so printing that line, until it has read a first packet
        from every mapped input, so it can interleave them from a common
        start time. With nothing feeding the audio pipe yet, waiting for that
        line here would simply hang, confirmed by writing this against a real
        ffmpeg: a fifo held open but fed no bytes never produces the line, no
        matter how long the wait. `RtmpBroadcaster` is what arranges for the
        pipe to already have something in it, priming it with a short burst
        of silence before this is ever called, exactly so this ordering
        requirement is met without leaning on the audio thread. */
    bool waitForConfirmation (juce::String& error);

    void disconnect();
    bool isConnected() const noexcept { return connected.load (std::memory_order_relaxed); }

    /** Sends raw interleaved 16-bit PCM to ffmpeg's stdin. Works as soon as
        `launch` has succeeded, whether or not the connection is confirmed
        yet: this is what lets the caller prime the pipe before waiting for
        that confirmation. Returns false once the pipe or the process is
        gone, which is `RtmpBroadcaster`'s cue to reconnect, the same role
        `IcecastConnection::send` plays for a socket that has dropped. */
    bool send (const void* data, int numBytes);

    juce::int64 getBytesSent() const noexcept { return bytesSent.load (std::memory_order_relaxed); }

private:
    /** Platform-specific: a spawned process and the pipe into its stdin.
        Implemented with posix_spawn on Linux and macOS, and CreateProcess on
        Windows. The Windows half is written and compiles but has not been run
        anywhere, in the same spirit the platform table in ROADMAP.md is
        honest about Arch and macOS. */
    class Process;

    std::unique_ptr<Process> process;
    std::atomic<bool> connected { false };
    std::atomic<juce::int64> bytesSent { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RtmpConnection)
};

} // namespace opendj
