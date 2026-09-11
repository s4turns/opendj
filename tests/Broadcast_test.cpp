/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>

#include "stream/Broadcaster.h"
#include "stream/IcecastConnection.h"

#include <cmath>

using opendj::BroadcastSettings;
using opendj::Broadcaster;

namespace
{
    BroadcastSettings usable()
    {
        BroadcastSettings settings;
        settings.host = "radio.example";
        settings.port = 8000;
        settings.mount = "/opendj";
        settings.user = "source";
        settings.password = "hackme";
        settings.name = "Test stream";
        settings.genre = "Techno";
        return settings;
    }

    /** A server that accepts one connection, answers, and remembers what it was
        sent. Enough to prove the handshake and that audio followed it, with no
        Icecast anywhere. */
    struct FakeServer final : private juce::Thread
    {
        FakeServer() : juce::Thread ("fake icecast")
        {
            // Port 0 asks the system for a free one, which is what lets these
            // tests run anywhere without colliding with something.
            REQUIRE (listener.createListener (0, "127.0.0.1"));
            port = listener.getBoundPort();
            REQUIRE (port > 0);
            startThread();
        }

        ~FakeServer() override
        {
            stopThread (2000);
            listener.close();
        }

        void run() override
        {
            std::unique_ptr<juce::StreamingSocket> client (listener.waitForNextConnection());

            if (client == nullptr)
                return;

            char buffer[4096];

            while (! threadShouldExit() && client->isConnected())
            {
                if (client->waitUntilReady (true, 200) <= 0)
                    continue;

                const auto read = client->read (buffer, (int) sizeof (buffer), false);

                if (read <= 0)
                    break;

                const std::lock_guard<std::mutex> lock (mutex);
                received.append (buffer, (size_t) read);

                // Answered once the request head is complete, exactly as a
                // server does before any audio is allowed to flow.
                if (! answered && asText().contains ("\r\n\r\n"))
                {
                    answered = true;
                    const char* ok = "HTTP/1.1 200 OK\r\n\r\n";
                    client->write (ok, (int) strlen (ok));
                }
            }
        }

        juce::String asText() const
        {
            return { juce::CharPointer_UTF8 ((const char*) received.getData()), received.getSize() };
        }

        juce::String textSoFar()
        {
            const std::lock_guard<std::mutex> lock (mutex);
            return asText();
        }

        /** Searched over the raw bytes, not over a string. Ogg is binary, and
            reading it as text mangles exactly the parts worth looking for. */
        bool sawBytes (const char* needle)
        {
            const std::lock_guard<std::mutex> lock (mutex);

            const auto* data = (const char*) received.getData();
            const auto length = received.getSize();
            const auto wanted = strlen (needle);

            for (size_t i = 0; i + wanted <= length; ++i)
                if (memcmp (data + i, needle, wanted) == 0)
                    return true;

            return false;
        }

        size_t bytesReceived()
        {
            const std::lock_guard<std::mutex> lock (mutex);
            return received.getSize();
        }

        juce::StreamingSocket listener;
        juce::MemoryBlock received;
        std::mutex mutex;
        bool answered = false;
        int port = 0;
    };

    /** Waits for something to become true, rather than sleeping and hoping. */
    bool waitFor (std::function<bool()> condition, int timeoutMs = 5000)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (juce::Time::getMillisecondCounter() < deadline)
        {
            if (condition())
                return true;

            juce::Thread::sleep (25);
        }

        return false;
    }
}

//==============================================================================
// The handshake, with no network anywhere near it.

TEST_CASE ("the PUT request says who we are and what we are sending", "[broadcast]")
{
    const auto request = opendj::buildSourceRequest (usable(), true);

    REQUIRE (request.startsWith ("PUT /opendj HTTP/1.1\r\n"));
    REQUIRE (request.contains ("Host: radio.example:8000\r\n"));
    REQUIRE (request.contains ("Content-Type: audio/ogg\r\n"));
    REQUIRE (request.contains ("Ice-Name: Test stream\r\n"));
    REQUIRE (request.contains ("Ice-Genre: Techno\r\n"));
    REQUIRE (request.contains ("Ice-Public: 0\r\n"));
    REQUIRE (request.contains ("Expect: 100-continue\r\n"));
    REQUIRE (request.endsWith ("\r\n\r\n"));
}

TEST_CASE ("the password is sent as basic authentication", "[broadcast]")
{
    const auto request = opendj::buildSourceRequest (usable(), true);

    // source:hackme, base64. Checked against the value rather than by encoding
    // it the same way twice, which would pass even if both were wrong.
    REQUIRE (request.contains ("Authorization: Basic c291cmNlOmhhY2ttZQ==\r\n"));
}

TEST_CASE ("the older SOURCE form is available for older servers", "[broadcast]")
{
    const auto request = opendj::buildSourceRequest (usable(), false);

    REQUIRE (request.startsWith ("SOURCE /opendj HTTP/1.0\r\n"));
    REQUIRE (request.contains ("Authorization: Basic "));

    // Nothing to expect from a server that does not speak HTTP/1.1.
    REQUIRE (! request.contains ("Expect:"));
}

TEST_CASE ("a mount typed without a slash still works", "[broadcast]")
{
    auto settings = usable();
    settings.mount = "opendj";

    REQUIRE (settings.normalisedMount() == "/opendj");
    REQUIRE (opendj::buildSourceRequest (settings, true).startsWith ("PUT /opendj "));
}

TEST_CASE ("empty stream details are left out rather than sent empty", "[broadcast]")
{
    auto settings = usable();
    settings.genre = {};
    settings.description = {};

    const auto request = opendj::buildSourceRequest (settings, true);

    // An empty Ice-Genre is worse than none: some servers publish it verbatim.
    REQUIRE (! request.contains ("Ice-Genre:"));
    REQUIRE (! request.contains ("Ice-Description:"));
    REQUIRE (request.contains ("Ice-Name: Test stream"));
}

TEST_CASE ("settings that cannot work are refused before a socket is opened", "[broadcast]")
{
    REQUIRE (usable().validate().isEmpty());

    auto noPassword = usable();
    noPassword.password = {};
    REQUIRE (noPassword.validate().isNotEmpty());

    auto noHost = usable();
    noHost.host = "   ";
    REQUIRE (noHost.validate().isNotEmpty());

    auto badPort = usable();
    badPort.port = 0;
    REQUIRE (badPort.validate().isNotEmpty());

    auto noMount = usable();
    noMount.mount = "/";
    REQUIRE (noMount.validate().isNotEmpty());
}

TEST_CASE ("a track title reaches the metadata request intact", "[broadcast]")
{
    const auto request = opendj::buildMetadataRequest (usable(), "Alpha & Omega - Sound Æffect");

    REQUIRE (request.startsWith ("GET /admin/metadata?"));
    REQUIRE (request.contains ("mode=updinfo"));
    REQUIRE (request.contains ("Authorization: Basic "));

    // The characters that would otherwise end the query string early, or mean
    // something else once they got there.
    REQUIRE (! request.upToFirstOccurrenceOf ("\r\n", false, false).contains (" - Sound"));
    REQUIRE ((request.contains ("%26") || request.contains ("%20")));
}

//==============================================================================
// The whole path, against a server that only exists for the test.

TEST_CASE ("a broadcast connects, and audio follows the request", "[broadcast][network]")
{
    FakeServer server;

    auto settings = usable();
    settings.host = "127.0.0.1";
    settings.port = server.port;

    Broadcaster broadcaster;
    juce::String error;

    REQUIRE (broadcaster.start (settings, 48000.0, error));
    REQUIRE (error.isEmpty());
    REQUIRE (broadcaster.getState() == Broadcaster::State::live);

    // The request has to have arrived before any audio does.
    REQUIRE (waitFor ([&server] { return server.textSoFar().contains ("\r\n\r\n"); }));
    REQUIRE (server.textSoFar().startsWith ("PUT /opendj HTTP/1.1"));

    const auto headBytes = server.bytesReceived();

    // Five seconds of tone, handed over the way the audio thread hands it over.
    // Fewer would prove nothing: Vorbis fills an Ogg page before it emits one,
    // and a pure tone compresses so well that a second of it is not a page.
    juce::AudioBuffer<float> block (2, 512);
    double phase = 0.0;

    for (int i = 0; i < 470; ++i)
    {
        for (int sample = 0; sample < block.getNumSamples(); ++sample)
        {
            const auto value = (float) std::sin (phase) * 0.5f;
            phase += juce::MathConstants<double>::twoPi * 440.0 / 48000.0;

            for (int ch = 0; ch < 2; ++ch)
                block.setSample (ch, sample, value);
        }

        broadcaster.write (block, block.getNumSamples());

        // Paced, because the audio thread is paced. Pushing five seconds of
        // audio in one go would overrun a FIFO that exists to absorb a network
        // hiccup, not a caller with no sense of time.
        if (i % 4 == 3)
            juce::Thread::sleep (1);
    }

    // Encoded on a background thread, so the bytes arrive shortly afterwards.
    REQUIRE (waitFor ([&server, headBytes] { return server.bytesReceived() > headBytes + 2000; }));
    REQUIRE (broadcaster.getDroppedSamples() == 0);
    REQUIRE (broadcaster.getState() == Broadcaster::State::live);

    REQUIRE (server.sawBytes ("OggS"));     // the Ogg page marker, so it is a real stream
    REQUIRE (server.sawBytes ("vorbis"));   // and the codec headers went out first

    broadcaster.stop();
    REQUIRE (broadcaster.getState() == Broadcaster::State::offline);
}

TEST_CASE ("a server that is not there fails with a reason, not a hang", "[broadcast][network]")
{
    auto settings = usable();
    settings.host = "127.0.0.1";
    settings.port = 1;      // nothing listens here

    Broadcaster broadcaster;
    juce::String error;

    REQUIRE (! broadcaster.start (settings, 48000.0, error));
    REQUIRE (error.isNotEmpty());
    REQUIRE (broadcaster.getState() == Broadcaster::State::failed);
    REQUIRE (broadcaster.getStatusMessage().contains (error));
}

TEST_CASE ("writing while offline is harmless", "[broadcast]")
{
    Broadcaster broadcaster;
    juce::AudioBuffer<float> block (2, 256);
    block.clear();

    // The audio thread calls this every block whether or not anybody is
    // broadcasting, so it has to be free and silent when nobody is.
    broadcaster.write (block, block.getNumSamples());

    REQUIRE (broadcaster.getState() == Broadcaster::State::offline);
    REQUIRE (broadcaster.getDroppedSamples() == 0);
    REQUIRE (broadcaster.getBytesSent() == 0);
    REQUIRE (broadcaster.getStatusMessage().isEmpty());
}

//==============================================================================

/** Against a real server, which no automated run can assume exists.

    Hidden by the leading dot in the tag, so a normal run skips it. To use one:

        docker run -d --name icecast -p 8010:8000 \
            -e ICECAST_SOURCE_PASSWORD=opendjtest libretime/icecast:latest

        OPENDJ_ICECAST_HOST=127.0.0.1 OPENDJ_ICECAST_PORT=8010 \
        OPENDJ_ICECAST_PASSWORD=opendjtest opendj-tests "[.live]"

    The loopback test above proves the protocol against something that always
    answers correctly. This proves it against something that answers the way
    Icecast really does, which is the part worth knowing.
*/
TEST_CASE ("a real server accepts the broadcast", "[.live]")
{
    const auto host = juce::SystemStats::getEnvironmentVariable ("OPENDJ_ICECAST_HOST", {});
    const auto password = juce::SystemStats::getEnvironmentVariable ("OPENDJ_ICECAST_PASSWORD", {});

    if (host.isEmpty() || password.isEmpty())
    {
        WARN ("Set OPENDJ_ICECAST_HOST and OPENDJ_ICECAST_PASSWORD to run this.");
        return;
    }

    BroadcastSettings settings;
    settings.host = host;
    settings.port = juce::SystemStats::getEnvironmentVariable ("OPENDJ_ICECAST_PORT", "8000").getIntValue();
    settings.mount = juce::SystemStats::getEnvironmentVariable ("OPENDJ_ICECAST_MOUNT", "/opendj");
    settings.password = password;
    settings.name = "OpenDJ test";
    settings.genre = "Techno";

    Broadcaster broadcaster;
    juce::String error;

    INFO ("connecting to " << settings.host << ":" << settings.port << settings.normalisedMount());
    REQUIRE (broadcaster.start (settings, 48000.0, error));
    REQUIRE (error.isEmpty());

    // Ten seconds, paced, so a listener could actually join and hear it.
    juce::AudioBuffer<float> block (2, 512);
    double phase = 0.0;

    for (int i = 0; i < 940; ++i)
    {
        for (int sample = 0; sample < block.getNumSamples(); ++sample)
        {
            const auto value = (float) std::sin (phase) * 0.4f;
            phase += juce::MathConstants<double>::twoPi * 440.0 / 48000.0;

            for (int ch = 0; ch < 2; ++ch)
                block.setSample (ch, sample, value);
        }

        broadcaster.write (block, block.getNumSamples());

        if (i % 4 == 3)
            juce::Thread::sleep (10);
    }

    broadcaster.noteTrack ("OpenDJ - Test Tone");
    juce::Thread::sleep (500);

    REQUIRE (broadcaster.getState() == Broadcaster::State::live);
    REQUIRE (broadcaster.getBytesSent() > 10000);
    REQUIRE (broadcaster.getDroppedSamples() == 0);

    broadcaster.stop();
}
