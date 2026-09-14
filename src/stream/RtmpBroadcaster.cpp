/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "stream/RtmpBroadcaster.h"

#include <cstdint>
#include <vector>

namespace opendj
{

namespace
{
    // Ten seconds at 48 kHz: a good deal more than `Broadcaster` gives its
    // Icecast FIFO, and deliberately so. Icecast is a socket connect; this is
    // a whole ffmpeg process, and its first keyframe and the RTMP handshake
    // both compete for the same CPU the rest of the machine is using.
    // Measured directly against a real ffmpeg on a loaded box: a two to
    // four second stall right after startup is ordinary, not a fault, and a
    // FIFO sized for a network hiccup was not enough to survive one.
    constexpr int fifoSamples = 480000;

    // Ramps the same way `Broadcaster`'s does: 1, 2, 4 seconds and so on up to
    // the cap. A reconnect here is heavier, a whole ffmpeg process rather than
    // a socket, but a viewer waiting for a stream to come back is in no more
    // of a hurry than an Icecast listener is.
    constexpr int firstRetryMs = 1000;
    constexpr int longestRetryMs = 30000;
}

//==============================================================================

/** Turns float blocks into interleaved 16-bit PCM and hands them to ffmpeg's
    stdin.

    Samples arrive here scaled to the full 32-bit range regardless of the
    16 bits this writer declares, which is `AudioFormatWriter::write`'s own
    contract; the top 16 bits of each is the sample this format actually
    wants, so the conversion is a shift, not a rescale. */
class RtmpBroadcaster::PcmWriter final : public juce::AudioFormatWriter
{
public:
    PcmWriter (RtmpConnection& connectionToUse, double writerSampleRate)
        : AudioFormatWriter (nullptr, "raw PCM for ffmpeg", writerSampleRate, 2u, 16u),
          connection (connectionToUse)
    {
    }

    bool write (const int** samplesToWrite, int numSamples) override
    {
        if (numSamples <= 0)
            return true;

        scratch.resize ((size_t) numSamples * 2);

        for (int i = 0; i < numSamples; ++i)
        {
            scratch[(size_t) i * 2 + 0] = (int16_t) (samplesToWrite[0][i] >> 16);
            scratch[(size_t) i * 2 + 1] = (int16_t) (samplesToWrite[1][i] >> 16);
        }

        if (! connection.send (scratch.data(), (int) (scratch.size() * sizeof (int16_t))))
        {
            failed = true;
            return false;
        }

        return true;
    }

    bool hasFailed() const noexcept { return failed; }

private:
    RtmpConnection& connection;
    std::vector<int16_t> scratch;
    bool failed = false;
};

//==============================================================================

/** Watches the connection and puts it back when it drops. See
    `Broadcaster::Reconnector`: this is the same class with ffmpeg in place of
    a socket. */
class RtmpBroadcaster::Reconnector final : public juce::Thread
{
public:
    Reconnector (RtmpBroadcaster& ownerToUse, double rate)
        : juce::Thread ("OpenDJ RTMP reconnect"), owner (ownerToUse), sampleRate (rate) {}

    void run() override
    {
        auto waitMs = firstRetryMs;

        while (! threadShouldExit())
        {
            if (owner.state.load (std::memory_order_relaxed) == RtmpBroadcaster::State::live)
            {
                std::lock_guard<std::mutex> lock (owner.writerMutex);

                if (owner.pcm == nullptr || ! owner.pcm->hasFailed())
                {
                    waitMs = firstRetryMs;
                    lockFreeWait (200);
                    continue;
                }
            }

            if (threadShouldExit())
                return;

            owner.state.store (RtmpBroadcaster::State::reconnecting, std::memory_order_relaxed);
            owner.closeWriter();
            owner.connection->disconnect();

            lockFreeWait (waitMs);

            if (threadShouldExit())
                return;

            juce::String error;
            bool confirmed = false;

            if (owner.connection->launch (owner.settings, sampleRate, error))
            {
                owner.openWriter (sampleRate);
                owner.primeWithSilence (sampleRate);
                confirmed = owner.connection->waitForConfirmation (error);
            }

            if (confirmed)
            {
                owner.state.store (RtmpBroadcaster::State::live, std::memory_order_relaxed);
                waitMs = firstRetryMs;
            }
            else
            {
                owner.failureReason = error;
                owner.closeWriter();
                owner.connection->disconnect();
                waitMs = juce::jmin (longestRetryMs, waitMs * 2);
            }
        }
    }

private:
    /** Sleeps in short pieces so stopping is quick even mid-backoff. */
    void lockFreeWait (int totalMs)
    {
        for (auto left = totalMs; left > 0 && ! threadShouldExit(); left -= 100)
            wait (juce::jmin (100, left));
    }

    RtmpBroadcaster& owner;
    double sampleRate;
};

//==============================================================================

RtmpBroadcaster::RtmpBroadcaster() = default;

RtmpBroadcaster::~RtmpBroadcaster() { stop(); }

bool RtmpBroadcaster::start (const RtmpSettings& settingsToUse, double sampleRate, juce::String& error)
{
    stop();

    settings = settingsToUse;
    failureReason.clear();
    droppedSamples.store (0, std::memory_order_relaxed);
    state.store (State::connecting, std::memory_order_relaxed);

    connection = std::make_shared<RtmpConnection>();

    if (! connection->launch (settings, sampleRate, error))
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
        // stop() ends by setting state back to offline, on the reasoning that
        // a broadcast which is not running is not failed either, just off.
        // That reasoning does not hold here: this IS the failure, and setting
        // state before calling stop() would only have it overwritten a moment
        // later. stop() runs first, purely for its cleanup, and the failure
        // is recorded once it is done clobbering things.
        stop();
        error = "ffmpeg started, but the writer could not be created.";
        failureReason = error;
        state.store (State::failed, std::memory_order_relaxed);
        return false;
    }

    // ffmpeg will not confirm the connection until audio is already reaching
    // it; see `RtmpConnection::waitForConfirmation` for why, and
    // `primeWithSilence` for what this does about it.
    primeWithSilence (sampleRate);

    if (! connection->waitForConfirmation (error))
    {
        // Same ordering as above, and for the same reason: stop() would
        // otherwise reset state to offline immediately after this sets it to
        // failed, and a caller checking getState() right after start()
        // returns false would see the wrong one.
        stop();
        failureReason = error;
        state.store (State::failed, std::memory_order_relaxed);
        return false;
    }

    startedAtMs.store (juce::Time::currentTimeMillis(), std::memory_order_relaxed);
    state.store (State::live, std::memory_order_relaxed);

    reconnector = std::make_unique<Reconnector> (*this, sampleRate);
    reconnector->startThread (juce::Thread::Priority::low);

    return true;
}

void RtmpBroadcaster::stop()
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

void RtmpBroadcaster::openWriter (double sampleRate)
{
    std::lock_guard<std::mutex> lock (writerMutex);

    auto* encoder = new PcmWriter (*connection, sampleRate);
    pcm = encoder;

    writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (encoder, encoderThread, fifoSamples);
    activeWriter.store (writer.get(), std::memory_order_release);
}

void RtmpBroadcaster::primeWithSilence (double sampleRate)
{
    auto* target = activeWriter.load (std::memory_order_acquire);

    if (target == nullptr)
        return;

    const auto numSamples = juce::jmax (1, (int) (sampleRate * 0.25));
    juce::AudioBuffer<float> silence (2, numSamples);
    silence.clear();

    target->write (silence.getArrayOfReadPointers(), numSamples);
}

void RtmpBroadcaster::closeWriter()
{
    // Taken away from the audio thread first, then destroyed, exactly as
    // `Broadcaster::closeWriter` does it: the store is what makes the
    // handover safe, and the lock only keeps two message-thread callers from
    // tripping over each other.
    activeWriter.store (nullptr, std::memory_order_release);

    std::lock_guard<std::mutex> lock (writerMutex);
    writer.reset();
    pcm = nullptr;
}

//==============================================================================

void RtmpBroadcaster::write (const juce::AudioBuffer<float>& master, int numSamples)
{
    auto* target = activeWriter.load (std::memory_order_acquire);

    if (target == nullptr || numSamples <= 0)
        return;

    // The same bargain `Broadcaster::write` makes: a full FIFO costs the
    // broadcast those samples and costs the room nothing.
    if (! target->write (master.getArrayOfReadPointers(), numSamples))
        droppedSamples.fetch_add (numSamples, std::memory_order_relaxed);
}

//==============================================================================

double RtmpBroadcaster::getSecondsLive() const
{
    const auto started = startedAtMs.load (std::memory_order_relaxed);

    return started == 0 ? 0.0 : (double) (juce::Time::currentTimeMillis() - started) / 1000.0;
}

juce::int64 RtmpBroadcaster::getBytesSent() const noexcept
{
    return connection != nullptr ? connection->getBytesSent() : 0;
}

juce::String RtmpBroadcaster::getStatusMessage() const
{
    switch (getState())
    {
        case State::connecting:
            return "Connecting to " + settings.server + "...";

        case State::live:
        {
            const auto seconds = (int) getSecondsLive();
            juce::String text;
            text << "Live on " << settings.server << "  |  " << juce::String (seconds / 60) << ":"
                 << juce::String (seconds % 60).paddedLeft ('0', 2);

            if (getDroppedSamples() > 0)
                text << "  |  dropped audio";

            return text;
        }

        case State::reconnecting:
            return "Lost ffmpeg's connection, trying again"
                 + (failureReason.isEmpty() ? juce::String() : ": " + failureReason);

        case State::failed:
            return "RTMP broadcast failed: " + failureReason;

        case State::offline:
        default:
            return {};
    }
}

} // namespace opendj
