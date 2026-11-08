/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "analysis/StemQueue.h"

#include <chrono>
#include <exception>
#include <limits>

namespace opendj
{

namespace
{
    /** How soon a deck starting takes the machine back, and how soon a deck
        stopping gives it up again. */
    constexpr auto deckPollPeriod = std::chrono::milliseconds (100);

    /** Which version of a file failed. A missing file has a stamp of its own,
        so the same file back on its drive is a different one. */
    juce::String stampOf (const juce::File& file)
    {
        return juce::String (file.getSize()) + " " + juce::String (file.getLastModificationTime().toMilliseconds());
    }
}

juce::String StemQueue::Progress::describe() const
{
    juce::String text;

    const auto ofTotal = [this]
    {
        juce::String position;
        position << " " << finished + failed + 1 << " of " << total;

        if (failed > 0)
            position << ", " << failed << " failed";

        return position;
    };

    // A halt says what was done before it, or the counts would be lost behind it.
    const auto soFar = [this]
    {
        if (total <= 0)
            return juce::String();

        juce::String done;
        done << " (" << finished << " of " << total << " ready";

        if (failed > 0)
            done << ", " << failed << " failed";

        return done + ")";
    };

    if (halt == Halt::failures)
        text << "Stems stopped after " << failuresBeforeHalting << " failures in a row" << soFar();
    else if (halt == Halt::noSpace)
        text << "Stems stopped: under " << (int) (minimumFreeBytes / (1024 * 1024 * 1024)) << " GB free" << soFar();
    else if (running)
        text << "Stems" << (runningInBatch ? ofTotal() : juce::String()) << ": " << currentFile
             << " " << juce::roundToInt (fraction * 100.0f) << "%";
    else if (finished + failed < total)
        text << "Stems" << ofTotal() << (waitingForDecks ? ", waiting for the decks to stop" : "");
    else if (total > 0)
        text << "Stems ready for " << finished << " of " << total
             << (failed > 0 ? ", " + juce::String (failed) + " failed" : juce::String());

    return text;
}

StemQueue::StemQueue (Config configToUse)
    : config (std::move (configToUse))
{
}

StemQueue::~StemQueue()
{
    shutdown();
}

void StemQueue::startThreadsIfNeeded()
{
    // Not until something is asked for: a session that never touches stems
    // should cost nothing for their having been possible.
    if (threadsStarted || closing)
        return;

    threadsStarted = true;

    worker = std::thread ([this]
    {
        juce::Thread::setCurrentThreadName ("Stem queue");
        run();
    });

    watcher = std::thread ([this]
    {
        juce::Thread::setCurrentThreadName ("Stem queue watcher");
        watchDecks();
    });
}

bool StemQueue::deckIsPlaying() const
{
    return config.anyDeckPlaying != nullptr && config.anyDeckPlaying();
}

void StemQueue::stopRunningJob (bool requeue)
{
    // A stop already on its way keeps the answer it was given.
    if (! running.has_value() || stop.load (std::memory_order_relaxed))
        return;

    requeueWhenStopped = requeue;
    stop.store (true, std::memory_order_relaxed);
}

void StemQueue::haltBackground (Halt reason)
{
    background.clear();
    halted = reason;
    waitingForDecks = false;
}

void StemQueue::requestNow (const juce::File& track, Waiter waiter, bool byHand)
{
    auto answer = StemOutcome::stopped;

    {
        const std::lock_guard<std::mutex> lock (mutex);

        if (! closing)
        {
            if (byHand)
                failedFiles.erase (track.getFullPathName());

            if (! hasFailed (track))
            {
                addUrgent (track, std::move (waiter));
                return;
            }

            answer = StemOutcome::failed;
        }
    }

    // Told with the lock down, as every waiter is.
    if (waiter != nullptr)
        waiter (answer);
}

bool StemQueue::hasFailed (const juce::File& track)
{
    const auto found = failedFiles.find (track.getFullPathName());

    if (found == failedFiles.end())
        return false;

    if (found->second == stampOf (track))
        return true;

    failedFiles.erase (found);
    return false;
}

void StemQueue::addUrgent (const juce::File& track, Waiter waiter)
{
    const auto attach = [&waiter] (Job& job)
    {
        job.urgent = true;

        if (waiter != nullptr)
            job.waiters.push_back (std::move (waiter));
    };

    if (running.has_value() && running->file == track)
    {
        attach (*running);

        // A cancel may have this one on its way out already. Somebody wants it
        // after all, so it has to come back.
        if (stop.load (std::memory_order_relaxed))
            requeueWhenStopped = true;

        return;
    }

    for (auto& job : urgent)
    {
        if (job.file == track)
        {
            attach (job);
            return;
        }
    }

    Job job;
    job.file = track;

    // Already waiting to be prepared: it keeps its place in the count and leaves
    // the lane, so it cannot run twice.
    for (auto it = background.begin(); it != background.end(); ++it)
    {
        if (it->file == track)
        {
            job = std::move (*it);
            background.erase (it);
            break;
        }
    }

    attach (job);
    urgent.push_back (std::move (job));

    if (running.has_value() && ! running->urgent)
        stopRunningJob (true);

    startThreadsIfNeeded();
    workerWake.notify_all();
}

void StemQueue::requestInBackground (const std::vector<juce::File>& tracks)
{
    const std::lock_guard<std::mutex> lock (mutex);

    if (closing)
        return;

    // A new list after the last one finished starts a new count.
    if (background.empty() && ! (running.has_value() && running->inBatch))
    {
        batchTotal = batchFinished = batchFailed = 0;

        for (auto& job : urgent)
            job.inBatch = false;
    }

    halted = Halt::none;
    consecutiveFailures = 0;
    failureRun.clear();

    // A selection can be the whole library, and on the message thread, so this
    // is not a walk of the lanes for every track.
    std::set<juce::String> known;

    for (const auto& job : urgent)
        known.insert (job.file.getFullPathName());

    for (const auto& job : background)
        known.insert (job.file.getFullPathName());

    if (running.has_value())
        known.insert (running->file.getFullPathName());

    for (const auto& track : tracks)
    {
        const auto path = track.getFullPathName();

        // A cancel may have the running job on its way out, to be forgotten.
        // Asked for again, it comes back to the front of the lane instead, and
        // counts in the new list like anything else in it.
        if (running.has_value() && running->file == track && ! running->urgent
            && stop.load (std::memory_order_relaxed) && ! requeueWhenStopped && ! closing)
        {
            requeueWhenStopped = true;

            if (! running->inBatch)
            {
                running->inBatch = true;
                ++batchTotal;
            }

            continue;
        }

        if (! known.insert (path).second || hasFailed (track))
            continue;

        Job job;
        job.file = track;
        job.inBatch = true;
        background.push_back (std::move (job));
        ++batchTotal;
    }

    startThreadsIfNeeded();
    workerWake.notify_all();
}

void StemQueue::cancelBackground()
{
    const std::lock_guard<std::mutex> lock (mutex);

    background.clear();
    halted = Halt::none;
    waitingForDecks = false;
    batchTotal = batchFinished = batchFailed = 0;

    for (auto& job : urgent)
        job.inBatch = false;

    if (running.has_value())
    {
        running->inBatch = false;

        if (! running->urgent)
        {
            stopRunningJob (false);

            // A stop already on its way, because a deck started or asked for
            // something else, was going to put this back in the lane. Cancelled,
            // it is not going anywhere.
            requeueWhenStopped = false;
        }
    }
}

void StemQueue::shutdown()
{
    std::vector<Waiter> waiters;

    {
        const std::lock_guard<std::mutex> lock (mutex);

        closing = true;

        for (auto* lane : { &urgent, &background })
        {
            for (auto& job : *lane)
                for (auto& waiter : job.waiters)
                    waiters.push_back (std::move (waiter));

            lane->clear();
        }

        // Past the guard in stopRunningJob: whatever an earlier stop meant to
        // happen next, nothing is going to now.
        requeueWhenStopped = false;
        stop.store (running.has_value(), std::memory_order_relaxed);

        workerWake.notify_all();
        watcherWake.notify_all();
    }

    if (worker.joinable())
        worker.join();

    if (watcher.joinable())
        watcher.join();

    for (auto& waiter : waiters)
        waiter (StemOutcome::stopped);
}

StemQueue::Progress StemQueue::getProgress() const
{
    const std::lock_guard<std::mutex> lock (mutex);

    Progress progress;
    progress.running = running.has_value();
    progress.runningInBatch = running.has_value() && running->inBatch;
    progress.waitingForDecks = waitingForDecks;
    progress.currentFile = running.has_value() ? running->file.getFileNameWithoutExtension() : juce::String();
    progress.fraction = fraction.load (std::memory_order_relaxed);
    progress.total = batchTotal;
    progress.finished = batchFinished;
    progress.failed = batchFailed;
    progress.halt = halted;
    return progress;
}

bool StemQueue::isRequested (const juce::File& track) const
{
    const std::lock_guard<std::mutex> lock (mutex);

    if (running.has_value() && running->file == track)
        return true;

    for (const auto& job : urgent)
        if (job.file == track)
            return true;

    return false;
}

float StemQueue::getFractionFor (const juce::File& track) const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return running.has_value() && running->file == track ? fraction.load (std::memory_order_relaxed) : 0.0f;
}

bool StemQueue::isPreparing() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return ! background.empty() || (running.has_value() && ! running->urgent);
}

void StemQueue::run()
{
    std::unique_lock<std::mutex> lock (mutex);

    while (! closing)
    {
        if (urgent.empty() && background.empty())
        {
            waitingForDecks = false;
            workerWake.wait (lock);
            continue;
        }

        if (urgent.empty() ? ! backgroundMayStart (lock) : ! urgentStillWanted (lock))
            continue;

        runNextJob (lock);
    }
}

bool StemQueue::urgentStillWanted (std::unique_lock<std::mutex>& lock)
{
    if (config.onADeck == nullptr)
        return true;

    // A deck can ask and then move on, and automix asks for every track it
    // lines up. Nothing else takes a request back, and each one is minutes of
    // every core, during a set, ahead of the track a deck does have.
    const auto file = urgent.front().file;

    lock.unlock();
    const auto held = config.onADeck (file);
    lock.lock();

    // Anything may have changed while the lock was down.
    if (closing || urgent.empty() || urgent.front().file != file)
        return false;

    if (held)
        return true;

    auto job = std::move (urgent.front());
    urgent.pop_front();

    const auto waiters = std::move (job.waiters);
    job.waiters.clear();

    // Asked for ahead of time as well, it goes back to wait for the decks.
    if (job.inBatch)
    {
        job.urgent = false;
        background.push_front (std::move (job));
    }

    lock.unlock();

    for (const auto& waiter : waiters)
        waiter (StemOutcome::stopped);

    lock.lock();
    return false;
}

bool StemQueue::backgroundMayStart (std::unique_lock<std::mutex>& lock)
{
    // Asked with the lock down. Free space can mean waiting on a disk that has
    // gone to sleep, and the interface is reading the progress in the meantime.
    lock.unlock();

    const auto playing = deckIsPlaying();
    auto freeBytes = std::numeric_limits<juce::int64>::max();

    if (! playing && config.freeBytes != nullptr)
        freeBytes = config.freeBytes();

    lock.lock();

    // Anything may have changed while the lock was down, and the caller starts
    // again from the top when told no.
    if (closing || ! urgent.empty() || background.empty())
        return false;

    waitingForDecks = playing;

    if (playing)
    {
        workerWake.wait_for (lock, deckPollPeriod);
        return false;
    }

    if (freeBytes < minimumFreeBytes)
    {
        haltBackground (Halt::noSpace);
        return false;
    }

    return true;
}

void StemQueue::runNextJob (std::unique_lock<std::mutex>& lock)
{
    auto& lane = urgent.empty() ? background : urgent;

    running = std::move (lane.front());
    lane.pop_front();

    stop.store (false, std::memory_order_relaxed);
    requeueWhenStopped = false;
    waitingForDecks = false;
    fraction.store (0.0f, std::memory_order_relaxed);

    const auto file = running->file;
    watcherWake.notify_all();
    lock.unlock();

    auto outcome = StemOutcome::failed;

    // Running out of memory on one long track should cost that track, not the
    // whole application.
    try
    {
        outcome = config.work (file,
                               [this] (float progress) { fraction.store (progress, std::memory_order_relaxed); },
                               stop);

        // Handing stems to the decks is a decode of four whole stems, and
        // shutting down waits for this thread. Nothing is going to play them.
        const auto closingNow = [this]
        {
            const std::lock_guard<std::mutex> closingLock (mutex);
            return closing;
        };

        if (outcome == StemOutcome::ready && config.onReady != nullptr && ! closingNow())
            config.onReady (file);
    }
    catch (const std::exception&)
    {
        outcome = StemOutcome::failed;
    }

    // Off the lock: the file may be on a drive that has to wake up.
    const auto stamp = outcome == StemOutcome::ready ? juce::String() : stampOf (file);

    lock.lock();
    const auto waiters = settle (outcome, stamp);
    lock.unlock();

    for (const auto& waiter : waiters)
        waiter (outcome);

    lock.lock();
}

std::vector<StemQueue::Waiter> StemQueue::settle (StemOutcome& outcome, const juce::String& stamp)
{
    auto job = std::move (*running);
    running.reset();

    // Stopping when nobody asked is a failure wearing the wrong name, and taking
    // it at its word could run the same track forever.
    if (outcome == StemOutcome::stopped && ! stop.load (std::memory_order_relaxed))
        outcome = StemOutcome::failed;

    if (outcome == StemOutcome::stopped)
    {
        if (requeueWhenStopped && ! closing)
        {
            // Back to the front, so it is the next thing done once the way is clear.
            if (job.urgent)
                urgent.push_back (std::move (job));
            else
                background.push_front (std::move (job));

            return {};
        }

        return std::move (job.waiters);
    }

    if (outcome == StemOutcome::ready)
    {
        consecutiveFailures = 0;
        failureRun.clear();
        batchFinished += job.inBatch ? 1 : 0;
    }
    else
    {
        const auto path = job.file.getFullPathName();
        failedFiles[path] = stamp;
        failureRun.push_back (path);
        batchFailed += job.inBatch ? 1 : 0;

        // Three in a row says something about the machine rather than the
        // tracks, and there may be a thousand more behind them. Since it is the
        // machine, the tracks are not held against it: asking again, once the
        // disk has room or whatever it was is put right, tries them again.
        if (++consecutiveFailures >= failuresBeforeHalting)
        {
            haltBackground (Halt::failures);

            for (const auto& failed : failureRun)
                failedFiles.erase (failed);

            failureRun.clear();
        }
    }

    return std::move (job.waiters);
}

void StemQueue::watchDecks()
{
    std::unique_lock<std::mutex> lock (mutex);

    while (! closing)
    {
        // Nothing to keep an eye on until a background job starts, and the
        // worker says when one does.
        if (! running.has_value() || running->urgent || stop.load (std::memory_order_relaxed))
        {
            watcherWake.wait (lock);
            continue;
        }

        watcherWake.wait_for (lock, deckPollPeriod);

        if (! running.has_value() || running->urgent)
            continue;

        const auto file = running->file;

        lock.unlock();
        const auto playing = deckIsPlaying();
        const auto onADeck = playing && config.onADeck != nullptr && config.onADeck (file);
        lock.lock();

        if (! playing || ! running.has_value() || running->urgent || running->file != file
            || stop.load (std::memory_order_relaxed))
            continue;

        // The track on a deck that has just started is the one track this must
        // not throw away: its button has been showing the progress as its own.
        // It goes on as though that deck had asked.
        if (onADeck)
            running->urgent = true;
        else
            stopRunningJob (true);
    }
}

} // namespace opendj
