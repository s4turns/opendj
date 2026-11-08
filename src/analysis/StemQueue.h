/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include "analysis/Stems.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace opendj
{

/** Every separation there is, one at a time, in the order they matter.

    Separating takes every core the machine has for minutes, so two at once would
    starve both and one during a set would be heard. Hence two lanes and one
    worker. A deck that wants stems now goes in the urgent lane, and takes the
    machine from whatever is running for later. Tracks being prepared ahead of
    time go in the background lane, which only moves while no deck is playing: a
    job running when a deck starts is stopped and goes back in the queue, and its
    work is lost, because the separators have nothing to resume from.

    What the work is comes in from outside, which is what lets everything here be
    tested without a model. Nothing here is reached by the audio thread. The
    other public calls may come from any thread, and hold a lock only briefly.
*/
class StemQueue
{
public:
    /** Runs on the worker. It should give up soon after `stop` goes true, and
        answer StemOutcome::stopped when it does. */
    using Work = std::function<StemOutcome (const juce::File& track,
                                            const std::function<void (float)>& onProgress,
                                            const std::atomic<bool>& stop)>;

    /** Told how a request ended, on the worker. */
    using Waiter = std::function<void (StemOutcome)>;

    struct Config
    {
        Work work;

        /** Asked often, off the worker's lock. */
        std::function<bool()> anyDeckPlaying;

        /** The room left for the cache, asked before each background job. */
        std::function<juce::int64()> freeBytes;

        /** Called on the worker when a track's stems are ready, before its
            waiters are told. The job still counts as running until it returns. */
        std::function<void (const juce::File&)> onReady;

        /** Whether a deck has this track loaded, asked off the lock. A deck's
            request for a track no deck holds any more is not worth the machine,
            and a track being prepared that a deck turns out to hold is not
            stopped when the decks start. Left empty, every track is on one. */
        std::function<bool (const juce::File&)> onADeck;
    };

    /** A track costs about 300 MB and nothing is ever evicted, so a long list
        could otherwise fill a disk it shares with the record collection. */
    static constexpr juce::int64 minimumFreeBytes = (juce::int64) 5 * 1024 * 1024 * 1024;

    static constexpr int failuresBeforeHalting = 3;

    /** Why the background lane gave up, and was emptied. */
    enum class Halt { none, failures, noSpace };

    struct Progress
    {
        bool running = false;
        bool runningInBatch = false;    ///< the running job is one of `total`
        bool waitingForDecks = false;
        juce::String currentFile;
        float fraction = 0.0f;

        int total = 0;                  ///< tracks asked for ahead of time
        int finished = 0;               ///< of those, how many are ready
        int failed = 0;                 ///< of those, how many could not be done
        Halt halt = Halt::none;

        /** One line for the browser's status, empty when there is nothing to say. */
        juce::String describe() const;
    };

    explicit StemQueue (Config configToUse);
    ~StemQueue();

    /** A deck wants this track now. It runs next, ahead of anything being
        prepared. A request for the file already running attaches to that job,
        whichever lane it came from, rather than starting it over.

        If no deck holds the track by the time its turn comes, it is dropped,
        and its waiters told StemOutcome::stopped. A track that failed this
        session is told StemOutcome::failed at once, unless the file has changed
        since: one put back on a drive that was not there, or finished copying,
        or the request is `byHand`. A person pressing the button again is asking
        for another try, which automix, asking on its own, never is. */
    void requestNow (const juce::File& track, Waiter waiter = {}, bool byHand = false);

    /** Tracks to have ready for later. Anything already asked for is left where
        it is, and a track that failed this session is left out, on the same
        terms as for a deck. Clears a halt: asking again is how a person says the
        disk has room now. */
    void requestInBackground (const std::vector<juce::File>& tracks);

    /** Forgets everything in the background lane and stops the job if it is one
        of those. A deck's request is left alone. */
    void cancelBackground();

    /** Clears both lanes, stops the job and waits for the worker to finish, so
        nothing it reaches can go away beneath it. Anything still waiting is told
        StemOutcome::stopped, and anything asked for afterwards the same. */
    void shutdown();

    Progress getProgress() const;

    /** True while a deck's request for this track is waiting, or the track is
        being separated for any reason. */
    bool isRequested (const juce::File& track) const;

    /** How far along this track is, or 0 when it is not the one running. */
    float getFractionFor (const juce::File& track) const;

    /** True while prepared tracks are waiting or being worked on. */
    bool isPreparing() const;

private:
    struct Job
    {
        juce::File file;
        std::vector<Waiter> waiters;
        bool urgent = false;
        bool inBatch = false;
    };

    void addUrgent (const juce::File& track, Waiter waiter);
    bool hasFailed (const juce::File& track);
    bool urgentStillWanted (std::unique_lock<std::mutex>& lock);
    void run();
    void watchDecks();
    bool backgroundMayStart (std::unique_lock<std::mutex>& lock);
    void runNextJob (std::unique_lock<std::mutex>& lock);
    std::vector<Waiter> settle (StemOutcome& outcome, const juce::String& stamp);
    void stopRunningJob (bool requeue);
    void haltBackground (Halt reason);
    void startThreadsIfNeeded();
    bool deckIsPlaying() const;

    const Config config;

    mutable std::mutex mutex;
    std::condition_variable workerWake, watcherWake;

    std::deque<Job> urgent, background;
    std::optional<Job> running;
    bool requeueWhenStopped = false;

    // The running job's own, set back to false before each one starts, and only
    // ever written under the lock, so a stop can never land on the job after.
    std::atomic<bool> stop { false };
    std::atomic<float> fraction { 0.0f };

    // Each with the size and date the file had when it failed, so that a file
    // that has changed since is tried again.
    std::map<juce::String, juce::String> failedFiles;

    // The failures since the last success. Enough of them halt the background
    // lane, and are then put down to the machine rather than to the tracks.
    std::vector<juce::String> failureRun;
    int consecutiveFailures = 0;
    int batchTotal = 0, batchFinished = 0, batchFailed = 0;
    Halt halted = Halt::none;
    bool waitingForDecks = false;

    bool threadsStarted = false;
    bool closing = false;
    std::thread worker, watcher;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemQueue)
};

} // namespace opendj
