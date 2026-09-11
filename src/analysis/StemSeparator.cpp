/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "analysis/StemSeparator.h"

#include "analysis/TrackDecoder.h"

namespace opendj
{

namespace
{
    const char* const stemFileNames[numStems] { "drums", "bass", "other", "vocals" };

    /** Written beside the stems so a cache entry describes itself. */
    const char* const manifestName = "opendj-stems.txt";

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
        return track.getFileNameWithoutExtension().retainCharacters (
                   "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_")
             + " [" + digest + "]";
    }
}

StemSeparator::StemSeparator()
    : separatorCommand (findSeparator())
{
}

StemSeparator::~StemSeparator()
{
    cancel();
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
        auto description = "Separating in process, model at " + StemModel::findModel().getFullPathName();

        if (const auto device = model.getDeviceName(); device.isNotEmpty())
            description << " on the " << device;

        if (modelError.isNotEmpty())
            description << ". " << modelError;

        return description;
    }

    if (separatorCommand.existsAsFile())
        return "Separating with " + separatorCommand.getFullPathName();

    return "No stem separator found. Install the htdemucs OpenVINO model, or point "
           "OPENDJ_STEM_MODEL at it; failing that OPENDJ_STEM_SEPARATOR at a command.";
}

std::shared_ptr<const SeparatedTrack> StemSeparator::separateNatively (
    const juce::AudioBuffer<float>& mix, const std::function<void (float)>& onProgress)
{
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

    const auto ok = model.separate (mix, *separation, [this, &onProgress] (float progress)
    {
        if (onProgress != nullptr)
            onProgress (progress);

        return ! cancelled.load (std::memory_order_relaxed);
    });

    return ok && separation->isWellFormed() ? separation : nullptr;
}

juce::File StemSeparator::getCacheFolder (const juce::File& track) const
{
    return cacheRoot.getChildFile (cacheKeyFor (track));
}

std::shared_ptr<const SeparatedTrack> StemSeparator::loadFromCache (juce::AudioFormatManager& formats,
                                                                    const juce::File& track)
{
    const auto folder = getCacheFolder (track);

    if (! folder.isDirectory())
        return nullptr;

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
        auto file = folder.getChildFile (juce::String (stemFileNames[stem]) + ".wav");

        if (! file.existsAsFile())
            file = folder.getChildFile (juce::String (stemFileNames[stem]) + ".flac");

        auto decoded = TrackDecoder::decode (formats, file);

        if (decoded == nullptr)
            return nullptr;

        separation->sampleRate = decoded->sampleRate;
        shortest = juce::jmin (shortest, decoded->audio.getNumSamples());
        separation->stems[(size_t) stem] = std::move (decoded->audio);
    }

    // A stem that decoded a sample or two short of the others would be read
    // past its end by the audio thread, which trusts them to match.
    for (auto& stem : separation->stems)
    {
        stem.setSize (2, shortest, true, true, true);

        if (scale != 1.0f)
            stem.applyGain (1.0f / scale);
    }

    return separation->isWellFormed() ? separation : nullptr;
}

std::shared_ptr<const SeparatedTrack> StemSeparator::separate (juce::AudioFormatManager& formats,
                                                               const juce::File& track,
                                                               std::function<void (float)> onProgress)
{
    if (! isAvailable() || ! track.existsAsFile())
        return nullptr;

    if (auto cached = loadFromCache (formats, track))
        return cached;

    cancelled.store (false, std::memory_order_relaxed);

    const auto folder = getCacheFolder (track);
    folder.getParentDirectory().createDirectory();

    // In process first, where the model is available: no subprocess, no
    // temporary float WAVs, and it works the same on every platform.
    if (hasNativeModel())
    {
        if (auto decoded = TrackDecoder::decode (formats, track))
        {
            if (auto separation = separateNatively (decoded->audio, onProgress))
            {
                writeToCache (folder, *separation);

                if (onProgress != nullptr)
                    onProgress (1.0f);

                return separation;
            }
        }

        // Falling through to the command is deliberate: a model that failed to
        // compile on this machine should not mean no stems at all.
    }

    if (! separatorCommand.existsAsFile())
        return nullptr;

    juce::StringArray command;
    command.add (separatorCommand.getFullPathName());
    command.add ("run");
    command.add (track.getFullPathName());
    command.add ("-o");
    command.add (folder.getParentDirectory().getFullPathName());
    command.add ("--format");
    command.add ("wav");   // float, so nothing is clipped before the repack below
    command.add ("--device");
    command.add ("auto");

    {
        const juce::ScopedLock lock (processLock);
        runningProcess = std::make_unique<juce::ChildProcess>();

        if (! runningProcess->start (command, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            runningProcess.reset();
            return nullptr;
        }
    }

    // The separator reports its own progress, but reading it would tie this to
    // one tool's output. Elapsed time against a rough rate is honest enough for
    // a progress bar and needs no agreement about formatting.
    const auto started = juce::Time::getMillisecondCounter();
    const auto expectedMs = juce::jmax (1000.0, TrackDecoder::maxTrackMinutes * 0.0 + 60000.0);

    while (true)
    {
        {
            const juce::ScopedLock lock (processLock);

            if (runningProcess == nullptr)
                return nullptr;

            if (! runningProcess->isRunning())
                break;

            if (cancelled.load (std::memory_order_relaxed))
            {
                runningProcess->kill();
                runningProcess.reset();
                return nullptr;
            }
        }

        if (onProgress != nullptr)
        {
            const auto elapsed = (double) (juce::Time::getMillisecondCounter() - started);
            onProgress ((float) juce::jlimit (0.0, 0.99, elapsed / expectedMs));
        }

        juce::Thread::sleep (200);
    }

    auto succeeded = false;

    {
        const juce::ScopedLock lock (processLock);

        if (runningProcess != nullptr)
        {
            succeeded = runningProcess->getExitCode() == 0;
            runningProcess.reset();
        }
    }

    if (! succeeded)
        return nullptr;

    // The separator names its output folder after the track it was given, which
    // is not the cache key: two tracks called the same thing in different
    // folders would land on top of each other. Move it to where the cache
    // expects to find it.
    if (const auto produced = folder.getParentDirectory()
                                    .getChildFile (track.getFileNameWithoutExtension());
        produced != folder && produced.isDirectory())
    {
        folder.deleteRecursively();

        if (! produced.moveFileTo (folder))
            return nullptr;
    }

    // The separator writes 32-bit float WAV, which is four bytes a sample and
    // adds up fast: four stems of a seven minute track are close to six hundred
    // megabytes. Repacking them as 24-bit FLAC is lossless for anything a
    // listener can hear and costs about half the space.
    repackAsFlac (formats, folder);

    auto separation = loadFromCache (formats, track);

    if (onProgress != nullptr)
        onProgress (1.0f);

    return separation;
}

void StemSeparator::writeToCache (const juce::File& folder, const SeparatedTrack& separation)
{
    folder.createDirectory();

    const auto scale = sharedScaleFor (separation.stems);
    juce::FlacAudioFormat flac;

    for (int stem = 0; stem < numStems; ++stem)
    {
        juce::AudioBuffer<float> scaled (separation.stems[(size_t) stem]);

        if (scale != 1.0f)
            scaled.applyGain (scale);

        const auto target = folder.getChildFile (juce::String (stemFileNames[stem]) + ".flac");
        target.deleteFile();

        auto stream = std::make_unique<juce::FileOutputStream> (target);

        if (! stream->openedOk())
            return;

        std::unique_ptr<juce::AudioFormatWriter> writer (
            flac.createWriterFor (stream.get(), separation.sampleRate, 2, 24, {}, 0));

        if (writer == nullptr)
            return;

        stream.release();
        writer->writeFromAudioSampleBuffer (scaled, 0, scaled.getNumSamples());
    }

    folder.getChildFile (manifestName).replaceWithText (juce::String (scale, 8));
}

void StemSeparator::repackAsFlac (juce::AudioFormatManager& formats, const juce::File& folder)
{
    std::array<juce::AudioBuffer<float>, numStems> stems;
    auto rate = 44100.0;

    for (int stem = 0; stem < numStems; ++stem)
    {
        const auto wav = folder.getChildFile (juce::String (stemFileNames[stem]) + ".wav");

        if (! wav.existsAsFile())
            return;                 // already repacked, or never written

        auto decoded = TrackDecoder::decode (formats, wav);

        if (decoded == nullptr)
            return;

        rate = decoded->sampleRate;
        stems[(size_t) stem] = std::move (decoded->audio);
    }

    const auto scale = sharedScaleFor (stems);
    juce::FlacAudioFormat flac;

    for (int stem = 0; stem < numStems; ++stem)
    {
        auto& buffer = stems[(size_t) stem];

        if (scale != 1.0f)
            buffer.applyGain (scale);

        const auto target = folder.getChildFile (juce::String (stemFileNames[stem]) + ".flac");
        target.deleteFile();

        auto stream = std::make_unique<juce::FileOutputStream> (target);

        if (! stream->openedOk())
            return;

        std::unique_ptr<juce::AudioFormatWriter> writer (
            flac.createWriterFor (stream.get(), rate, 2, 24, {}, 0));

        if (writer == nullptr)
            return;

        stream.release();           // the writer owns it now
        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }

    // Only once every stem is safely written does the float original go.
    folder.getChildFile (manifestName).replaceWithText (juce::String (scale, 8));

    for (const auto* name : stemFileNames)
        folder.getChildFile (juce::String (name) + ".wav").deleteFile();
}

void StemSeparator::cancel()
{
    cancelled.store (true, std::memory_order_relaxed);

    const juce::ScopedLock lock (processLock);

    if (runningProcess != nullptr && runningProcess->isRunning())
        runningProcess->kill();
}

} // namespace opendj
