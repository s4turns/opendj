/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>

#include "stream/RtmpBroadcaster.h"
#include "stream/RtmpConnection.h"

#include <cmath>
#include <cstdlib>
#include <vector>

using opendj::RtmpSettings;
using opendj::RtmpBroadcaster;

namespace
{
    RtmpSettings usable()
    {
        RtmpSettings settings;
        settings.server = "rtmp://a.rtmp.youtube.com/live2";
        settings.streamKey = "abcd-efgh-ijkl-mnop";
        settings.streamTitle = "OpenDJ live";
        return settings;
    }

    /** The same search `RtmpConnection::launch` actually runs, not a second
        copy of it: a test that checked bare "ffmpeg" on PATH while the real
        code looks harder than that would drift the moment a machine has more
        than one ffmpeg installed, which is exactly the situation this search
        exists to handle. `diagnostic` is why, if it came back empty; the
        tests below WARN with it and skip, the same judgement call the
        Icecast `[.live]` test makes about a server that is not there. */
    juce::String findCapableFfmpeg (juce::String& diagnostic)
    {
        return opendj::RtmpConnection::findFfmpeg (diagnostic);
    }

    /** Waits for something to become true, rather than sleeping and hoping.
        The same helper `Broadcast_test.cpp` uses. */
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
// ffmpeg's stderr, with a stand-in for ffmpeg so no real one is needed.

#if ! JUCE_WINDOWS

/** ffmpeg prints warnings for as long as it runs. When nothing read them once
    the broadcast was live, its stderr pipe filled, ffmpeg blocked on that write
    and stopped reading its stdin, and `send` waited on a reader that was
    waiting on it. That was the loopback test's stall on about half of runs,
    and it depended on how much ffmpeg happened to print.

    This makes the flood certain rather than likely: a shell script that
    answers the capability checks like ffmpeg, confirms the connection, prints
    megabytes of warnings, and only then reads its stdin. POSIX only, because
    only the POSIX side gives ffmpeg a stderr pipe to fill. */
TEST_CASE ("a broadcast keeps flowing while ffmpeg floods its stderr", "[rtmp]")
{
    const auto script = juce::File::createTempFile (".sh");

    // Put back however the test ends, a failed assertion included, so a
    // stand-in is never left on disk or in the environment for later tests.
    struct Restore
    {
        juce::File file;
        juce::String previous { juce::SystemStats::getEnvironmentVariable ("OPENDJ_RTMP_FFMPEG", {}) };

        ~Restore()
        {
            if (previous.isNotEmpty())
                ::setenv ("OPENDJ_RTMP_FFMPEG", previous.toRawUTF8(), 1);
            else
                ::unsetenv ("OPENDJ_RTMP_FFMPEG");

            file.deleteFile();
        }
    } restore { script };

    // Unix line endings. JUCE writes "\r\n" by default, and "#!/bin/sh\r" is
    // not an interpreter the kernel can find.
    REQUIRE (script.replaceWithText (
        "#!/bin/sh\n"
        "case \"$*\" in\n"
        "  *-encoders*) echo ' V....D libx264              stand-in'; exit 0 ;;\n"
        "  *-version*)  echo 'ffmpeg version stand-in'; exit 0 ;;\n"
        "esac\n"
        "echo \"Output #0, flv, to 'rtmp://stand-in':\" >&2\n"
        // Some audio is read before the flood starts, so the flood arrives
        // once the broadcast is running, which is when nothing used to read
        // stderr. Flooding straight away would be soaked up by the wait for
        // confirmation instead, and the test would pass either way.
        "head -c 100000 > /dev/null\n"
        "i=0\n"
        "while [ $i -lt 5000 ]; do\n"
        "  echo '[flv @ 0x0] Non-monotonic DTS; previous 1, current 0; changing to 2. "
        "This may result in incorrect timestamps in the output file.' >&2\n"
        "  i=$((i + 1))\n"
        "done\n"
        "exec cat > /dev/null\n", false, false, "\n"));
    REQUIRE (script.setExecutePermission (true));

    ::setenv ("OPENDJ_RTMP_FFMPEG", script.getFullPathName().toRawUTF8(), 1);

    // The search has to pick the stand-in. Falling through to a real ffmpeg
    // would test something else entirely and fail for an unrelated reason.
    juce::String diagnostic;
    REQUIRE (opendj::RtmpConnection::findFfmpeg (diagnostic) == script.getFullPathName());

    {
        opendj::RtmpConnection connection;
        juce::String error;

        REQUIRE (connection.launch (usable(), 48000.0, error));

        const auto confirmed = connection.waitForConfirmation (error);
        INFO (error);
        REQUIRE (confirmed);

        // Three seconds of 48 kHz stereo 16-bit, far more than one pipe
        // buffer, while the script is still printing. Unfixed, the first write
        // to block sat out the whole ten second send timeout and failed.
        std::vector<char> chunk (4096, 0);
        const int chunks = 3 * 48000 * 2 * 2 / (int) chunk.size();
        const auto startedMs = juce::Time::getMillisecondCounter();
        auto allSent = true;

        for (int i = 0; i < chunks && allSent; ++i)
            allSent = connection.send (chunk.data(), (int) chunk.size());

        const auto elapsedMs = juce::Time::getMillisecondCounter() - startedMs;

        REQUIRE (allSent);
        REQUIRE (connection.isConnected());
        REQUIRE (elapsedMs < 5000);
    }
}

#endif

//==============================================================================
// The ffmpeg command line, and the settings it is built from, with no process
// anywhere near either.

TEST_CASE ("the server and key join into one ffmpeg output URL", "[rtmp]")
{
    auto settings = usable();
    REQUIRE (settings.fullUrl() == "rtmp://a.rtmp.youtube.com/live2/abcd-efgh-ijkl-mnop");

    // A trailing slash on the server or a leading one on the key is a typo
    // either platform's dashboard practically invites, and both are absorbed
    // rather than turned into a URL with two slashes in the middle.
    settings.server = "rtmp://a.rtmp.youtube.com/live2/";
    settings.streamKey = "/abcd-efgh-ijkl-mnop";
    REQUIRE (settings.fullUrl() == "rtmp://a.rtmp.youtube.com/live2/abcd-efgh-ijkl-mnop");
}

TEST_CASE ("settings that cannot work are refused before ffmpeg is started", "[rtmp]")
{
    REQUIRE (usable().validate().isEmpty());

    auto noServer = usable();
    noServer.server = "   ";
    REQUIRE (noServer.validate().isNotEmpty());

    auto badScheme = usable();
    badScheme.server = "http://a.rtmp.youtube.com/live2";
    REQUIRE (badScheme.validate().isNotEmpty());

    auto noKey = usable();
    noKey.streamKey = "";
    REQUIRE (noKey.validate().isNotEmpty());

    // rtmps is just as valid as rtmp: some targets, and some ffmpeg builds,
    // want the TLS form.
    auto tls = usable();
    tls.server = "rtmps://a.rtmp.youtube.com/live2";
    REQUIRE (tls.validate().isEmpty());
}

TEST_CASE ("the ffmpeg arguments carry both tracks and the target URL", "[rtmp]")
{
    const auto args = opendj::buildFfmpegArguments (usable(), 48000.0);
    const auto joined = args.joinIntoString (" ");

    // The audio track: raw PCM on stdin, at the device's own rate.
    REQUIRE (args.contains ("pipe:0"));
    REQUIRE (joined.contains ("-ar 48000"));

    // Unprobed, or ffmpeg waits for five seconds of audio before it connects.
    REQUIRE (joined.contains ("-analyzeduration 0 -probesize 32 -i pipe:0"));
    REQUIRE (joined.contains ("-ac 2"));

    // The video track: ffmpeg's own colour source and text filter, not a
    // file OpenDJ rendered and shipped.
    REQUIRE (joined.contains ("lavfi"));
    REQUIRE (joined.contains ("color=c="));
    REQUIRE (joined.contains ("drawtext"));
    REQUIRE (joined.contains ("OpenDJ live"));

   #if JUCE_WINDOWS
    // Without one, ffmpeg's Windows builds crash in drawtext rather than
    // failing with a message, so the font is always named there.
    REQUIRE (joined.contains ("fontfile='"));
   #endif

    // Both encoders, and where it is all going.
    REQUIRE (args.contains ("libx264"));
    REQUIRE (args.contains ("aac"));
    REQUIRE (args.contains (usable().fullUrl()));
}

TEST_CASE ("a title with filter-breaking characters is cleaned rather than sent broken", "[rtmp]")
{
    auto settings = usable();
    settings.streamTitle = "Set: 'Live' 100% [take 2]";

    const auto args = opendj::buildFfmpegArguments (settings, 44100.0);
    const auto joined = args.joinIntoString (" ");

    // The exact replacement is not the point; not breaking ffmpeg's filter
    // syntax is. None of the characters that would end the expression early
    // should reach it.
    REQUIRE (! joined.contains ("Set:"));
    REQUIRE (! joined.contains ("100%"));
    REQUIRE (! joined.contains ("[take"));
    REQUIRE (joined.contains ("drawtext"));
}

TEST_CASE ("a blank title falls back to something rather than an empty filter", "[rtmp]")
{
    auto settings = usable();
    settings.streamTitle = "   ";

    const auto args = opendj::buildFfmpegArguments (settings, 44100.0);
    REQUIRE (args.joinIntoString (" ").contains ("text='OpenDJ'"));
}

TEST_CASE ("writing while offline is harmless", "[rtmp]")
{
    RtmpBroadcaster broadcaster;
    juce::AudioBuffer<float> block (2, 256);
    block.clear();

    // The audio thread calls this every block whether or not anybody is
    // broadcasting, so it has to be free and silent when nobody is.
    broadcaster.write (block, block.getNumSamples());

    REQUIRE (broadcaster.getState() == RtmpBroadcaster::State::offline);
    REQUIRE (broadcaster.getDroppedSamples() == 0);
    REQUIRE (broadcaster.getBytesSent() == 0);
    REQUIRE (broadcaster.getStatusMessage().isEmpty());
}

//==============================================================================
// Against a real ffmpeg subprocess, but not a real YouTube or Twitch: a
// second ffmpeg stands in as the RTMP receiver, on loopback, the same way
// `Broadcast_test.cpp`'s `FakeServer` stands in for Icecast.

TEST_CASE ("ffmpeg that cannot be reached fails with a reason, not a hang", "[rtmp][network]")
{
    juce::String diagnostic;

    if (findCapableFfmpeg (diagnostic).isEmpty())
    {
        WARN (diagnostic);
        return;
    }

    RtmpSettings settings;
    settings.server = "rtmp://127.0.0.1:1/live";   // nothing listens on port 1
    settings.streamKey = "test";

    RtmpBroadcaster broadcaster;
    juce::String error;

    REQUIRE (! broadcaster.start (settings, 48000.0, error));
    REQUIRE (error.isNotEmpty());
    REQUIRE (broadcaster.getState() == RtmpBroadcaster::State::failed);
    REQUIRE (broadcaster.getStatusMessage().contains (error));
}

/** Needs no account, only a capable ffmpeg, so it runs by default and skips
    with a warning where there is none. It was hidden for a while because it
    failed most runs, and that turned out to be three separate faults, each
    real outside the test too: ffmpeg's stderr filling up with nobody reading
    it, ffmpeg probing five seconds of audio before connecting, and
    `-shortest` holding back every stream while audio ran ahead of the
    video. See the broadcasting notes in ROADMAP.md.

        opendj-tests "[rtmp-loopback]" */
TEST_CASE ("a broadcast reaches ffmpeg's own receiver with a video track and an audio track",
          "[rtmp-loopback][rtmp][network]")
{
    juce::String diagnostic;

    if (findCapableFfmpeg (diagnostic).isEmpty())
    {
        WARN (diagnostic);
        return;
    }

    // A random high port, not port 0: ffmpeg's own `-listen` has to be told
    // what to bind, so nothing here can ask the system for a free one the
    // way `Broadcast_test.cpp`'s loopback socket does.
    const auto port = 19000 + (int) juce::Random::getSystemRandom().nextInt (10000);
    const auto url = "rtmp://127.0.0.1:" + juce::String (port) + "/live/test";

    const auto outFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("opendj-rtmp-test-"
                                           + juce::String (juce::Random::getSystemRandom().nextInt())
                                           + ".flv");
    outFile.deleteFile();

    juce::ChildProcess receiver;
    const juce::StringArray receiverArgs { "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                                           "-listen", "1", "-f", "flv", "-i", url,
                                           "-c", "copy", outFile.getFullPathName() };
    REQUIRE (receiver.start (receiverArgs));

    // Stopped however the test ends. A failed assertion leaves the scope
    // early, and a receiver still listening after that holds the test runner's
    // output pipe open, so the run never reports that it failed.
    struct StopOnExit
    {
        juce::ChildProcess& process;
        ~StopOnExit() { if (process.isRunning()) process.kill(); }
    } stopReceiver { receiver };

    // Given a moment to bind and start listening before the broadcaster
    // tries to reach it, the same allowance the manual protocol check this
    // test is modelled on needed.
    juce::Thread::sleep (700);

    RtmpSettings settings;
    settings.server = "rtmp://127.0.0.1:" + juce::String (port) + "/live";
    settings.streamKey = "test";
    settings.streamTitle = "OpenDJ test";
    settings.videoWidth = 320;
    settings.videoHeight = 180;
    settings.videoBitrateKbps = 400;
    settings.audioBitrateKbps = 96;

    RtmpBroadcaster broadcaster;
    juce::String error;
    const auto started = broadcaster.start (settings, 48000.0, error);

    INFO (error);
    REQUIRE (started);
    REQUIRE (error.isEmpty());
    REQUIRE (broadcaster.getState() == RtmpBroadcaster::State::live);

    // Eight seconds of tone, handed over the way the audio thread hands it
    // over, paced the same way `Broadcast_test.cpp` paces the Icecast one.
    const int totalBlocks = 750;
    juce::AudioBuffer<float> block (2, 512);
    double phase = 0.0;

    for (int i = 0; i < totalBlocks; ++i)
    {
        for (int sample = 0; sample < block.getNumSamples(); ++sample)
        {
            const auto value = (float) std::sin (phase) * 0.5f;
            phase += juce::MathConstants<double>::twoPi * 440.0 / 48000.0;

            for (int ch = 0; ch < 2; ++ch)
                block.setSample (ch, sample, value);
        }

        broadcaster.write (block, block.getNumSamples());

        if (i % 4 == 3)
            juce::Thread::sleep (1);
    }

    // Handing blocks to `write` is not the same as ffmpeg having them: the
    // video track is paced to real time on purpose, a live stream cannot be
    // sent faster than real time, and this loop above just handed over eight
    // seconds of audio in a fraction of a second. What actually drains the
    // FIFO into ffmpeg's pipe is the background thread, gated by ffmpeg's own
    // real-time pacing, so this waits for the evidence that it has, the same
    // way `Broadcast_test.cpp` waits for bytes to reach its `FakeServer`
    // rather than assuming the write loop already delivered them.
    //
    // A working run gets there in about fifteen seconds, most of it libx264
    // waiting for its lookahead frames at two a second. Sixty is generous
    // without letting a real stall run on for minutes.
    const auto pcmBytesWritten = (juce::int64) totalBlocks * block.getNumSamples() * 2 /* channels */ * 2 /* bytes per sample */;
    REQUIRE (waitFor ([&]
    {
        return broadcaster.getBytesSent() >= pcmBytesWritten * 9 / 10;
    }, 60000));

    INFO (broadcaster.getStatusMessage());
    REQUIRE (broadcaster.getBytesSent() > 0);
    REQUIRE (broadcaster.getState() == RtmpBroadcaster::State::live);

    // Not a zero-drop assertion, unlike the Icecast loopback test: that test
    // stands against a `FakeServer` in this same process with nothing else
    // to wait on, where any drop at all would be this project's own bug.
    // ffmpeg is a real external process, and its first keyframe and its own
    // RTMP handshake genuinely compete for CPU with whatever else the
    // machine is doing at that moment; a startup stall long enough to lose
    // some of eight seconds of audio into a ten second FIFO is the cost that
    // was measured directly, not a hypothetical. What has to hold is the
    // thing this test actually exists to prove: that most of the audio got
    // through, the connection is still live, and what arrived decodes as
    // real H.264 and AAC, checked below.
    const auto totalSamplesWritten = (juce::int64) totalBlocks * block.getNumSamples();
    REQUIRE (broadcaster.getDroppedSamples() < totalSamplesWritten / 2);

    broadcaster.stop();

    // Closing our end of ffmpeg's stdin is its cue to finish encoding and
    // exit, which drops the RTMP connection and is the receiver's own cue to
    // finish its file.
    REQUIRE (receiver.waitForProcessToFinish (8000));

    REQUIRE (outFile.existsAsFile());
    REQUIRE (outFile.getSize() > 1000);

    // What ffmpeg itself thinks is in the file it just wrote, rather than
    // parsing FLV by hand: a video stream, in H.264, and an audio stream, in
    // AAC, which is exactly what the README says YouTube, Twitch and
    // Mixcloud require and Ogg over Icecast cannot provide.
    juce::ChildProcess probe;
    const juce::StringArray probeArgs { "ffprobe", "-v", "error", "-show_entries",
                                        "stream=codec_type,codec_name", "-of", "csv=p=0",
                                        outFile.getFullPathName() };
    REQUIRE (probe.start (probeArgs));
    const auto probeOutput = probe.readAllProcessOutput();

    INFO (probeOutput);
    REQUIRE (probeOutput.containsIgnoreCase ("video"));
    REQUIRE (probeOutput.containsIgnoreCase ("h264"));
    REQUIRE (probeOutput.containsIgnoreCase ("audio"));
    REQUIRE (probeOutput.containsIgnoreCase ("aac"));

    outFile.deleteFile();
}

//==============================================================================

/** Against a real target, which no automated run can assume exists, let
    alone one this project has any way to reach: neither YouTube nor Twitch
    can be tested against in this repository's CI, or by the author, because
    both require a live creator account and a key that identifies it.

    Hidden by the leading dot in the tag, so a normal run skips it, exactly
    like Icecast's `[.live]` test. To use one, point it at any RTMP target you
    control, such as your own YouTube "Stream now" or Twitch dashboard:

        OPENDJ_RTMP_SERVER=rtmp://a.rtmp.youtube.com/live2 \
        OPENDJ_RTMP_KEY=xxxx-xxxx-xxxx-xxxx \
        opendj-tests "[.live]"

    The loopback test above proves the protocol, the codecs and the pipe
    against something that always behaves correctly. This is the only thing
    that can prove a real platform accepts what ffmpeg sends it, which is the
    part genuinely unverified without one. */
TEST_CASE ("a real RTMP target accepts the broadcast", "[.live]")
{
    const auto server = juce::SystemStats::getEnvironmentVariable ("OPENDJ_RTMP_SERVER", {});
    const auto key = juce::SystemStats::getEnvironmentVariable ("OPENDJ_RTMP_KEY", {});

    if (server.isEmpty() || key.isEmpty())
    {
        WARN ("Set OPENDJ_RTMP_SERVER and OPENDJ_RTMP_KEY to run this.");
        return;
    }

    RtmpSettings settings;
    settings.server = server;
    settings.streamKey = key;
    settings.streamTitle = "OpenDJ live test";

    RtmpBroadcaster broadcaster;
    juce::String error;

    INFO ("connecting to " << settings.server);
    REQUIRE (broadcaster.start (settings, 48000.0, error));
    REQUIRE (error.isEmpty());

    // Ten seconds, paced, so a viewer could actually join and see it.
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

    REQUIRE (broadcaster.getState() == RtmpBroadcaster::State::live);
    REQUIRE (broadcaster.getBytesSent() > 10000);
    REQUIRE (broadcaster.getDroppedSamples() == 0);

    broadcaster.stop();
}
