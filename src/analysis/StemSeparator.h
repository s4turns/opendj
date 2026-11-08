/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include "analysis/StemModel.h"
#include "analysis/Stems.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <functional>
#include <memory>

namespace opendj
{

/** Separates a track into its four parts, and remembers that it did.

    Separation is slow -- around a minute for a seven minute track on a machine
    with a GPU worth using -- so it happens once, in the background, and the
    result is kept on disk. A track played a second time has its stems already.

    The work itself is done by an external separator rather than in this
    process. That is deliberate for now: the model, the runtime and the
    hundred megabytes of weights are a large thing to require of everyone who
    builds OpenDJ, and keeping it behind a command means a machine without any
    of it still builds and runs, simply with no stems.

    A cache entry is only ever complete. Everything is written to a folder beside
    the entry's own, checked, and renamed into place, so a load can come at any
    moment and a crash can leave nothing worse than a folder that is swept at the
    next start. StemQueue decides when anything runs; this decides how.
*/
class StemSeparator
{
public:
    /** Whether a model installed on this machine may be used. Tests say no, so
        that a stand-in separator is what actually runs on any machine. */
    enum class Models { useInstalled, ignoreInstalled };

    explicit StemSeparator (Models modelsToUse = Models::useInstalled);
    ~StemSeparator();

    /** True when stems are possible at all, by either route. */
    bool isAvailable() const { return hasNativeModel() || separatorCommand.existsAsFile(); }

    /** True when the model runs in this process rather than as a command. */
    bool hasNativeModel() const;

    /** Where the separator was found, or why there is none, for the interface. */
    juce::String getStatusDescription() const;

    /** True when a track has a cache entry that is still about this file. It
        only looks at file sizes and dates, so it does not decode anything. */
    bool isCached (const juce::File& track) const;

    /** Stems for a track that has been separated before, or null. Cheap enough
        to call on a load: it reads the cache and decodes, nothing more.

        `broken`, if given, says whether a null was the entry's fault: there,
        and not decoding. Not when there was no entry, or no memory for it. */
    std::shared_ptr<const SeparatedTrack> loadFromCache (juce::AudioFormatManager& formats,
                                                         const juce::File& track,
                                                         bool* broken = nullptr);

    /** How many times an entry has been decoded, which is hundreds of megabytes
        each. For the tests, which have no other way to see it happen. */
    int getCacheDecodeCount() const noexcept { return cacheDecodes.load (std::memory_order_relaxed); }

    /** Makes sure a track has a cache entry. Long and blocking, so it wants a
        background thread. Keeps no audio afterwards: a list of tracks prepared
        ahead of time costs disk, and memory only for the one being worked on.

        Gives up soon after `stop` goes true. Progress is reported as a fraction,
        from the calling thread. */
    StemOutcome prepare (juce::AudioFormatManager& formats,
                         const juce::File& track,
                         const std::function<void (float)>& onProgress,
                         const std::atomic<bool>& stop);

    /** Removes a track's entry, for one that says it is there and will not decode. */
    void discardCacheEntry (const juce::File& track);

    /** Removes what a job killed part way left behind. For startup, when no job
        can be running; a job clears its own folder before it starts anyway. */
    void sweepLeftovers();

    /** The room left where the cache lives. */
    juce::int64 getFreeBytes() const;

    /** Answers getFreeBytes instead of the disk. For tests, so that whether they
        run does not depend on how full the machine running them is. Set it
        before anything is asked of the separator. */
    void setFreeSpaceProbe (std::function<juce::int64()> probe) { freeSpaceProbe = std::move (probe); }

    /** Where a track's stems live. */
    juce::File getCacheFolder (const juce::File& track) const;

    /** Where a track's stems are put together, until they are known to be whole. */
    juce::File getPartialFolder (const juce::File& track) const;

    static juce::File defaultCacheRoot();

    /** Stands in for the in-process model: gets the mix and a progress callback
        that answers false when it should give up, as StemModel::separate does,
        and returns null if it could not do it. Lets a test reach that route on a
        machine with no model. */
    using StandInModel = std::function<std::shared_ptr<const SeparatedTrack> (
        const juce::AudioBuffer<float>& mix, const std::function<bool (float)>& onProgress)>;

    void useStandInModel (StandInModel standIn) { standInModel = std::move (standIn); }

private:
    juce::File findSeparator() const;

    /** Writes a track's stems into a partial folder, by whichever route works. */
    StemOutcome separateInto (const juce::File& partial,
                              juce::AudioFormatManager& formats,
                              const juce::File& track,
                              const std::function<void (float)>& onProgress,
                              const std::atomic<bool>& stop);

    StemOutcome runSeparatorCommand (const juce::File& partial,
                                     juce::AudioFormatManager& formats,
                                     const juce::File& track,
                                     const std::function<void (float)>& onProgress,
                                     const std::atomic<bool>& stop);

    /** Separates in this process. Loaded on first use, since compiling the
        model for a GPU takes seconds and most sessions never ask for stems. */
    std::shared_ptr<const SeparatedTrack> separateNatively (const juce::AudioBuffer<float>& mix,
                                                            const std::function<void (float)>& onProgress,
                                                            const std::atomic<bool>& stop);

    const bool useInstalledModels;
    StandInModel standInModel;

    StemModel model;
    bool modelLoadAttempted = false;
    juce::String modelError;
    mutable juce::CriticalSection modelLock;

    const juce::File separatorCommand;
    juce::File cacheRoot { defaultCacheRoot() };
    std::function<juce::int64()> freeSpaceProbe;

    std::atomic<int> cacheDecodes { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemSeparator)
};

} // namespace opendj
