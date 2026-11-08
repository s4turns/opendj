/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "analysis/StemSeparator.h"

#include "analysis/TrackDecoder.h"

#include <cerrno>
#include <cmath>
#include <limits>
#include <vector>

#if ! JUCE_WINDOWS
 #include <csignal>
 #include <fcntl.h>
 #include <sys/wait.h>
 #include <unistd.h>
#endif

#if JUCE_LINUX
 #include <sys/prctl.h>
#endif

namespace opendj
{

namespace
{
    const char* const stemFileNames[numStems] { "drums", "bass", "other", "vocals" };

    /** Written beside the stems so a cache entry describes itself. */
    const char* const manifestName = "opendj-stems.txt";

    /** Written beside the stems to say which version of the track they came
        from. The cache is keyed on the path, and a file can change under one. */
    const char* const stampName = "opendj-stems-source.txt";

    const char* const partialSuffix = ".partial";

    // 255 bytes, less " [" and sixteen hex digits and "]", less ".partial". The
    // name is kept to plain ASCII, so a character is a byte.
    constexpr int longestNameInKey = 255 - 19 - 8;

    /** Stems that disagree by less than this are trimmed to the shortest, since
        a resampler can leave a stem a little over. By more, something went wrong. */
    constexpr int mostStemsMayDiffer = 4096;

    juce::String stampFor (const juce::File& track)
    {
        return juce::String (track.getSize()) + " " + juce::String (track.getLastModificationTime().toMilliseconds());
    }

    juce::File stemFileIn (const juce::File& folder, int stem, const char* extension)
    {
        return folder.getChildFile (juce::String (stemFileNames[stem]) + extension);
    }

    /** Separation routinely produces stems that peak above unity -- one part of
        a mix can be louder than the mix -- and FLAC is an integer format, so
        they have to be scaled to fit. The scale must be the SAME for all four:
        scaling each by its own peak would mean they no longer sum back to the
        track they came from, which is the one property the deck relies on. */
    float sharedScaleFor (const std::array<juce::AudioBuffer<float>, numStems>& stems)
    {
        auto peak = 0.0f;

        for (const auto& stem : stems)
            peak = juce::jmax (peak, stem.getMagnitude (0, stem.getNumSamples()));

        return peak > 1.0f ? 1.0f / peak : 1.0f;
    }

    /** Somewhere unambiguous to keep a track's stems. The name alone would
        collide between two tracks called the same thing in different folders,
        and a full path does not fit in one. */
    juce::String cacheKeyFor (const juce::File& track)
    {
        // A plain hash of the path, so two tracks of the same name in different
        // folders do not share a cache entry.
        juce::uint64 hash = 1469598103934665603ull;

        const auto path = track.getFullPathName();

        for (int i = 0; i < path.getNumBytesAsUTF8(); ++i)
        {
            hash ^= (juce::uint64) (unsigned char) path.toRawUTF8()[i];
            hash *= 1099511628211ull;
        }

        const auto digest = juce::String::toHexString ((juce::int64) hash).paddedLeft ('0', 16);

        // A folder's name can be 255 bytes, and the one a job writes into has
        // the hash and ".partial" after this. The hash is what keeps keys apart,
        // so a long name loses its end. Anything short enough to have fitted
        // before keeps the key it always had.
        const auto name = track.getFileNameWithoutExtension().retainCharacters (
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_");

        return name.substring (0, longestNameInKey) + " [" + digest + "]";
    }
}

namespace
{
   #if JUCE_WINDOWS
    /** The separator, running. On Windows a child's exit code is the truth,
        including that of one something else killed. */
    class SeparatorProcess
    {
    public:
        bool start (const juce::StringArray& command) { return process.start (command, 0); }
        bool isRunning()    { return process.isRunning(); }
        bool succeeded()    { return process.getExitCode() == 0; }
        void kill()         { process.kill(); }

    private:
        juce::ChildProcess process;
    };
   #else
    /** The separator, running, and why juce::ChildProcess will not do here.

        JUCE reports a child killed by a signal as having exited with 0. A
        separator the kernel killed for its memory, with half its stems written,
        would read as a success. And it kills only the child, when the separator
        may well have workers of its own, which would keep every core busy with
        nobody to report to. So this asks waitpid itself, and puts the separator
        in a process group of its own so all of it can be stopped at once.

        Its output goes to /dev/null. A pipe nobody reads fills up, and then a
        chatty separator waits for ever on a write and takes the queue with it. */
    class SeparatorProcess
    {
    public:
        ~SeparatorProcess() { kill(); }

        bool start (const juce::StringArray& command)
        {
            std::vector<char*> arguments;

            for (const auto& argument : command)
                arguments.push_back (const_cast<char*> (argument.toRawUTF8()));

            arguments.push_back (nullptr);

            const auto parent = ::getpid();
            const auto pid = ::fork();

            if (pid < 0)
                return false;

            if (pid == 0)
            {
                // The child of a threaded process: nothing but system calls.
                ::setpgid (0, 0);

               #if JUCE_LINUX
                // In a group of its own, nothing takes it down with this
                // application: a crash, a kill or a Ctrl-C would leave it with
                // every core for minutes, and writing into a folder the next
                // start sweeps away. So it dies when the thread waiting on it
                // does, and if that has happened already, it never starts.
                ::prctl (PR_SET_PDEATHSIG, SIGKILL);

                if (::getppid() != parent)
                    ::_exit (127);
               #endif

                const auto nothing = ::open ("/dev/null", O_RDWR);
                ::dup2 (nothing, STDIN_FILENO);
                ::dup2 (nothing, STDOUT_FILENO);
                ::dup2 (nothing, STDERR_FILENO);

                ::execv (arguments[0], arguments.data());
                ::_exit (127);
            }

            // Both sides ask, so the group exists whichever of them runs first.
            ::setpgid (pid, pid);
            child = pid;
            return true;
        }

        bool isRunning()
        {
            if (child <= 0 || finished)
                return false;

            auto state = 0;
            const auto answer = ::waitpid (child, &state, WNOHANG);

            if (answer == 0 || (answer < 0 && errno == EINTR))
                return true;

            finished = true;
            exitedCleanly = answer == child && WIFEXITED (state) && WEXITSTATUS (state) == 0;

            // Anything it started goes with it, however it ended.
            ::kill (-child, SIGKILL);
            return false;
        }

        bool succeeded() const { return exitedCleanly; }

        void kill()
        {
            if (child <= 0 || finished)
                return;

            ::kill (-child, SIGKILL);

            // A kill takes effect at once unless the process is stuck in a
            // driver, and that must not wedge the queue. It stays a zombie then,
            // until it comes back or this application ends.
            for (int i = 0; i < 200 && isRunning(); ++i)
                juce::Thread::sleep (10);

            finished = true;
        }

    private:
        ::pid_t child = 0;
        bool finished = false;
        bool exitedCleanly = false;
    };
   #endif

    /** Writes four stems as 24-bit FLAC, which is lossless for anything a
        listener can hear, at one shared scale so they still sum back to the
        track. False if anything at all went wrong. */
    bool writeStems (const juce::File& folder,
                     const std::array<juce::AudioBuffer<float>, numStems>& stems,
                     double sampleRate)
    {
        const auto scale = sharedScaleFor (stems);
        juce::FlacAudioFormat flac;

        for (int stem = 0; stem < numStems; ++stem)
        {
            juce::AudioBuffer<float> scaled (stems[(size_t) stem]);

            if (! juce::exactlyEqual (scale, 1.0f))
                scaled.applyGain (scale);

            const auto target = stemFileIn (folder, stem, ".flac");

            if (! target.deleteFile())
                return false;

            auto stream = std::make_unique<juce::FileOutputStream> (target);

            if (! stream->openedOk())
                return false;

            std::unique_ptr<juce::AudioFormatWriter> writer (
                flac.createWriterFor (stream.get(), sampleRate, 2, 24, {}, 0));

            if (writer == nullptr)
                return false;

            stream.release();       // the writer owns it now

            if (! writer->writeFromAudioSampleBuffer (scaled, 0, scaled.getNumSamples()))
                return false;
        }

        return folder.getChildFile (manifestName).replaceWithText (juce::String (scale, 8));
    }

    /** The separator writes 32-bit float WAV, which is four bytes a sample and
        adds up fast: four stems of a seven minute track are close to six hundred
        megabytes. As FLAC they cost about half that. */
    bool repackAsFlac (juce::AudioFormatManager& formats,
                       const juce::File& from,
                       const juce::File& to,
                       const std::atomic<bool>& stop)
    {
        std::array<juce::AudioBuffer<float>, numStems> stems;
        auto rate = 44100.0;
        auto shortest = std::numeric_limits<int>::max();
        auto longest = 0;

        for (int stem = 0; stem < numStems; ++stem)
        {
            if (stop.load (std::memory_order_relaxed))
                return false;

            auto decoded = TrackDecoder::decode (formats, stemFileIn (from, stem, ".wav"));

            if (decoded == nullptr)
                return false;

            rate = decoded->sampleRate;
            shortest = juce::jmin (shortest, decoded->audio.getNumSamples());
            longest = juce::jmax (longest, decoded->audio.getNumSamples());
            stems[(size_t) stem] = std::move (decoded->audio);
        }

        if (longest - shortest > mostStemsMayDiffer)
            return false;

        for (auto& stem : stems)
            stem.setSize (2, shortest, true, true, true);

        return writeStems (to, stems, rate);
    }

    /** The last word before a folder becomes a cache entry: all four stems
        really decode, to one length, and that is the length of the track. An
        entry that passes this but is wrong would be played in a set, and nothing
        would ever separate that track again.

        Only near the track's, because a file's length is an estimate for some
        formats. Either measure will do: the model hands back as many samples as
        it was given, at a rate it states whatever the track's was, and a command
        may resample and so agree in seconds only. */
    bool stemsMatchTrack (juce::AudioFormatManager& formats,
                          const juce::File& folder,
                          const juce::File& track,
                          const std::atomic<bool>& stop)
    {
        const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (track));

        if (reader == nullptr || reader->sampleRate <= 0.0)
            return false;

        const auto trackSamples = (double) reader->lengthInSamples;
        const auto trackSeconds = trackSamples / reader->sampleRate;
        const auto allowedSeconds = juce::jmax (1.0, trackSeconds * 0.02);

        auto length = -1;

        for (int stem = 0; stem < numStems; ++stem)
        {
            if (stop.load (std::memory_order_relaxed))
                return false;

            const auto decoded = TrackDecoder::decode (formats, stemFileIn (folder, stem, ".flac"));

            if (decoded == nullptr)
                return false;

            const auto samples = decoded->audio.getNumSamples();

            if (samples <= 0 || (length >= 0 && samples != length))
                return false;

            length = samples;

            const auto nearInSeconds = std::abs (decoded->lengthSeconds() - trackSeconds) <= allowedSeconds;
            const auto nearInSamples = std::abs ((double) samples - trackSamples) <= allowedSeconds * reader->sampleRate;

            if (! nearInSeconds && ! nearInSamples)
                return false;
        }

        return true;
    }
}

StemSeparator::StemSeparator (Models modelsToUse)
    : useInstalledModels (modelsToUse == Models::useInstalled),
      separatorCommand (findSeparator())
{
}

StemSeparator::~StemSeparator() = default;

bool StemSeparator::hasNativeModel() const
{
    if (standInModel != nullptr)
        return true;

    return useInstalledModels && (StemModel::findModel().existsAsFile() || StemModel::findOnnxModel().existsAsFile());
}

juce::File StemSeparator::defaultCacheRoot()
{
    // Stems are large and reproducible, so they belong in a cache rather than
    // beside the settings: deleting them costs time, not data.
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    const auto xdg = juce::SystemStats::getEnvironmentVariable ("XDG_CACHE_HOME", {});

    return (xdg.isNotEmpty() ? juce::File (xdg) : home.getChildFile (".cache"))
               .getChildFile ("opendj").getChildFile ("stems");
}

juce::File StemSeparator::findSeparator() const
{
    // An explicit choice first, then the usual places a user tool installs to.
    if (const auto fromEnvironment = juce::SystemStats::getEnvironmentVariable ("OPENDJ_STEM_SEPARATOR", {});
        fromEnvironment.isNotEmpty())
        if (const juce::File named (fromEnvironment); named.existsAsFile())
            return named;

    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    const juce::File candidates[]
    {
        home.getChildFile (".local/bin/stemsep"),
        juce::File ("/usr/local/bin/stemsep"),
        juce::File ("/usr/bin/stemsep")
    };

    for (const auto& candidate : candidates)
        if (candidate.existsAsFile())
            return candidate;

    return {};
}

juce::String StemSeparator::getStatusDescription() const
{
    if (hasNativeModel())
    {
        // Asked from the message thread, which must not wait out a load.
        const juce::ScopedTryLock lock (modelLock);

        if (! lock.isLocked())
            return "Loading the stem model";

        // Which model is in use is only known once one has loaded; before
        // that, the one it would most likely be.
        auto file = model.getModelFile();

        if (file == juce::File())
            file = StemModel::findOnnxModel().existsAsFile() ? StemModel::findOnnxModel() : StemModel::findModel();

        juce::String description;

        if (const auto device = model.getDeviceName(); device.isNotEmpty())
            description << "Separating on " << device << ", model at " << file.getFullPathName();
        else
            description << "Separating in process, model at " << file.getFullPathName();

        if (modelError.isNotEmpty())
            description << ". " << modelError;
        else if (const auto reason = model.getFallbackReason(); reason.isNotEmpty())
            description << ". " << reason;

        return description;
    }

    if (separatorCommand.existsAsFile())
        return "Separating with " + separatorCommand.getFullPathName();

    return "No stem separator found. Install the htdemucs OpenVINO model, or point "
           "OPENDJ_STEM_MODEL at it; failing that OPENDJ_STEM_SEPARATOR at a command.";
}

std::shared_ptr<const SeparatedTrack> StemSeparator::separateNatively (
    const juce::AudioBuffer<float>& mix,
    const std::function<void (float)>& onProgress,
    const std::atomic<bool>& stop)
{
    const auto keepGoing = [&onProgress, &stop] (float progress)
    {
        if (onProgress != nullptr)
            onProgress (progress);

        return ! stop.load (std::memory_order_relaxed);
    };

    if (standInModel != nullptr)
    {
        const auto answer = standInModel (mix, keepGoing);
        return answer != nullptr && answer->isWellFormed() ? answer : nullptr;
    }

    {
        const juce::ScopedLock lock (modelLock);

        if (! modelLoadAttempted)
        {
            modelLoadAttempted = true;
            modelError = model.load();
        }

        if (! model.isLoaded())
            return nullptr;
    }

    auto separation = std::make_shared<SeparatedTrack>();
    const auto ok = model.separate (mix, *separation, keepGoing);

    return ok && separation->isWellFormed() ? separation : nullptr;
}

juce::File StemSeparator::getCacheFolder (const juce::File& track) const
{
    return cacheRoot.getChildFile (cacheKeyFor (track));
}

juce::File StemSeparator::getPartialFolder (const juce::File& track) const
{
    return cacheRoot.getChildFile (cacheKeyFor (track) + partialSuffix);
}

juce::int64 StemSeparator::getFreeBytes() const
{
    if (freeSpaceProbe != nullptr)
        return freeSpaceProbe();

    // Asked of a folder that may not exist yet, on a first run.
    cacheRoot.createDirectory();
    return cacheRoot.getBytesFreeOnVolume();
}

bool StemSeparator::isCached (const juce::File& track) const
{
    const auto folder = getCacheFolder (track);

    if (! folder.isDirectory() || ! track.existsAsFile())
        return false;

    for (int stem = 0; stem < numStems; ++stem)
        if (stemFileIn (folder, stem, ".flac").getSize() <= 0 && stemFileIn (folder, stem, ".wav").getSize() <= 0)
            return false;

    if (const auto stamp = folder.getChildFile (stampName); stamp.existsAsFile())
        return stamp.loadFileAsString().trim() == stampFor (track);

    // Written before there were stamps. The folder being newer than the track
    // is the best that can be said for it, and it is usually true.
    return folder.getLastModificationTime() >= track.getLastModificationTime();
}

std::shared_ptr<const SeparatedTrack> StemSeparator::loadFromCache (juce::AudioFormatManager& formats,
                                                                    const juce::File& track,
                                                                    bool* broken)
{
    if (broken != nullptr)
        *broken = false;

    if (! isCached (track))
        return nullptr;

    cacheDecodes.fetch_add (1, std::memory_order_relaxed);

    const auto folder = getCacheFolder (track);

    // The scale the stems were written with, so it can be undone. An entry
    // without a manifest was written at unity.
    auto scale = 1.0f;

    if (const auto manifest = folder.getChildFile (manifestName); manifest.existsAsFile())
        if (const auto written = manifest.loadFileAsString().trim().getFloatValue(); written > 0.0f)
            scale = written;

    auto separation = std::make_shared<SeparatedTrack>();
    auto shortest = std::numeric_limits<int>::max();

    for (int stem = 0; stem < numStems; ++stem)
    {
        auto file = stemFileIn (folder, stem, ".wav");

        if (! file.existsAsFile())
            file = stemFileIn (folder, stem, ".flac");

        auto outOfMemory = false;
        auto decoded = TrackDecoder::decode (formats, file, nullptr, &outOfMemory);

        // Running out of memory says nothing about the entry, which may have
        // taken minutes to make.
        if (decoded == nullptr)
        {
            if (broken != nullptr)
                *broken = ! outOfMemory;

            return nullptr;
        }

        separation->sampleRate = decoded->sampleRate;
        shortest = juce::jmin (shortest, decoded->audio.getNumSamples());
        separation->stems[(size_t) stem] = std::move (decoded->audio);
    }

    // A stem that decoded a sample or two short of the others would be read
    // past its end by the audio thread, which trusts them to match.
    for (auto& stem : separation->stems)
    {
        stem.setSize (2, shortest, true, true, true);

        if (! juce::exactlyEqual (scale, 1.0f))
            stem.applyGain (1.0f / scale);
    }

    if (! separation->isWellFormed())
    {
        if (broken != nullptr)
            *broken = true;

        return nullptr;
    }

    return separation;
}

void StemSeparator::discardCacheEntry (const juce::File& track)
{
    getCacheFolder (track).deleteRecursively();
}

void StemSeparator::sweepLeftovers()
{
    for (const auto& folder : cacheRoot.findChildFiles (juce::File::findDirectories, false,
                                                        juce::String ("*") + partialSuffix))
        folder.deleteRecursively();
}

StemOutcome StemSeparator::prepare (juce::AudioFormatManager& formats,
                                    const juce::File& track,
                                    const std::function<void (float)>& onProgress,
                                    const std::atomic<bool>& stop)
{
    if (! isAvailable() || ! track.existsAsFile())
        return StemOutcome::failed;

    if (isCached (track))
    {
        if (onProgress != nullptr)
            onProgress (1.0f);

        return StemOutcome::ready;
    }

    // Nothing here could play it or check the result, so that is found out now
    // rather than after a separator has spent minutes on it.
    if (std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (track)) == nullptr)
        return StemOutcome::failed;

    // Taken before the work rather than after it, so a file changed in the
    // minutes this takes is not stamped as the one that was separated.
    const auto stamp = stampFor (track);
    const auto partial = getPartialFolder (track);

    partial.deleteRecursively();

    if (! partial.createDirectory())
        return StemOutcome::failed;

    auto outcome = separateInto (partial, formats, track, onProgress, stop);

    if (outcome == StemOutcome::ready)
    {
        const auto folder = getCacheFolder (track);

        const auto whole = stemsMatchTrack (formats, partial, track, stop)
                        && partial.getChildFile (stampName).replaceWithText (stamp);

        if (! (whole && folder.deleteRecursively() && partial.moveFileTo (folder)))
            outcome = StemOutcome::failed;
    }

    // Going wrong after being told to stop is what stopping looks like: a killed
    // separator's half-finished output, say, says nothing about the track.
    if (outcome == StemOutcome::failed && stop.load (std::memory_order_relaxed))
        outcome = StemOutcome::stopped;

    if (outcome != StemOutcome::ready)
    {
        partial.deleteRecursively();
        return outcome;
    }

    if (onProgress != nullptr)
        onProgress (1.0f);

    return outcome;
}

StemOutcome StemSeparator::separateInto (const juce::File& partial,
                                         juce::AudioFormatManager& formats,
                                         const juce::File& track,
                                         const std::function<void (float)>& onProgress,
                                         const std::atomic<bool>& stop)
{
    // In process first, where the model is available: no subprocess, no
    // temporary float WAVs, and it works the same on every platform.
    if (hasNativeModel())
    {
        if (const auto decoded = TrackDecoder::decode (formats, track))
            if (const auto separation = separateNatively (decoded->audio, onProgress, stop))
                return writeStems (partial, separation->stems, separation->sampleRate) ? StemOutcome::ready
                                                                                       : StemOutcome::failed;

        // Stopped is not the same as could not. The command would otherwise be
        // the very thing that stop was meant to get off the machine's cores.
        if (stop.load (std::memory_order_relaxed))
            return StemOutcome::stopped;

        // Falling through to the command is deliberate: a model that failed to
        // compile on this machine should not mean no stems at all.
    }

    return runSeparatorCommand (partial, formats, track, onProgress, stop);
}

StemOutcome StemSeparator::runSeparatorCommand (const juce::File& partial,
                                                juce::AudioFormatManager& formats,
                                                const juce::File& track,
                                                const std::function<void (float)>& onProgress,
                                                const std::atomic<bool>& stop)
{
    if (! separatorCommand.existsAsFile())
        return StemOutcome::failed;

    // Inside the partial folder, so all of it goes when that does. In the cache
    // itself, a separator killed part way would leave a folder named after the
    // track that nothing could ever tell from any other.
    const auto output = partial.getChildFile ("work");

    if (! output.createDirectory())
        return StemOutcome::failed;

    juce::StringArray command;
    command.add (separatorCommand.getFullPathName());
    command.add ("run");
    command.add (track.getFullPathName());
    command.add ("-o");
    command.add (output.getFullPathName());
    command.add ("--format");
    command.add ("wav");   // float, so nothing is clipped before the repack below
    command.add ("--device");
    command.add ("auto");

    SeparatorProcess process;

    if (! process.start (command))
        return StemOutcome::failed;

    // The separator reports its own progress, but reading it would tie this to
    // one tool's output. Elapsed time against a rough rate is honest enough for
    // a progress bar and needs no agreement about formatting.
    constexpr auto expectedMs = 60000.0;
    const auto started = juce::Time::getMillisecondCounter();

    while (process.isRunning())
    {
        if (stop.load (std::memory_order_relaxed))
        {
            process.kill();
            return StemOutcome::stopped;
        }

        if (onProgress != nullptr)
        {
            const auto elapsed = (double) (juce::Time::getMillisecondCounter() - started);
            onProgress ((float) juce::jlimit (0.0, 0.99, elapsed / expectedMs));
        }

        juce::Thread::sleep (100);
    }

    if (stop.load (std::memory_order_relaxed))
        return StemOutcome::stopped;

    if (! process.succeeded())
        return StemOutcome::failed;

    // The separator names its output folder after the track it was given, near
    // enough: a name it has cleaned up is still the only folder there.
    auto produced = output.getChildFile (track.getFileNameWithoutExtension());

    if (! produced.isDirectory())
    {
        const auto folders = output.findChildFiles (juce::File::findDirectories, false);

        if (folders.size() != 1)
            return StemOutcome::failed;

        produced = folders.getFirst();
    }

    if (! repackAsFlac (formats, produced, partial, stop))
        return StemOutcome::failed;

    return output.deleteRecursively() ? StemOutcome::ready : StemOutcome::failed;
}

} // namespace opendj
