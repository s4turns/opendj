/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "stream/RtmpConnection.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #include <cerrno>
 #include <csignal>
 #include <fcntl.h>
 #include <poll.h>
 #include <spawn.h>
 #include <sys/wait.h>
 #include <unistd.h>

 extern char** environ;
#endif

namespace opendj
{

namespace
{
    constexpr int connectTimeoutMs = 8000;   // ffmpeg starting a codec and a TLS handshake is slower than an Icecast socket
    // How long one write to ffmpeg's stdin pipe is allowed to sit before it
    // is treated as dead rather than merely slow. Measured directly: ffmpeg
    // pauses reading its own stdin for a few seconds at a time under real
    // CPU contention, most often right after it starts, since its first
    // keyframe and the RTMP handshake are competing for the same core as
    // whatever else the machine is doing. A short timeout here does not make
    // the pipe faster; it just turns an ordinary stall into a full
    // reconnect, which is far more expensive than the stall itself, since a
    // reconnect means killing ffmpeg and paying its startup cost all over
    // again. This is deliberately more patient than `IcecastConnection`'s
    // socket timeout for that reason.
    constexpr int sendTimeoutMs = 10000;
    constexpr int stopGraceMs = 4000;   // flushing an encoder is heavier than closing a socket

    /** Characters that would otherwise end the ffmpeg filter expression early
        or mean something else once they got there. Dropped rather than
        precisely escaped: a stream title is decoration, not data that has to
        survive intact, and the honest, simple answer for `:`, `'`, `\` and
        `%` inside a filtergraph is to keep them out rather than to get their
        escaping exactly right. */
    juce::String sanitiseForDrawtext (const juce::String& title)
    {
        auto cleaned = title;

        for (auto ch : { juce::juce_wchar (':'), juce::juce_wchar ('\''), juce::juce_wchar ('\\'),
                        juce::juce_wchar ('%'), juce::juce_wchar ('[') , juce::juce_wchar (']') })
            cleaned = cleaned.replaceCharacter (ch, ' ');

        cleaned = cleaned.trim();
        return cleaned.isEmpty() ? juce::String ("OpenDJ") : cleaned;
    }

    /** Whether an ffmpeg answers `-encoders` with libx264 in the list, which
        is the one thing every candidate must have: this whole feature is
        built on shelling out for the H.264 encoding neither JUCE nor OpenDJ
        can do itself. A build without it starts and runs happily on anything
        that does not ask for libx264, so failing later, mid broadcast, would
        be a far worse way to find out. */
    bool ffmpegCanEncodeH264 (const juce::String& executable)
    {
        juce::ChildProcess probe;

        if (! probe.start (juce::StringArray { executable, "-hide_banner", "-encoders" },
                           juce::ChildProcess::wantStdOut))
            return false;

        const auto output = probe.readAllProcessOutput();
        probe.waitForProcessToFinish (5000);
        return output.contains ("libx264");
    }

    /** The font `drawtext` should use, already escaped for a filtergraph, or
        empty to let ffmpeg find one itself. On Linux and macOS fontconfig does
        that. The Windows builds of ffmpeg carry fontconfig with no config file
        to read, and there `drawtext` without a font file does not fail
        cleanly: measured with ffmpeg 8.1.2, it prints "Cannot load default
        config file" and crashes. So Windows names a font it ships with. */
    juce::String titleFontFile()
    {
       #if JUCE_WINDOWS
        const auto fonts = juce::File (juce::SystemStats::getEnvironmentVariable ("WINDIR", "C:\\Windows"))
                               .getChildFile ("Fonts");

        for (const auto* name : { "segoeui.ttf", "arial.ttf" })
            if (const auto font = fonts.getChildFile (name); font.existsAsFile())
                return font.getFullPathName().replaceCharacter ('\\', '/').replace (":", "\\:");
       #endif

        return {};
    }

    /** Whether a title can be drawn at all. False only on a Windows machine
        missing both fonts above, where the title is left off rather than
        handing ffmpeg a filter it would crash on. */
    bool canDrawTitle (const juce::String& fontFile)
    {
       #if JUCE_WINDOWS
        return fontFile.isNotEmpty();
       #else
        juce::ignoreUnused (fontFile);
        return true;
       #endif
    }

}

//==============================================================================

juce::String RtmpSettings::fullUrl() const
{
    auto trimmedServer = server.trim();

    while (trimmedServer.endsWithChar ('/'))
        trimmedServer = trimmedServer.dropLastCharacters (1);

    auto key = streamKey.trim();

    while (key.startsWithChar ('/'))
        key = key.substring (1);

    return trimmedServer + "/" + key;
}

juce::String RtmpSettings::validate() const
{
    const auto trimmedServer = server.trim();

    if (trimmedServer.isEmpty())
        return "The server address is empty.";

    if (! trimmedServer.startsWithIgnoreCase ("rtmp://") && ! trimmedServer.startsWithIgnoreCase ("rtmps://"))
        return "The server address should start with rtmp:// or rtmps://.";

    if (streamKey.trim().isEmpty())
        return "The stream key is empty.";

    return {};
}

//==============================================================================

juce::StringArray buildFfmpegArguments (const RtmpSettings& settings, double sampleRate)
{
    juce::StringArray args;

    args.add ("-hide_banner");

    // "info", not "error": the line this connects on, `Output #0, flv, to
    // ...`, is printed at info level. Nothing above that is noisy enough to
    // be worth suppressing further.
    args.add ("-loglevel"); args.add ("info");
    args.add ("-nostdin");

    // The one-off lines "info" prints, `Output #0` among them, are what
    // `RtmpConnection::waitForConfirmation` watches for. The stderr pipe is
    // drained on every write for as long as the broadcast runs, so nothing
    // here can fill it; `-nostats` only turns off the periodic progress line
    // ("frame=... time=..."), which would otherwise crowd the real reason for
    // a failure out of the short tail kept for error messages.
    args.add ("-nostats");

    // The video track: a plain colour with the stream title drawn on it by
    // ffmpeg's own filters, looped forever from a single generated frame.
    // `-re` paces it to real time; nothing else here would, since a colour
    // source has no natural rate of its own the way the audio pipe does.
    //
    // Two frames a second, not one: measured directly, one frame a second
    // combined with libx264's default lookahead meant the encoder would not
    // emit anything at all until around ten real seconds had passed, since
    // it waits for several frames of lookahead before the first one comes
    // out and each frame here takes a full second to arrive. A picture that
    // never changes does not need many frames a second, but it needs enough
    // that the encoder is not sitting there waiting on real time for frames
    // that carry no new information anyway.
    juce::String videoFilter;
    videoFilter << "color=c=0x1a1a2e:s=" << settings.videoWidth << "x" << settings.videoHeight << ":r=2";

    if (const auto fontFile = titleFontFile(); canDrawTitle (fontFile))
    {
        videoFilter << ",drawtext=";

        if (fontFile.isNotEmpty())
            videoFilter << "fontfile='" << fontFile << "':";

        videoFilter << "text='" << sanitiseForDrawtext (settings.streamTitle) << "'"
                    << ":fontcolor=white:fontsize=36:x=(w-text_w)/2:y=(h-text_h)/2";
    }

    args.add ("-f"); args.add ("lavfi");
    args.add ("-re");
    args.add ("-i"); args.add (videoFilter);

    // The audio track: raw interleaved 16-bit PCM on stdin, at whatever rate
    // the audio device is actually running. Nothing paces this input; the
    // writer feeding it already does, at the rate the audio thread hands over
    // blocks, which is what real time means here.
    args.add ("-f"); args.add ("s16le");
    args.add ("-ar"); args.add (juce::String ((int) sampleRate));
    args.add ("-ac"); args.add ("2");

    // ffmpeg's default queue between the thread reading an input and the
    // thread processing it holds only eight packets, a fraction of a second
    // of audio. With video arriving twice a second, that queue empties into
    // a muxer that only wants to advance that slowly, backs up, and then
    // stalls the read from stdin entirely until a video frame lets it drain,
    // which is what was actually behind the audio thread's FIFO overflowing:
    // ffmpeg's own stdin reads were the slow end, not this process's writes.
    // A much larger queue here decouples the two, the standard fix for
    // exactly this pairing of a fast audio input and a slow video one.
    args.add ("-thread_queue_size"); args.add ("4096");

    // No probing. By default ffmpeg reads up to five seconds of an input
    // before it opens its outputs, and it prints `Output #0`, the line
    // `waitForConfirmation` waits for, only after that. The format, rate and
    // channel count are all given above, so there is nothing to find out.
    // Measured: given a quarter second of silence and a pipe left open, it
    // never connected at all; with probing off it connected in a third of a
    // second. In the app the audio device eventually supplies the five
    // seconds, so a broadcast only started slowly; a test that waits for
    // confirmation before sending more failed every time.
    args.add ("-analyzeduration"); args.add ("0");
    args.add ("-probesize"); args.add ("32");

    args.add ("-i"); args.add ("pipe:0");

    // No `-shortest`. It used to be here on the reasoning that it could only
    // help, but it never stopped ffmpeg (a looped colour source has no end to
    // compare against; `Process::stop` sends a signal instead), and in ffmpeg
    // 8 it does real harm: it holds every stream back until the others catch
    // up, and audio arriving ahead of a real-time video track means nothing
    // is encoded and the audio pipe stops being read. Measured with the
    // loopback test's own pattern, a quarter second, a pause, then eight
    // seconds at once: with it, no frames at all in ten seconds, until the
    // send timeout killed ffmpeg; without it, all eight seconds went through.
    // That was the loopback test's "stall".

    args.add ("-map"); args.add ("0:v");
    args.add ("-map"); args.add ("1:a");

    args.add ("-c:v"); args.add ("libx264");
    args.add ("-preset"); args.add ("veryfast");

    // A still frame has nothing for motion estimation to do, and this tuning
    // says so rather than spending encoder time discovering it every frame.
    args.add ("-tune"); args.add ("stillimage");
    args.add ("-pix_fmt"); args.add ("yuv420p");

    // No B-frames: they buy nothing against a picture that never changes,
    // and every one is a frame libx264 holds back before it can emit
    // anything, which is exactly the multi-second startup delay this whole
    // corner of the design is written to avoid.
    args.add ("-bf"); args.add ("0");

    // A key frame every 50 frames of a 2 fps source is one every 25 seconds,
    // comfortably under what a player waits before it gives up looking for
    // one, without paying the cost of a key frame for a picture that never
    // changes.
    args.add ("-g"); args.add ("50");
    args.add ("-b:v"); args.add (juce::String (settings.videoBitrateKbps) + "k");

    args.add ("-c:a"); args.add ("aac");
    args.add ("-b:a"); args.add (juce::String (settings.audioBitrateKbps) + "k");
    args.add ("-ar"); args.add ("44100");

    // A video frame every second is far rarer than an AAC frame (roughly
    // forty-three a second), and ffmpeg's muxer interleaves by holding
    // packets until it can write them in timestamp order across streams. The
    // default queue depth was not enough to bridge that gap: audio packets
    // backed up faster than a once-a-second video frame could clear them,
    // and once the queue is deep enough ffmpeg errors out rather than
    // stalling forever. This asks for a queue that can actually hold a
    // second's worth of audio.
    args.add ("-max_muxing_queue_size"); args.add ("1024");

    args.add ("-f"); args.add ("flv");
    args.add (settings.fullUrl());

    return args;
}

//==============================================================================
#if JUCE_WINDOWS

/** The Windows half: an anonymous pipe wired to the child's stdin via
    inheritable handles, and `CreateProcess` in place of `posix_spawn`. Same
    shape as the POSIX half below; only the operating system calls differ.
    Verified with the loopback test on Windows 11 and ffmpeg 8.1.2. ffmpeg's
    stderr goes to this process's own rather than a pipe, so confirmation is
    only "still running" here; see `stderrSawConnectSignal`. */
class RtmpConnection::Process
{
public:
    ~Process() { stop(); }

    bool start (const juce::String& executable, const juce::StringArray& arguments, juce::String& error)
    {
        SECURITY_ATTRIBUTES sa {};
        sa.nLength = sizeof (SECURITY_ATTRIBUTES);
        sa.bInheritHandle = TRUE;

        HANDLE stdinRead = nullptr, stdinWrite = nullptr;

        if (! CreatePipe (&stdinRead, &stdinWrite, &sa, 0))
        {
            error = "Could not create a pipe for ffmpeg.";
            return false;
        }

        // The write end is ours; the child must not inherit it, or its own
        // copy keeps the pipe open after we close ours and disconnect never
        // finishes.
        SetHandleInformation (stdinWrite, HANDLE_FLAG_INHERIT, 0);

        juce::String commandLine = "\"" + executable + "\"";

        for (const auto& arg : arguments)
            commandLine << " \"" << arg.replace ("\"", "\\\"") << "\"";

        STARTUPINFOW startInfo {};
        startInfo.cb = sizeof (STARTUPINFOW);
        startInfo.dwFlags = STARTF_USESTDHANDLES;
        startInfo.hStdInput = stdinRead;
        startInfo.hStdOutput = GetStdHandle (STD_ERROR_HANDLE);   // ffmpeg's own noise, not read back
        startInfo.hStdError = GetStdHandle (STD_ERROR_HANDLE);

        PROCESS_INFORMATION processInfo {};
        auto commandLineBuffer = commandLine.toUTF16();

        const auto launched = CreateProcessW (nullptr, const_cast<LPWSTR> (commandLineBuffer.getAddress()),
                                              nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                              nullptr, nullptr, &startInfo, &processInfo);

        CloseHandle (stdinRead);

        if (! launched)
        {
            CloseHandle (stdinWrite);
            error = "Could not start ffmpeg. Is it installed and on PATH?";
            return false;
        }

        processHandle = processInfo.hProcess;
        CloseHandle (processInfo.hThread);
        stdinHandle = stdinWrite;

        return true;
    }

    bool isRunning() const
    {
        if (processHandle == nullptr)
            return false;

        DWORD exitCode = 0;
        return GetExitCodeProcess (processHandle, &exitCode) != 0 && exitCode == STILL_ACTIVE;
    }

    bool write (const void* data, size_t numBytes)
    {
        if (stdinHandle == nullptr)
            return false;

        auto* bytes = static_cast<const char*> (data);
        size_t remaining = numBytes;

        while (remaining > 0)
        {
            DWORD written = 0;

            if (! WriteFile (stdinHandle, bytes, (DWORD) remaining, &written, nullptr) || written == 0)
            {
                stop();
                return false;
            }

            bytes += written;
            remaining -= written;
        }

        return true;
    }

    /** No stderr capture on this side: without a second inheritable pipe and
        a reader thread for it there is nothing to search for the connect
        signal, so this side falls back to "still running after the timeout
        means connected", a weaker check than the POSIX one. Worth tightening
        alongside whoever first runs this on Windows. */
    bool stderrSawConnectSignal() const { return isRunning(); }

    /** Nothing to report for the same reason: ffmpeg's output goes to this
        process's own stderr rather than a pipe it could be read back from. */
    juce::String lastDiagnostic() const { return {}; }

    void stop()
    {
        if (stdinHandle != nullptr)
        {
            CloseHandle (stdinHandle);
            stdinHandle = nullptr;
        }

        if (processHandle != nullptr)
        {
            WaitForSingleObject (processHandle, (DWORD) stopGraceMs);

            DWORD exitCode = 0;

            if (GetExitCodeProcess (processHandle, &exitCode) != 0 && exitCode == STILL_ACTIVE)
                TerminateProcess (processHandle, 1);

            CloseHandle (processHandle);
            processHandle = nullptr;
        }
    }

private:
    HANDLE processHandle = nullptr;
    HANDLE stdinHandle = nullptr;
};

#else

/** The POSIX half: `posix_spawn` rather than `fork`, because forking a
    process with JUCE's audio and message threads already running and then
    doing anything beyond `exec` in the child is asking for a deadlock on a
    lock some other thread held at the moment of the fork. `posix_spawn` does
    the fork-then-exec entirely inside the C library, off the record. */
class RtmpConnection::Process
{
public:
    ~Process() { stop(); }

    bool start (const juce::String& executable, const juce::StringArray& arguments, juce::String& error)
    {
        // Writing to a pipe nobody is reading from raises SIGPIPE, which
        // kills the whole application by default. Every process using pipes
        // has to opt out of that once; this is as good a place as any.
        static const bool sigpipeIgnored = [] { ::signal (SIGPIPE, SIG_IGN); return true; }();
        juce::ignoreUnused (sigpipeIgnored);

        int stdinPipe[2];
        int stderrPipe[2];

        if (::pipe (stdinPipe) != 0)
        {
            error = "Could not create a pipe for ffmpeg.";
            return false;
        }

        if (::pipe (stderrPipe) != 0)
        {
            ::close (stdinPipe[0]); ::close (stdinPipe[1]);
            error = "Could not create a pipe for ffmpeg.";
            return false;
        }

        std::vector<juce::String> argStorage;
        argStorage.push_back (executable);

        for (const auto& arg : arguments)
            argStorage.push_back (arg);

        std::vector<char*> argv;

        for (auto& arg : argStorage)
            argv.push_back (const_cast<char*> (arg.toRawUTF8()));

        argv.push_back (nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init (&actions);

        // The child's stdin is our end of the pipe it reads audio from; the
        // child's stderr is our end of the pipe we read its diagnostics from.
        posix_spawn_file_actions_adddup2 (&actions, stdinPipe[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2 (&actions, stderrPipe[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose (&actions, stdinPipe[1]);
        posix_spawn_file_actions_addclose (&actions, stderrPipe[0]);

        const auto devNull = ::open ("/dev/null", O_WRONLY);

        if (devNull >= 0)
            posix_spawn_file_actions_adddup2 (&actions, devNull, STDOUT_FILENO);

        pid_t spawnedPid = 0;
        const auto rc = ::posix_spawnp (&spawnedPid, executable.toRawUTF8(), &actions, nullptr, argv.data(), environ);

        posix_spawn_file_actions_destroy (&actions);
        ::close (stdinPipe[0]);
        ::close (stderrPipe[1]);

        if (devNull >= 0)
            ::close (devNull);

        if (rc != 0)
        {
            ::close (stdinPipe[1]);
            ::close (stderrPipe[0]);
            error = "Could not start ffmpeg. Is it installed and on PATH? (" + juce::String (::strerror (rc)) + ")";
            return false;
        }

        pid = spawnedPid;
        stdinFd = stdinPipe[1];
        stderrFd = stderrPipe[0];

        // Non-blocking: the stderr pipe is drained a little at a time from
        // whichever thread happens to poll it, never waited on.
        ::fcntl (stderrFd, F_SETFL, O_NONBLOCK);

        return true;
    }

    bool isRunning()
    {
        if (pid <= 0)
            return false;

        int status = 0;
        const auto result = ::waitpid (pid, &status, WNOHANG);

        if (result == 0)
            return true;

        // Reaped or gone: either way, not running, and not worth calling
        // waitpid on again.
        pid = -1;
        return false;
    }

    bool write (const void* data, size_t numBytes)
    {
        if (stdinFd < 0)
            return false;

        auto* bytes = static_cast<const char*> (data);
        size_t remaining = numBytes;

        // One deadline for the whole write rather than one per poll, so an
        // ffmpeg that keeps printing but never reads its stdin still times out.
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) sendTimeoutMs;

        while (remaining > 0)
        {
            const auto now = juce::Time::getMillisecondCounter();

            if (now >= deadline)
            {
                stop();
                return false;
            }

            // Stderr is watched here as well as stdin, and drained whenever it
            // has anything. ffmpeg goes on printing warnings for as long as it
            // runs, and once nobody reads them its stderr pipe fills, it blocks
            // on that write and stops reading stdin, while this waits for it to
            // read stdin: two idle processes, each waiting on the other. That
            // was the loopback test's intermittent stall.
            pollfd fds[2] { { stdinFd, POLLOUT, 0 }, { stderrFd, POLLIN, 0 } };
            const auto ready = ::poll (fds, 2, (int) (deadline - now));

            if (ready < 0)
            {
                if (errno == EINTR)
                    continue;

                stop();
                return false;
            }

            if ((fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
                drain();

            // Nothing writable yet. A pipe the child has closed reports an
            // error instead, and the write below turns that into a stop.
            if ((fds[0].revents & (POLLOUT | POLLERR | POLLHUP)) == 0)
                continue;

            const auto written = ::write (stdinFd, bytes, remaining);

            if (written < 0)
            {
                if (errno == EINTR)
                    continue;

                stop();
                return false;
            }

            if (written == 0)
            {
                stop();
                return false;
            }

            bytes += written;
            remaining -= (size_t) written;
        }

        return true;
    }

    /** Reads whatever ffmpeg has printed since the last call, without ever
        waiting for more of it, and keeps the tail of it for an error message.
        `Output #0` is what ffmpeg prints once it has opened its output URL,
        which for the `rtmp` and `rtmps` protocols happens only once the
        handshake has completed; that line is the closest thing to a
        confirmed connection this design has. */
    bool stderrSawConnectSignal()
    {
        drain();
        return sawConnectSignal;
    }

    juce::String lastDiagnostic()
    {
        drain();

        const auto lines = juce::StringArray::fromLines (recent);

        for (int i = lines.size() - 1; i >= 0; --i)
            if (lines[i].trim().isNotEmpty())
                return lines[i].trim();

        return {};
    }

    void stop()
    {
        if (stdinFd >= 0)
        {
            ::close (stdinFd);
            stdinFd = -1;
        }

        if (pid > 0)
        {
            // A closed stdin alone is not enough: the video track is a
            // looped colour source with no natural end, and `-shortest`
            // turned out not to stop ffmpeg when only the audio side runs
            // out, measured directly rather than assumed. SIGTERM is what
            // actually reaches ffmpeg's own signal handler, the same one a
            // person hitting Ctrl+C gets, and it flushes and closes the RTMP
            // connection properly before exiting; SIGKILL is only the
            // fallback for a process that does not respond to that at all.
            ::kill (pid, SIGTERM);

            int status = 0;
            const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) stopGraceMs;
            bool exited = false;

            while (juce::Time::getMillisecondCounter() < deadline)
            {
                if (::waitpid (pid, &status, WNOHANG) != 0)
                {
                    exited = true;
                    break;
                }

                juce::Thread::sleep (20);
            }

            if (! exited)
            {
                ::kill (pid, SIGKILL);
                ::waitpid (pid, &status, 0);
            }

            pid = -1;
        }

        const std::lock_guard<std::mutex> lock (stderrMutex);

        if (stderrFd >= 0)
        {
            ::close (stderrFd);
            stderrFd = -1;
        }
    }

private:
    void drain()
    {
        // `write` drains from the broadcast's writer thread while
        // `waitForConfirmation` and `lastDiagnostic` drain from whichever
        // thread is starting or reconnecting, so the pipe and the tail are
        // shared between threads and take turns.
        const std::lock_guard<std::mutex> lock (stderrMutex);

        if (stderrFd < 0)
            return;

        char buffer[1024];
        ssize_t read = 0;

        while ((read = ::read (stderrFd, buffer, sizeof (buffer))) > 0)
            recent += juce::String::fromUTF8 (buffer, (int) read);

        // End of file: ffmpeg has closed its stderr or exited. Closed here so
        // `write` stops polling a pipe that would report a hangup forever.
        if (read == 0)
        {
            ::close (stderrFd);
            stderrFd = -1;
        }

        // Noted before the tail is cut, not searched for afterwards: anything
        // ffmpeg prints after `Output #0` in the same read would otherwise
        // push the line out of the tail before anybody looked for it.
        if (! sawConnectSignal && recent.contains ("Output #0"))
            sawConnectSignal = true;

        // Kept short on purpose: this exists to say why a connection failed,
        // not to archive ffmpeg's console.
        constexpr int keepChars = 4096;

        if (recent.length() > keepChars)
            recent = recent.substring (recent.length() - keepChars);
    }

    pid_t pid = -1;
    int stdinFd = -1;
    int stderrFd = -1;
    juce::String recent;
    std::atomic<bool> sawConnectSignal { false };
    std::mutex stderrMutex;
};

#endif

//==============================================================================

RtmpConnection::RtmpConnection() = default;
RtmpConnection::~RtmpConnection() { disconnect(); }

juce::String RtmpConnection::findFfmpeg (juce::String& diagnostic)
{
    juce::StringArray candidates;

    if (const auto fromEnvironment = juce::SystemStats::getEnvironmentVariable ("OPENDJ_RTMP_FFMPEG", {});
        fromEnvironment.isNotEmpty())
        candidates.add (fromEnvironment);

   #if JUCE_WINDOWS
    candidates.add ("ffmpeg.exe");
   #else
    candidates.add ("/usr/local/bin/ffmpeg");
    candidates.add ("/opt/homebrew/bin/ffmpeg");
    candidates.add ("/usr/bin/ffmpeg");
    candidates.add ("ffmpeg");
   #endif

    auto sawAny = false;

    for (const auto& candidate : candidates)
    {
        // A bare name is left to PATH; anything else has to exist, since
        // posix_spawnp only falls back to a PATH search when the name it
        // is given contains no slash at all.
        if (candidate.containsChar (juce::File::getSeparatorChar())
            && ! juce::File (candidate).existsAsFile())
            continue;

        juce::ChildProcess versionCheck;

        if (! versionCheck.start (juce::StringArray { candidate, "-version" },
                                  juce::ChildProcess::wantStdOut))
            continue;

        versionCheck.waitForProcessToFinish (5000);
        sawAny = true;

        if (ffmpegCanEncodeH264 (candidate))
            return candidate;
    }

    diagnostic = sawAny
        ? juce::String ("Found ffmpeg, but none of it can encode H.264 (no libx264 in any "
                        "copy on PATH or the usual install locations). Install a build that "
                        "can, or set OPENDJ_RTMP_FFMPEG to one that does.")
        : juce::String ("Could not find ffmpeg anywhere. Install it, or set OPENDJ_RTMP_FFMPEG "
                        "to its full path.");
    return {};
}

bool RtmpConnection::launch (const RtmpSettings& settings, double sampleRate, juce::String& error)
{
    disconnect();

    if (error = settings.validate(); error.isNotEmpty())
        return false;

    const auto ffmpegPath = findFfmpeg (error);

    if (ffmpegPath.isEmpty())
        return false;

    process = std::make_unique<Process>();
    const auto args = buildFfmpegArguments (settings, sampleRate);

    if (! process->start (ffmpegPath, args, error))
    {
        process.reset();
        return false;
    }

    // Not a confirmed connection, only a process that exists and a pipe it
    // will read from: see `waitForConfirmation`.
    connected.store (true, std::memory_order_relaxed);
    bytesSent.store (0, std::memory_order_relaxed);
    return true;
}

bool RtmpConnection::waitForConfirmation (juce::String& error)
{
    if (process == nullptr)
    {
        error = "ffmpeg was not started.";
        return false;
    }

    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) connectTimeoutMs;

    while (juce::Time::getMillisecondCounter() < deadline)
    {
        if (! process->isRunning())
        {
            const auto diagnostic = process->lastDiagnostic();
            error = diagnostic.isNotEmpty() ? "ffmpeg exited: " + diagnostic
                                            : "ffmpeg exited before it connected.";
            connected.store (false, std::memory_order_relaxed);
            return false;
        }

        if (process->stderrSawConnectSignal())
            return true;

        juce::Thread::sleep (50);
    }

    error = "ffmpeg did not confirm the connection within "
          + juce::String (connectTimeoutMs / 1000) + " seconds.";
    connected.store (false, std::memory_order_relaxed);
    return false;
}

void RtmpConnection::disconnect()
{
    connected.store (false, std::memory_order_relaxed);
    process.reset();
}

bool RtmpConnection::send (const void* data, int numBytes)
{
    if (process == nullptr || numBytes <= 0 || ! isConnected())
        return false;

    if (! process->write (data, (size_t) numBytes))
    {
        connected.store (false, std::memory_order_relaxed);
        return false;
    }

    bytesSent.fetch_add (numBytes, std::memory_order_relaxed);
    return true;
}

} // namespace opendj
