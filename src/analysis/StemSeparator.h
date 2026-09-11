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
*/
class StemSeparator
{
public:
    StemSeparator();
    ~StemSeparator();

    /** True when stems are possible at all, by either route. */
    bool isAvailable() const { return hasNativeModel() || separatorCommand.existsAsFile(); }

    /** True when the model runs in this process rather than as a command. */
    bool hasNativeModel() const { return StemModel::findModel().existsAsFile(); }

    /** Where the separator was found, or why there is none, for the interface. */
    juce::String getStatusDescription() const;

    /** Stems for a track that has been separated before, or null. Cheap enough
        to call on a load: it reads the cache and decodes, nothing more. */
    std::shared_ptr<const SeparatedTrack> loadFromCache (juce::AudioFormatManager& formats,
                                                         const juce::File& track);

    /** Separates a track and caches the result. Long and blocking, so it wants
        a background thread. Returns null if the separator failed or was
        stopped. Progress is reported as a fraction, from any thread. */
    std::shared_ptr<const SeparatedTrack> separate (juce::AudioFormatManager& formats,
                                                    const juce::File& track,
                                                    std::function<void (float)> onProgress = {});

    /** Asks a separation in progress to give up. */
    void cancel();

    /** Where a track's stems live. */
    juce::File getCacheFolder (const juce::File& track) const;

    static juce::File defaultCacheRoot();

private:
    juce::File findSeparator() const;

    /** Rewrites a freshly separated folder's float WAVs as 24-bit FLAC, at one
        shared scale so the stems still sum back to the track. */
    void repackAsFlac (juce::AudioFormatManager& formats, const juce::File& folder);

    /** Writes a separation straight to the cache as 24-bit FLAC. */
    void writeToCache (const juce::File& folder, const SeparatedTrack& separation);

    /** Separates in this process. Loaded on first use, since compiling the
        model for a GPU takes seconds and most sessions never ask for stems. */
    std::shared_ptr<const SeparatedTrack> separateNatively (const juce::AudioBuffer<float>& mix,
                                                            const std::function<void (float)>& onProgress);

    StemModel model;
    bool modelLoadAttempted = false;
    juce::String modelError;
    juce::CriticalSection modelLock;

    const juce::File separatorCommand;
    juce::File cacheRoot { defaultCacheRoot() };

    std::atomic<bool> cancelled { false };
    std::unique_ptr<juce::ChildProcess> runningProcess;
    juce::CriticalSection processLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemSeparator)
};

} // namespace opendj
