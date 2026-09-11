/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "stream/Broadcaster.h"

namespace opendj
{

namespace
{
    // Two seconds at 48 kHz. Larger than the recorder's, because a network
    // hiccup lasts longer than a disk one and there is no reason to drop
    // samples that a moment's patience would have carried.
    constexpr int fifoSamples = 96000;

    // Long enough to be worth waiting for, short enough to be back before a
    // track ends. Doubles up to the cap after every failure.
    constexpr int firstRetryMs = 1000;
    constexpr int longestRetryMs = 30000;
}

//==============================================================================

/** The encoder writes here, and this writes to the socket.

    An `OutputStream` is the seam JUCE's Ogg writer gives us, and a socket is a
    perfectly good thing to put behind one. When the socket goes, this says so
    once and stops pretending, which is what the reconnector watches for.
*/
class Broadcaster::ConnectionStream final : public juce::OutputStream
{
public:
    explicit ConnectionStream (IcecastConnection& connectionToUse) : connection (connectionToUse) {}

    bool write (const void* data, size_t numBytes) override
    {
        if (numBytes == 0)
            return true;

        if (! connection.send (data, (int) numBytes))
        {
            failed = true;
            return false;
        }

        return true;
    }

    bool hasFailed() const noexcept { return failed; }

    void flush() override {}
    juce::int64 getPosition() override { return connection.getBytesSent(); }
    bool setPosition (juce::int64) override { return false; }   // a stream, not a file

private:
    IcecastConnection& connection;
    bool failed = false;
};

//==============================================================================

/** Watches the connection and puts it back when it drops.

    A reconnect is not a resume. Ogg carries its headers at the front of the
    stream, so a server that joins halfway through one has nothing to decode.
    The writer is therefore rebuilt from scratch every time, which is also why
    the audio thread must see the writer disappear rather than see a stale one.
*/
class Broadcaster::Reconnector final : public juce::Thread
{
public:
    Reconnector (Broadcaster& ownerToUse, double rate)
        : juce::Thread ("OpenDJ broadcast reconnect"), owner (ownerToUse), sampleRate (rate) {}

    void run() override
    {
        auto waitMs = firstRetryMs;

        while (! threadShouldExit())
        {
            // Nothing to do while the stream is healthy.
            if (owner.state.load (std::memory_order_relaxed) == Broadcaster::State::live)
            {
                std::lock_guard<std::mutex> lock (owner.writerMutex);

                if (owner.stream == nullptr || ! owner.stream->hasFailed())
                {
                    waitMs = firstRetryMs;
                    lock_free_wait (200);
                    continue;
                }
            }

            if (threadShouldExit())
                return;

            owner.state.store (Broadcaster::State::reconnecting, std::memory_order_relaxed);
            owner.closeWriter();
            owner.connection->disconnect();

            lock_free_wait (waitMs);

            if (threadShouldExit())
                return;

            juce::String error;

            if (owner.connection->connect (owner.settings, error))
            {
                owner.openWriter (sampleRate);
                owner.state.store (Broadcaster::State::live, std::memory_order_relaxed);
                waitMs = firstRetryMs;
            }
            else
            {
                owner.failureReason = error;
                waitMs = juce::jmin (longestRetryMs, waitMs * 2);
            }
        }
    }

private:
    /** Sleeps in short pieces so stopping is quick even mid-backoff. */
    void lock_free_wait (int totalMs)
    {
        for (auto left = totalMs; left > 0 && ! threadShouldExit(); left -= 100)
            wait (juce::jmin (100, left));
    }

    Broadcaster& owner;
    double sampleRate;
};

//==============================================================================

Broadcaster::Broadcaster() = default;

Broadcaster::~Broadcaster() { stop(); }

bool Broadcaster::start (const BroadcastSettings& settingsToUse, double sampleRate, juce::String& error)
{
    stop();

    settings = settingsToUse;
    failureReason.clear();
    droppedSamples.store (0, std::memory_order_relaxed);
    state.store (State::connecting, std::memory_order_relaxed);

    connection = std::make_shared<IcecastConnection>();

    if (! connection->connect (settings, error))
    {
        failureReason = error;
        state.store (State::failed, std::memory_order_relaxed);
        connection.reset();
        return false;
    }

    encoderThread.startThread (juce::Thread::Priority::normal);
    openWriter (sampleRate);

    if (activeWriter.load (std::memory_order_acquire) == nullptr)
    {
        error = "The Ogg Vorbis encoder could not be started.";
        failureReason = error;
        stop();
        return false;
    }

    startedAtMs.store (juce::Time::currentTimeMillis(), std::memory_order_relaxed);
    state.store (State::live, std::memory_order_relaxed);

    reconnector = std::make_unique<Reconnector> (*this, sampleRate);
    reconnector->startThread (juce::Thread::Priority::low);

    return true;
}

void Broadcaster::stop()
{
    if (reconnector != nullptr)
    {
        reconnector->stopThread (2000);
        reconnector.reset();
    }

    closeWriter();
    encoderThread.stopThread (2000);

    if (connection != nullptr)
    {
        connection->disconnect();
        connection.reset();
    }

    state.store (State::offline, std::memory_order_relaxed);
    startedAtMs.store (0, std::memory_order_relaxed);
}

void Broadcaster::openWriter (double sampleRate)
{
    std::lock_guard<std::mutex> lock (writerMutex);

    auto ownedStream = std::make_unique<ConnectionStream> (*connection);
    stream = ownedStream.get();

    juce::OggVorbisAudioFormat ogg;

    // The encoder takes the stream, and the threaded writer takes the encoder.
    // Sixteen bits is the interface, not the quality: Vorbis is lossy, and the
    // quality index is what decides the bitrate.
    std::unique_ptr<juce::OutputStream> asStream (std::move (ownedStream));

    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate (sampleRate)
                             .withNumChannels (2)
                             .withBitsPerSample (16)
                             .withQualityOptionIndex (juce::jlimit (0, 10, settings.quality));

    auto* encoder = ogg.createWriterFor (asStream, options).release();

    if (encoder == nullptr)
    {
        stream = nullptr;
        return;
    }

    writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (encoder, encoderThread,
                                                                       fifoSamples);
    activeWriter.store (writer.get(), std::memory_order_release);
}

void Broadcaster::closeWriter()
{
    // Taken away from the audio thread first, then destroyed. The store is what
    // makes the handover safe; the lock only keeps two message-thread callers
    // from tripping over each other.
    activeWriter.store (nullptr, std::memory_order_release);

    std::lock_guard<std::mutex> lock (writerMutex);
    writer.reset();
    stream = nullptr;
}

//==============================================================================

void Broadcaster::write (const juce::AudioBuffer<float>& master, int numSamples)
{
    auto* target = activeWriter.load (std::memory_order_acquire);

    if (target == nullptr || numSamples <= 0)
        return;

    // Exactly the recorder's bargain: a full FIFO costs the stream those samples
    // and costs the room nothing.
    if (! target->write (master.getArrayOfReadPointers(), numSamples))
        droppedSamples.fetch_add (numSamples, std::memory_order_relaxed);
}

//==============================================================================

double Broadcaster::getSecondsLive() const
{
    const auto started = startedAtMs.load (std::memory_order_relaxed);

    return started == 0 ? 0.0 : (double) (juce::Time::currentTimeMillis() - started) / 1000.0;
}

juce::int64 Broadcaster::getBytesSent() const noexcept
{
    return connection != nullptr ? connection->getBytesSent() : 0;
}

juce::String Broadcaster::getStatusMessage() const
{
    switch (getState())
    {
        case State::connecting:
            return "Connecting to " + settings.host + "...";

        case State::live:
        {
            const auto seconds = (int) getSecondsLive();
            juce::String text;
            text << "Streaming to " << settings.host << settings.normalisedMount()
                 << "  |  " << juce::String (seconds / 60) << ":"
                 << juce::String (seconds % 60).paddedLeft ('0', 2);

            if (getDroppedSamples() > 0)
                text << "  |  dropped audio";

            return text;
        }

        case State::reconnecting:
            return "Lost the server, trying again"
                 + (failureReason.isEmpty() ? juce::String() : ": " + failureReason);

        case State::failed:
            return "Broadcast failed: " + failureReason;

        case State::offline:
        default:
            return {};
    }
}

void Broadcaster::noteTrack (const juce::String& title)
{
    if (! isBroadcasting() || connection == nullptr || title.isEmpty())
        return;

    // On a background thread: this opens a second socket, and the caller is the
    // message thread in the middle of loading a track.
    const auto settingsCopy = settings;

    juce::Thread::launch ([held = connection, settingsCopy, title]
    {
        held->sendMetadata (settingsCopy, title);
    });
}

} // namespace opendj
