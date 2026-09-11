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

/** Everything needed to reach a broadcast server.

    The password is held in clear text, here and in the settings file. That is
    what every DJ application does and what the user chose, but it is worth
    being honest about rather than pretending otherwise: anything that can read
    the user's profile can read it.
*/
struct BroadcastSettings
{
    juce::String host = "localhost";
    int port = 8000;
    juce::String mount = "/opendj";

    juce::String user = "source";      ///< Icecast's own default
    juce::String password;

    // What listeners and directories see.
    juce::String name = "OpenDJ";
    juce::String description;
    juce::String genre = "DJ mix";
    juce::String url;
    bool isPublic = false;             ///< off by default: listing a stream is a choice

    /** Index into `juce::OggVorbisAudioFormat::getQualityOptions()`. */
    int quality = 5;

    /** The mount with exactly one leading slash, which is what the protocol
        wants and not what people type. */
    juce::String normalisedMount() const;

    /** Empty when these settings could be used, or a sentence saying what is
        missing. Checked before a socket is opened, so a typo is a message
        rather than a timeout. */
    juce::String validate() const;
};

/** The source request a server is sent before any audio.

    Two forms exist. Icecast 2.4 and later prefer HTTP `PUT`; everything older,
    and most Icecast-alikes, take the original `SOURCE` verb. Both are built
    here, as a string, by a function that touches no socket: the handshake is
    the part most likely to be subtly wrong and the part easiest to test if it
    is kept away from the network.
*/
juce::String buildSourceRequest (const BroadcastSettings& settings, bool useHttpPut);

/** The admin request that updates what listeners see as the current track. */
juce::String buildMetadataRequest (const BroadcastSettings& settings, const juce::String& title);

/** A socket carrying one broadcast to one server.

    Nothing here runs on the audio thread. Connecting blocks, writing can block,
    and both are done from the broadcaster's background thread.
*/
class IcecastConnection
{
public:
    IcecastConnection();
    ~IcecastConnection();

    /** Opens the socket and completes the handshake. Returns false with
        `error` filled in, having tried `PUT` first and `SOURCE` after, since a
        server that rejects one usually wants the other. */
    bool connect (const BroadcastSettings& settings, juce::String& error);

    void disconnect();
    bool isConnected() const noexcept { return connected.load (std::memory_order_relaxed); }

    /** Sends encoded audio. Returns false once the connection is gone, which is
        the broadcaster's cue to reconnect. */
    bool send (const void* data, int numBytes);

    juce::int64 getBytesSent() const noexcept { return bytesSent.load (std::memory_order_relaxed); }

    /** Tells the server what is playing. Opens its own short-lived connection,
        because the streaming socket is busy carrying audio. Failure is ignored
        on purpose: a title that did not update is not worth interrupting a
        broadcast over. */
    void sendMetadata (const BroadcastSettings& settings, const juce::String& title);

private:
    /** Reads the response head and decides whether the server accepted us. */
    bool readResponse (juce::String& error);

    std::unique_ptr<juce::StreamingSocket> socket;
    std::atomic<bool> connected { false };
    std::atomic<juce::int64> bytesSent { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IcecastConnection)
};

} // namespace opendj
