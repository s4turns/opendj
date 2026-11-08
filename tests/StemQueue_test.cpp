/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "analysis/StemQueue.h"
#include "analysis/StemSeparator.h"
#include "control/ActionDispatcher.h"
#include "core/AudioEngine.h"
#include "ui/StemPrepareMenu.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#if ! JUCE_WINDOWS
 #include <csignal>
 #include <cstdlib>
 #include <unistd.h>
#endif

using Catch::Matchers::WithinAbs;
using opendj::StemOutcome;
using opendj::StemQueue;
using opendj::StemSeparator;

namespace
{
    /** Waits on something another thread is going to do. Long enough for a slow
        machine; a test only sits it out when it is already failing. */
    bool eventually (const std::function<bool()>& condition, int limitMs = 20000)
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) limitMs;

        while (juce::Time::getMillisecondCounter() < end)
        {
            if (condition())
                return true;

            juce::Thread::sleep (2);
        }

        return condition();
    }

    /** A while, for something that must not happen: several times the period the
        queue looks at the decks. */
    void giveItTheChanceToGoWrong()
    {
        juce::Thread::sleep (400);
    }

    juce::File track (const juce::String& name)
    {
        return juce::File ("/stand-in/" + name + ".flac");
    }

    /** Work with no model and no files. It records what was started and in what
        order, and answers what it is told to. A track marked held runs until it
        is stopped or let go. */
    struct StandIn
    {
        /** Unless asked to know about decks, it leaves the queue to think every
            track is on one, as a queue with no such question does. */
        explicit StandIn (bool knowsDecks = false)
        {
            StemQueue::Config config;

            config.work = [this] (const juce::File& file, const std::function<void (float)>& onProgress,
                                  const std::atomic<bool>& stop)
            {
                const auto name = file.getFileNameWithoutExtension();

                {
                    const std::lock_guard<std::mutex> lock (mutex);
                    started.push_back (name);
                }

                onProgress (0.25f);

                while (isHeld (name))
                {
                    if (stop.load())
                        return StemOutcome::stopped;

                    juce::Thread::sleep (1);
                }

                const std::lock_guard<std::mutex> lock (mutex);
                const auto found = answers.find (name);
                return found != answers.end() ? found->second : StemOutcome::ready;
            };

            config.anyDeckPlaying = [this] { return deckPlaying.load(); };
            config.freeBytes = [this] { return freeBytes.load(); };

            config.onReady = [this] (const juce::File& file)
            {
                const std::lock_guard<std::mutex> lock (mutex);
                ready.push_back (file.getFileNameWithoutExtension());
            };

            if (knowsDecks)
            {
                config.onADeck = [this] (const juce::File& file)
                {
                    const std::lock_guard<std::mutex> lock (mutex);
                    return onDecks.count (file.getFileNameWithoutExtension()) != 0;
                };
            }

            queue = std::make_unique<StemQueue> (std::move (config));
        }

        bool isHeld (const juce::String& name)
        {
            const std::lock_guard<std::mutex> lock (mutex);
            return held.count (name) != 0;
        }

        void hold (const juce::String& name)
        {
            const std::lock_guard<std::mutex> lock (mutex);
            held.insert (name);
        }

        void letGo (const juce::String& name)
        {
            const std::lock_guard<std::mutex> lock (mutex);
            held.erase (name);
        }

        /** From now on, only these tracks are on a deck. */
        void decksHold (std::set<juce::String> names)
        {
            const std::lock_guard<std::mutex> lock (mutex);
            onDecks = std::move (names);
        }

        void answer (const juce::String& name, StemOutcome outcome)
        {
            const std::lock_guard<std::mutex> lock (mutex);
            answers[name] = outcome;
        }

        /** What has been started so far, in order, as one string. */
        juce::String order()
        {
            const std::lock_guard<std::mutex> lock (mutex);
            juce::StringArray names;

            for (const auto& name : started)
                names.add (name);

            return names.joinIntoString (" ");
        }

        int startsOf (const juce::String& name)
        {
            const std::lock_guard<std::mutex> lock (mutex);
            return (int) std::count (started.begin(), started.end(), name);
        }

        bool isFinished()
        {
            return ! queue->isPreparing() && ! queue->getProgress().running;
        }

        std::mutex mutex;
        std::vector<juce::String> started, ready;
        std::set<juce::String> held, onDecks;
        std::map<juce::String, StemOutcome> answers;
        std::atomic<bool> deckPlaying { false };
        std::atomic<juce::int64> freeBytes { StemQueue::minimumFreeBytes * 2 };
        std::unique_ptr<StemQueue> queue;
    };

    /** A waiter, and what it was told. */
    struct Told
    {
        StemQueue::Waiter waiter()
        {
            return [this] (StemOutcome outcome)
            {
                answer.store ((int) outcome);
                ++times;
            };
        }

        bool heard (StemOutcome outcome) const { return times.load() > 0 && answer.load() == (int) outcome; }

        std::atomic<int> answer { -1 }, times { 0 };
    };
}

//==============================================================================
// The queue's own decisions, with stand-in work.

TEST_CASE ("a deck's request stops a background job, runs first, and the job runs next", "[stems][queue]")
{
    StandIn work;
    work.hold ("prepared");

    work.queue->requestInBackground ({ track ("prepared"), track ("later") });
    REQUIRE (eventually ([&] { return work.startsOf ("prepared") == 1; }));

    Told told;
    work.queue->requestNow (track ("deck"), told.waiter());

    REQUIRE (eventually ([&] { return told.heard (StemOutcome::ready); }));

    // Let the stopped job through when it comes round again.
    work.letGo ("prepared");

    REQUIRE (eventually ([&] { return work.isFinished(); }));
    REQUIRE (work.order() == "prepared deck prepared later");
}

TEST_CASE ("a deck's request waits its turn behind another deck's", "[stems][queue]")
{
    StandIn work;
    work.hold ("first");

    Told first, second;
    work.queue->requestNow (track ("first"), first.waiter());
    REQUIRE (eventually ([&] { return work.startsOf ("first") == 1; }));

    work.queue->requestNow (track ("second"), second.waiter());
    giveItTheChanceToGoWrong();

    // Stopping one deck for the other would lose minutes and gain nothing.
    REQUIRE (work.order() == "first");
    REQUIRE (first.times.load() == 0);

    work.letGo ("first");

    REQUIRE (eventually ([&] { return second.heard (StemOutcome::ready); }));
    REQUIRE (first.heard (StemOutcome::ready));
    REQUIRE (work.order() == "first second");
}

TEST_CASE ("asking for the file that is running attaches to it rather than starting over", "[stems][queue]")
{
    StandIn work;
    work.hold ("song");

    work.queue->requestInBackground ({ track ("song") });
    REQUIRE (eventually ([&] { return work.startsOf ("song") == 1; }));

    Told first, second;
    work.queue->requestNow (track ("song"), first.waiter());
    work.queue->requestNow (track ("song"), second.waiter());
    giveItTheChanceToGoWrong();

    REQUIRE (work.startsOf ("song") == 1);
    REQUIRE (first.times.load() == 0);

    // It is the deck's now, so a deck starting cannot stop it either.
    work.deckPlaying.store (true);
    giveItTheChanceToGoWrong();

    work.letGo ("song");

    REQUIRE (eventually ([&] { return first.heard (StemOutcome::ready) && second.heard (StemOutcome::ready); }));
    REQUIRE (work.startsOf ("song") == 1);
}

TEST_CASE ("a request for a queued file takes it out of the background lane", "[stems][queue]")
{
    StandIn work;
    work.hold ("running");

    work.queue->requestInBackground ({ track ("running"), track ("a"), track ("b") });
    REQUIRE (eventually ([&] { return work.startsOf ("running") == 1; }));

    Told told;
    work.queue->requestNow (track ("b"), told.waiter());
    REQUIRE (eventually ([&] { return told.heard (StemOutcome::ready); }));

    work.letGo ("running");
    REQUIRE (eventually ([&] { return work.isFinished(); }));

    REQUIRE (work.order() == "running b running a");
    REQUIRE (work.queue->getProgress().total == 3);
    REQUIRE (work.queue->getProgress().finished == 3);
}

TEST_CASE ("nothing in the background starts while a deck plays", "[stems][queue]")
{
    StandIn work;
    work.deckPlaying.store (true);

    work.queue->requestInBackground ({ track ("prepared") });
    giveItTheChanceToGoWrong();

    REQUIRE (work.startsOf ("prepared") == 0);
    REQUIRE (work.queue->getProgress().waitingForDecks);

    // A deck's own request is not held back by that.
    Told told;
    work.queue->requestNow (track ("deck"), told.waiter());
    REQUIRE (eventually ([&] { return told.heard (StemOutcome::ready); }));
    REQUIRE (work.startsOf ("prepared") == 0);

    work.deckPlaying.store (false);
    REQUIRE (eventually ([&] { return work.startsOf ("prepared") == 1; }));
    REQUIRE (eventually ([&] { return work.isFinished(); }));
    REQUIRE (! work.queue->getProgress().waitingForDecks);
}

TEST_CASE ("a background job stops when a deck starts, and runs again when it stops", "[stems][queue]")
{
    StandIn work;
    work.hold ("prepared");

    work.queue->requestInBackground ({ track ("prepared"), track ("later") });
    REQUIRE (eventually ([&] { return work.startsOf ("prepared") == 1; }));

    work.deckPlaying.store (true);
    REQUIRE (eventually ([&] { return ! work.queue->getProgress().running; }));
    giveItTheChanceToGoWrong();

    // Stopped, not started again, and nothing else taking its place.
    REQUIRE (work.order() == "prepared");
    REQUIRE (work.queue->isPreparing());

    work.letGo ("prepared");
    work.deckPlaying.store (false);

    REQUIRE (eventually ([&] { return work.isFinished(); }));
    REQUIRE (work.order() == "prepared prepared later");
}

TEST_CASE ("a deck starting does not stop a deck's request", "[stems][queue]")
{
    StandIn work;
    work.hold ("deck");

    Told told;
    work.queue->requestNow (track ("deck"), told.waiter());
    REQUIRE (eventually ([&] { return work.startsOf ("deck") == 1; }));

    work.deckPlaying.store (true);
    giveItTheChanceToGoWrong();

    REQUIRE (work.queue->getProgress().running);
    REQUIRE (told.times.load() == 0);

    work.letGo ("deck");
    REQUIRE (eventually ([&] { return told.heard (StemOutcome::ready); }));
    REQUIRE (work.startsOf ("deck") == 1);
}

TEST_CASE ("three failures in a row halt the background lane", "[stems][queue]")
{
    StandIn work;

    for (const auto* name : { "a", "b", "c", "d", "e" })
        work.answer (name, StemOutcome::failed);

    work.queue->requestInBackground ({ track ("a"), track ("b"), track ("c"), track ("d"), track ("e") });

    REQUIRE (eventually ([&] { return work.queue->getProgress().halt == StemQueue::Halt::failures; }));
    REQUIRE (eventually ([&] { return work.isFinished(); }));
    giveItTheChanceToGoWrong();

    REQUIRE (work.order() == "a b c");
    REQUIRE (work.queue->getProgress().failed == 3);
    REQUIRE (work.queue->getProgress().describe().contains ("3 failures"));

    // Asking again is how a person says to try, once whatever was wrong with
    // the machine is put right. Three in a row were put down to the machine and
    // not to the tracks, so those are tried again too.
    work.answer ("a", StemOutcome::ready);
    work.answer ("d", StemOutcome::ready);
    work.queue->requestInBackground ({ track ("a"), track ("d") });

    REQUIRE (eventually ([&] { return work.startsOf ("d") == 1; }));
    REQUIRE (eventually ([&] { return work.isFinished(); }));
    REQUIRE (work.queue->getProgress().halt == StemQueue::Halt::none);
    REQUIRE (work.order() == "a b c a d");
    REQUIRE (work.queue->getProgress().finished == 2);
}

TEST_CASE ("a success between failures starts the count again", "[stems][queue]")
{
    StandIn work;

    for (const auto* name : { "a", "b", "d", "e" })
        work.answer (name, StemOutcome::failed);

    work.queue->requestInBackground ({ track ("a"), track ("b"), track ("c"), track ("d"), track ("e"), track ("f") });

    REQUIRE (eventually ([&] { return work.startsOf ("f") == 1; }));
    REQUIRE (eventually ([&] { return work.isFinished(); }));
    REQUIRE (work.queue->getProgress().halt == StemQueue::Halt::none);
    REQUIRE (work.queue->getProgress().finished == 2);
    REQUIRE (work.queue->getProgress().failed == 4);
}

TEST_CASE ("a stopped job counts for nothing and is not treated as failed", "[stems][queue]")
{
    StandIn work;
    work.hold ("prepared");

    work.queue->requestInBackground ({ track ("prepared") });

    // Far more stops than would halt the lane if they were counted.
    for (int stops = 1; stops <= StemQueue::failuresBeforeHalting + 2; ++stops)
    {
        REQUIRE (eventually ([&] { return work.startsOf ("prepared") == stops; }));
        work.deckPlaying.store (true);
        REQUIRE (eventually ([&] { return ! work.queue->getProgress().running; }));
        work.deckPlaying.store (false);
    }

    REQUIRE (eventually ([&] { return work.startsOf ("prepared") == StemQueue::failuresBeforeHalting + 3; }));

    work.letGo ("prepared");
    REQUIRE (eventually ([&] { return work.isFinished(); }));

    const auto progress = work.queue->getProgress();
    REQUIRE (progress.halt == StemQueue::Halt::none);
    REQUIRE (progress.failed == 0);
    REQUIRE (progress.finished == 1);

    // It would be refused if it had been written down as failed.
    Told told;
    work.queue->requestNow (track ("prepared"), told.waiter());
    REQUIRE (eventually ([&] { return told.heard (StemOutcome::ready); }));
}

TEST_CASE ("a job that says it stopped when nobody asked has failed", "[stems][queue]")
{
    StandIn work;
    work.answer ("liar", StemOutcome::stopped);

    Told told;
    work.queue->requestNow (track ("liar"), told.waiter());

    // Taken at its word and requeued, this would run for ever.
    REQUIRE (eventually ([&] { return told.heard (StemOutcome::failed); }));
    REQUIRE (work.startsOf ("liar") == 1);
}

TEST_CASE ("a file that failed is not tried again this session", "[stems][queue]")
{
    StandIn work;
    work.answer ("broken", StemOutcome::failed);

    Told first;
    work.queue->requestNow (track ("broken"), first.waiter());
    REQUIRE (eventually ([&] { return first.heard (StemOutcome::failed); }));

    work.answer ("broken", StemOutcome::ready);

    Told second;
    work.queue->requestNow (track ("broken"), second.waiter());
    REQUIRE (second.heard (StemOutcome::failed));

    work.queue->requestInBackground ({ track ("broken") });
    giveItTheChanceToGoWrong();

    REQUIRE (work.startsOf ("broken") == 1);
}

TEST_CASE ("a press by hand tries a failed track again, and automix's request does not", "[stems][queue]")
{
    StandIn work;
    work.answer ("broken", StemOutcome::failed);

    Told first;
    work.queue->requestNow (track ("broken"), first.waiter());
    REQUIRE (eventually ([&] { return first.heard (StemOutcome::failed); }));

    // Automix asking again is turned away without the work being tried.
    Told automix;
    work.queue->requestNow (track ("broken"), automix.waiter());
    REQUIRE (automix.heard (StemOutcome::failed));
    REQUIRE (work.startsOf ("broken") == 1);

    // A person pressing the button is asking for another go, and gets one.
    work.answer ("broken", StemOutcome::ready);

    Told pressed;
    work.queue->requestNow (track ("broken"), pressed.waiter(), true);
    REQUIRE (eventually ([&] { return pressed.heard (StemOutcome::ready); }));
    REQUIRE (work.startsOf ("broken") == 2);
}

TEST_CASE ("the background lane halts when the disk is nearly full", "[stems][queue]")
{
    StandIn work;
    work.freeBytes.store (StemQueue::minimumFreeBytes - 1);

    work.queue->requestInBackground ({ track ("a"), track ("b") });

    REQUIRE (eventually ([&] { return work.queue->getProgress().halt == StemQueue::Halt::noSpace; }));
    REQUIRE (work.order().isEmpty());
    REQUIRE (work.queue->getProgress().describe().contains ("5 GB"));

    // A deck asked for this one in the middle of a set, and 300 MB will fit.
    Told told;
    work.queue->requestNow (track ("deck"), told.waiter());
    REQUIRE (eventually ([&] { return told.heard (StemOutcome::ready); }));

    work.freeBytes.store (StemQueue::minimumFreeBytes);
    work.queue->requestInBackground ({ track ("a") });
    REQUIRE (eventually ([&] { return work.startsOf ("a") == 1; }));
}

TEST_CASE ("cancelling forgets the queue and stops its job, and leaves a deck's alone", "[stems][queue]")
{
    StandIn work;
    work.hold ("prepared");
    work.hold ("deck");

    work.queue->requestInBackground ({ track ("prepared"), track ("later") });
    REQUIRE (eventually ([&] { return work.startsOf ("prepared") == 1; }));

    work.queue->cancelBackground();

    REQUIRE (eventually ([&] { return work.isFinished(); }));
    giveItTheChanceToGoWrong();

    REQUIRE (work.order() == "prepared");
    REQUIRE (work.queue->getProgress().describe().isEmpty());

    Told told;
    work.queue->requestNow (track ("deck"), told.waiter());
    REQUIRE (eventually ([&] { return work.startsOf ("deck") == 1; }));

    work.queue->cancelBackground();
    giveItTheChanceToGoWrong();

    REQUIRE (work.queue->getProgress().running);
    REQUIRE (told.times.load() == 0);

    work.letGo ("deck");
    REQUIRE (eventually ([&] { return told.heard (StemOutcome::ready); }));
}

TEST_CASE ("a deck asking for a job being cancelled gets it back", "[stems][queue]")
{
    // Holds on until the stop has been seen and the deck has asked.
    std::atomic<bool> stopSeen { false }, deckHasAsked { false }, firstRun { true };

    StemQueue::Config config;
    config.work = [&] (const juce::File&, const std::function<void (float)>&, const std::atomic<bool>& stop)
    {
        if (! firstRun.exchange (false))
            return StemOutcome::ready;

        while (! stop.load())
            juce::Thread::sleep (1);

        stopSeen.store (true);

        while (! deckHasAsked.load())
            juce::Thread::sleep (1);

        return StemOutcome::stopped;
    };

    StemQueue queue (std::move (config));
    queue.requestInBackground ({ track ("song") });
    REQUIRE (eventually ([&] { return queue.getProgress().running; }));

    queue.cancelBackground();
    REQUIRE (eventually ([&] { return stopSeen.load(); }));

    Told told;
    queue.requestNow (track ("song"), told.waiter());
    deckHasAsked.store (true);

    REQUIRE (eventually ([&] { return told.heard (StemOutcome::ready); }));
}

TEST_CASE ("a cancel holds even when the job was already on its way back to the queue", "[stems][queue]")
{
    // The deck starting has the job stopping, to go back in the lane, and it is
    // still winding down when the cancel comes.
    std::atomic<bool> stopSeen { false }, cancelled { false };
    std::atomic<int> starts { 0 };

    StemQueue::Config config;
    config.work = [&] (const juce::File&, const std::function<void (float)>&, const std::atomic<bool>& stop)
    {
        ++starts;

        while (! stop.load())
            juce::Thread::sleep (1);

        stopSeen.store (true);

        while (! cancelled.load())
            juce::Thread::sleep (1);

        return StemOutcome::stopped;
    };

    std::atomic<bool> playing { false };
    config.anyDeckPlaying = [&] { return playing.load(); };

    StemQueue queue (std::move (config));
    queue.requestInBackground ({ track ("song") });
    REQUIRE (eventually ([&] { return starts.load() == 1; }));

    playing.store (true);
    REQUIRE (eventually ([&] { return stopSeen.load(); }));

    queue.cancelBackground();
    cancelled.store (true);
    playing.store (false);

    REQUIRE (eventually ([&] { return ! queue.getProgress().running; }));
    giveItTheChanceToGoWrong();

    REQUIRE (starts.load() == 1);
    REQUIRE (! queue.isPreparing());
}

TEST_CASE ("asking again for a track being cancelled brings it back, and counts it", "[stems][queue]")
{
    std::atomic<bool> stopSeen { false }, askedAgain { false };
    std::map<juce::String, int> starts;
    std::mutex startsLock;

    StemQueue::Config config;
    config.work = [&] (const juce::File& file, const std::function<void (float)>&, const std::atomic<bool>& stop)
    {
        {
            const std::lock_guard<std::mutex> lock (startsLock);

            if (++starts[file.getFileNameWithoutExtension()] > 1 || file != track ("song"))
                return StemOutcome::ready;
        }

        while (! stop.load())
            juce::Thread::sleep (1);

        stopSeen.store (true);

        while (! askedAgain.load())
            juce::Thread::sleep (1);

        return StemOutcome::stopped;
    };

    StemQueue queue (std::move (config));
    queue.requestInBackground ({ track ("song") });
    REQUIRE (eventually ([&] { return queue.getProgress().running; }));

    queue.cancelBackground();
    REQUIRE (eventually ([&] { return stopSeen.load(); }));

    queue.requestInBackground ({ track ("song"), track ("other") });
    askedAgain.store (true);

    REQUIRE (eventually ([&] { return ! queue.isPreparing() && ! queue.getProgress().running; }));

    const std::lock_guard<std::mutex> lock (startsLock);
    REQUIRE (starts[juce::String ("song")] == 2);
    REQUIRE (queue.getProgress().total == 2);
    REQUIRE (queue.getProgress().finished == 2);
}

TEST_CASE ("a deck's request for a track no deck holds any more is dropped", "[stems][queue]")
{
    StandIn work (true);
    work.hold ("first");
    work.decksHold ({ "first", "gone", "kept" });

    Told first, gone, kept;
    work.queue->requestNow (track ("first"), first.waiter());
    REQUIRE (eventually ([&] { return work.startsOf ("first") == 1; }));

    work.queue->requestNow (track ("gone"), gone.waiter());
    work.queue->requestNow (track ("kept"), kept.waiter());

    // The deck that asked for this one has loaded something else.
    work.decksHold ({ "first", "kept" });
    work.letGo ("first");

    REQUIRE (eventually ([&] { return kept.heard (StemOutcome::ready); }));
    REQUIRE (gone.heard (StemOutcome::stopped));
    REQUIRE (work.order() == "first kept");
}

TEST_CASE ("a track being prepared that a deck holds carries on when the decks start", "[stems][queue]")
{
    StandIn work (true);
    work.hold ("loaded");
    work.decksHold ({ "loaded" });

    work.queue->requestInBackground ({ track ("loaded"), track ("later") });
    REQUIRE (eventually ([&] { return work.startsOf ("loaded") == 1; }));

    // Its deck's button has been showing this job as its own, and now plays.
    work.deckPlaying.store (true);
    giveItTheChanceToGoWrong();

    REQUIRE (work.queue->getProgress().running);
    REQUIRE (work.order() == "loaded");

    work.letGo ("loaded");
    REQUIRE (eventually ([&] { return ! work.queue->getProgress().running; }));
    giveItTheChanceToGoWrong();

    // Done once, and still counted with the list; the rest waits for the decks.
    REQUIRE (work.order() == "loaded");
    REQUIRE (work.queue->getProgress().finished == 1);

    work.deckPlaying.store (false);
    REQUIRE (eventually ([&] { return work.isFinished(); }));
    REQUIRE (work.order() == "loaded later");
}

TEST_CASE ("a file that failed is tried again once it has changed", "[stems][queue]")
{
    // The drive it was on was not there, or it was still being copied: either
    // way the file is not the one that failed any more.
    juce::TemporaryFile temporary (".flac");
    const auto file = temporary.getFile();
    REQUIRE (file.replaceWithText ("half of a file"));

    std::atomic<int> starts { 0 };
    std::atomic<bool> works { false };

    StemQueue::Config config;
    config.work = [&] (const juce::File&, const std::function<void (float)>&, const std::atomic<bool>&)
    {
        ++starts;
        return works.load() ? StemOutcome::ready : StemOutcome::failed;
    };

    StemQueue queue (std::move (config));

    Told first, unchanged, changed;
    queue.requestNow (file, first.waiter());
    REQUIRE (eventually ([&] { return first.heard (StemOutcome::failed); }));

    works.store (true);
    queue.requestNow (file, unchanged.waiter());
    REQUIRE (unchanged.heard (StemOutcome::failed));
    REQUIRE (starts.load() == 1);

    REQUIRE (file.appendText (" and the rest of it"));
    queue.requestNow (file, changed.waiter());
    REQUIRE (eventually ([&] { return changed.heard (StemOutcome::ready); }));
    REQUIRE (starts.load() == 2);
}

TEST_CASE ("a halt for failures does not hold them against a deck's request", "[stems][queue]")
{
    StandIn work;

    for (const auto* name : { "a", "b", "c" })
        work.answer (name, StemOutcome::failed);

    work.queue->requestInBackground ({ track ("a"), track ("b"), track ("c"), track ("d") });
    REQUIRE (eventually ([&] { return work.queue->getProgress().halt == StemQueue::Halt::failures; }));
    REQUIRE (eventually ([&] { return work.isFinished(); }));

    // The disk had filled, say, and has been cleared.
    work.answer ("b", StemOutcome::ready);

    Told told;
    work.queue->requestNow (track ("b"), told.waiter());
    REQUIRE (eventually ([&] { return told.heard (StemOutcome::ready); }));
    REQUIRE (work.startsOf ("b") == 2);
}

TEST_CASE ("a halt still says how far the list had got", "[stems][queue]")
{
    StemQueue::Progress progress;
    progress.total = 40;
    progress.finished = 12;
    progress.failed = 3;

    progress.halt = StemQueue::Halt::failures;
    REQUIRE (progress.describe().contains ("12 of 40 ready"));
    REQUIRE (progress.describe().contains ("3 failed"));

    progress.halt = StemQueue::Halt::noSpace;
    REQUIRE (progress.describe().contains ("12 of 40 ready"));
}

TEST_CASE ("the queue says where it has got to", "[stems][queue]")
{
    StandIn work;
    work.hold ("c");
    work.answer ("b", StemOutcome::failed);

    REQUIRE (work.queue->getProgress().describe().isEmpty());

    work.queue->requestInBackground ({ track ("a"), track ("b"), track ("c"), track ("d") });
    REQUIRE (eventually ([&] { return work.startsOf ("c") == 1; }));
    REQUIRE (eventually ([&] { return work.queue->getProgress().fraction > 0.0f; }));

    REQUIRE (work.queue->isRequested (track ("c")));
    REQUIRE (! work.queue->isRequested (track ("d")));
    REQUIRE (work.queue->getFractionFor (track ("c")) > 0.2f);
    REQUIRE (work.queue->getFractionFor (track ("d")) < 0.01f);

    REQUIRE (work.queue->getProgress().describe() == "Stems 3 of 4, 1 failed: c 25%");

    work.deckPlaying.store (true);
    REQUIRE (eventually ([&] { return work.queue->getProgress().waitingForDecks; }));
    REQUIRE (work.queue->getProgress().describe() == "Stems 3 of 4, 1 failed, waiting for the decks to stop");

    work.letGo ("c");
    work.deckPlaying.store (false);
    REQUIRE (eventually ([&] { return work.isFinished(); }));
    REQUIRE (work.queue->getProgress().describe() == "Stems ready for 3 of 4, 1 failed");

    // A new list is a new count.
    work.queue->requestInBackground ({ track ("e") });
    REQUIRE (eventually ([&] { return work.startsOf ("e") == 1; }));
    REQUIRE (eventually ([&] { return work.isFinished(); }));
    REQUIRE (work.queue->getProgress().total == 1);
}

TEST_CASE ("shutting down stops the job, clears the lanes and tells everyone", "[stems][queue]")
{
    StandIn work;
    work.hold ("running");

    Told running, waiting;
    work.queue->requestNow (track ("running"), running.waiter());
    REQUIRE (eventually ([&] { return work.startsOf ("running") == 1; }));

    work.queue->requestNow (track ("waiting"), waiting.waiter());
    work.queue->requestInBackground ({ track ("prepared") });

    // Not eventually: when this returns, the worker is gone. A stand-in that
    // outlived its queue is what would crash if it were not.
    work.queue->shutdown();

    REQUIRE (running.heard (StemOutcome::stopped));
    REQUIRE (waiting.heard (StemOutcome::stopped));
    REQUIRE (work.order() == "running");

    Told afterwards;
    work.queue->requestNow (track ("late"), afterwards.waiter());
    REQUIRE (afterwards.heard (StemOutcome::stopped));

    work.queue->requestInBackground ({ track ("late") });
    giveItTheChanceToGoWrong();
    REQUIRE (work.order() == "running");
}

TEST_CASE ("onReady has run by the time a waiter is told, and only for what is ready", "[stems][queue]")
{
    StandIn work;
    work.answer ("broken", StemOutcome::failed);

    std::vector<juce::String> readyWhenTold;
    std::atomic<bool> told { false };

    work.queue->requestNow (track ("broken"));
    work.queue->requestNow (track ("song"), [&] (StemOutcome)
    {
        const std::lock_guard<std::mutex> lock (work.mutex);
        readyWhenTold = work.ready;
        told.store (true);
    });

    REQUIRE (eventually ([&] { return told.load(); }));
    REQUIRE (readyWhenTold == std::vector<juce::String> { "song" });
}

//==============================================================================
// The actions.

TEST_CASE ("the stems menu says when it cannot, and offers a way out while busy or halted", "[stems][menu]")
{
    using opendj::StemPrepareMenu;

    auto available = true, busy = false, status = false;

    StemPrepareMenu items;
    items.isAvailable = [&] { return available; };
    items.isBusy = [&] { return busy; };
    items.hasStatus = [&] { return status; };
    items.prepare = [] {};
    items.cancel = [] {};

    // Every item's text, with a "-" in front of the ones that cannot be chosen.
    const auto itemsOf = [&] (bool anythingToPrepare)
    {
        juce::PopupMenu menu;
        items.addTo (menu, "Prepare stems", anythingToPrepare);

        juce::StringArray texts;

        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            if (! it.getItem().isSeparator)
                texts.add ((it.getItem().isEnabled ? "" : "-") + it.getItem().text);

        return texts.joinIntoString (", ");
    };

    REQUIRE (itemsOf (true) == "Prepare stems");
    REQUIRE (itemsOf (false) == "-Prepare stems");

    available = false;
    REQUIRE (itemsOf (true) == "-Prepare stems (no separator found)");
    available = true;

    busy = true;
    REQUIRE (itemsOf (true) == juce::String ("Prepare stems, ") + StemPrepareMenu::cancelText);

    // Halted: nothing is running, and the message is still up.
    busy = false;
    status = true;
    REQUIRE (itemsOf (true) == juce::String ("Prepare stems, ") + StemPrepareMenu::clearText);
}

TEST_CASE ("a press of prepare means the list on show", "[stems][menu]")
{
    const std::vector<juce::File> playlist { track ("one"), track ("two"), track ("three") };
    const std::vector<juce::File> selection { track ("two") };

    REQUIRE (&opendj::stemListOnShow (true, playlist, selection) == &playlist);
    REQUIRE (&opendj::stemListOnShow (false, playlist, selection) == &selection);
}

TEST_CASE ("the prepare actions have names and name no deck", "[stems][actions]")
{
    using namespace opendj;

    REQUIRE (actionFromString ("stems.prepare_selected") == Action::stemsPrepareSelected);
    REQUIRE (actionFromString ("stems.prepare_cancel") == Action::stemsPrepareCancel);
    REQUIRE (toString (Action::stemsPrepareSelected) == "stems.prepare_selected");
    REQUIRE (toString (Action::stemsPrepareCancel) == "stems.prepare_cancel");

    REQUIRE (! actionTakesDeck (Action::stemsPrepareSelected));
    REQUIRE (! actionTakesDeck (Action::stemsPrepareCancel));
    REQUIRE (! actionTakesSlot (Action::stemsPrepareSelected));
    REQUIRE (! isContinuous (Action::stemsPrepareSelected));
}

TEST_CASE ("a press of prepare reaches the handler, and a release does not", "[stems][actions]")
{
    using namespace opendj;

    AudioEngine engine (StemSeparator::Models::ignoreInstalled);
    ActionDispatcher dispatcher (engine);

    // Nothing is ready to answer yet, and nothing should mind.
    dispatcher.dispatch ({ Action::stemsPrepareSelected, 0, 0, 1.0f });

    auto calls = 0;
    dispatcher.prepareStemsHandler = [&calls] { ++calls; };

    dispatcher.dispatch ({ Action::stemsPrepareSelected, 0, 0, 1.0f });
    REQUIRE (calls == 1);

    dispatcher.dispatch ({ Action::stemsPrepareSelected, 0, 0, 0.0f });
    REQUIRE (calls == 1);
}

//==============================================================================
// The real separator and the real cache, with a shell script for a model.

#if ! JUCE_WINDOWS

namespace
{
    constexpr double fileRate = 44100.0;
    constexpr double trackSeconds = 6.0;

    constexpr double stemTones[opendj::numStems] { 110.0, 220.0, 440.0, 880.0 };
    constexpr float stemLevel = 0.2f;

    const char* const stemNames[opendj::numStems] { "drums", "bass", "other", "vocals" };

    void writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio)
    {
        file.deleteFile();

        juce::WavAudioFormat wav;
        auto stream = std::make_unique<juce::FileOutputStream> (file);
        REQUIRE (stream->openedOk());

        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (stream.get(), fileRate, 2, 32, {}, 0));
        REQUIRE (writer != nullptr);

        stream.release();
        REQUIRE (writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()));
    }

    juce::AudioBuffer<float> toneOf (double frequency, double seconds)
    {
        const auto numSamples = (int) std::lround (seconds * fileRate);
        const auto delta = juce::MathConstants<double>::twoPi * frequency / fileRate;

        juce::AudioBuffer<float> audio (2, numSamples);

        for (int i = 0; i < numSamples; ++i)
            for (int ch = 0; ch < 2; ++ch)
                audio.setSample (ch, i, stemLevel * (float) std::sin (delta * i));

        return audio;
    }

    /** How much of one tone is in a signal, as its amplitude. */
    double toneLevel (const std::vector<float>& samples, double frequency, double rate)
    {
        const auto delta = juce::MathConstants<double>::twoPi * frequency / rate;
        const auto windowStep = juce::MathConstants<double>::twoPi / (double) samples.size();
        double real = 0.0, imaginary = 0.0, windowSum = 0.0;

        for (size_t i = 0; i < samples.size(); ++i)
        {
            const auto window = 0.5 - 0.5 * std::cos (windowStep * (double) i);
            real += window * samples[i] * std::cos (delta * (double) i);
            imaginary += window * samples[i] * std::sin (delta * (double) i);
            windowSum += window;
        }

        return 2.0 * std::sqrt (real * real + imaginary * imaginary) / windowSum;
    }

    /** A folder of its own, the cache pointed into it, and a track there whose
        mix is exactly the sum of four tones. The script copies those four tones
        out as its stems, so what comes back from the cache can be measured.

        The environment is put back afterwards, since the whole binary can also
        be run as one process. */
    struct Sandbox
    {
        Sandbox()
            : root (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("opendj-stems-test-" + juce::String ((int) ::getpid()) + "-"
                                       + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64())))
        {
            REQUIRE (root.createDirectory());
            REQUIRE (templates.createDirectory());

            for (const auto* name : { "XDG_CACHE_HOME", "OPENDJ_STEM_SEPARATOR", "OPENDJ_STEM_MODEL" })
                saved.emplace_back (name, juce::SystemStats::getEnvironmentVariable (name, {}));

            ::setenv ("XDG_CACHE_HOME", root.getChildFile ("cache").getFullPathName().toRawUTF8(), 1);

            juce::AudioBuffer<float> mix (2, (int) std::lround (trackSeconds * fileRate));
            mix.clear();

            for (int stem = 0; stem < opendj::numStems; ++stem)
            {
                const auto tone = toneOf (stemTones[stem], trackSeconds);
                writeWav (templates.getChildFile (juce::String (stemNames[stem]) + ".wav"), tone);

                for (int ch = 0; ch < 2; ++ch)
                    mix.addFrom (ch, 0, tone, ch, 0, mix.getNumSamples());
            }

            writeWav (song, mix);

            formats.registerBasicFormats();
        }

        ~Sandbox()
        {
            for (const auto& [name, value] : saved)
            {
                if (value.isNotEmpty())
                    ::setenv (name.toRawUTF8(), value.toRawUTF8(), 1);
                else
                    ::unsetenv (name.toRawUTF8());
            }

            root.deleteRecursively();
        }

        /** The stand-in: called as `run <track> -o <folder> --format wav --device
            auto`, it does `before`, writes the four stems where the real one
            would, then does `after`. Every run is counted in a file. */
        void writeSeparator (const juce::String& before = {}, const juce::String& after = {})
        {
            const auto script = root.getChildFile ("stand-in-separator");

            // Unix line endings. JUCE writes "\r\n" by default, and "#!/bin/sh\r"
            // is not an interpreter the kernel can find.
            REQUIRE (script.replaceWithText (
                "#!/bin/sh\n"
                "echo run >> '" + runs.getFullPathName() + "'\n"
                "echo \"$4\" > '" + outputWasGiven.getFullPathName() + "'\n"
                "name=$(basename \"$2\")\n"
                "name=\"${name%.*}\"\n"
                + before + "\n"
                "mkdir -p \"$4/$name\"\n"
                "cp '" + templates.getFullPathName() + "'/*.wav \"$4/$name/\"\n"
                + after + "\n", false, false, "\n"));

            REQUIRE (script.setExecutePermission (true));
            ::setenv ("OPENDJ_STEM_SEPARATOR", script.getFullPathName().toRawUTF8(), 1);
        }

        int numRuns() const
        {
            juce::StringArray lines;
            runs.readLines (lines);
            lines.removeEmptyStrings();
            return lines.size();
        }

        StemOutcome prepare (StemSeparator& separator, const std::atomic<bool>& stop)
        {
            return separator.prepare (formats, song, {}, stop);
        }

        /** Nothing about this track left anywhere in the cache. */
        void requireNoEntry (StemSeparator& separator)
        {
            REQUIRE (! separator.isCached (song));
            REQUIRE (separator.loadFromCache (formats, song) == nullptr);
            REQUIRE (! separator.getCacheFolder (song).exists());
            REQUIRE (! separator.getPartialFolder (song).exists());
        }

        juce::File root;
        juce::File templates { root.getChildFile ("templates") };
        juce::File song { root.getChildFile ("song.wav") };
        juce::File runs { root.getChildFile ("runs.txt") };
        juce::File outputWasGiven { root.getChildFile ("output.txt") };
        juce::File marker { root.getChildFile ("marker.txt") };
        std::vector<std::pair<juce::String, juce::String>> saved;
        juce::AudioFormatManager formats;
    };

    /** Whether a process is still doing anything. A dead one nobody has reaped
        still answers a signal, so it is asked about too. */
    bool isAlive (int pid)
    {
        if (::kill (pid, 0) != 0)
            return false;

        const auto stat = juce::File ("/proc/" + juce::String (pid) + "/stat");
        return ! stat.existsAsFile() || ! stat.loadFileAsString().contains (") Z ");
    }

    /** prepare on a thread of its own, for a job that has to be stopped. */
    struct RunningPrepare
    {
        RunningPrepare (Sandbox& sandboxToUse, StemSeparator& separator)
            : thread ([this, &sandboxToUse, &separator]
              {
                  result.store ((int) sandboxToUse.prepare (separator, stop));
                  finished.store (true);
              })
        {
        }

        ~RunningPrepare()
        {
            stop.store (true);

            if (thread.joinable())
                thread.join();
        }

        StemOutcome stopAndWait()
        {
            stop.store (true);
            thread.join();
            return (StemOutcome) result.load();
        }

        std::atomic<bool> stop { false }, finished { false };
        std::atomic<int> result { -1 };
        std::thread thread;
    };

    const std::atomic<bool> neverStop { false };
}

TEST_CASE ("a track with a very long name can still be prepared", "[stems][separator]")
{
    // The folder a job writes into is named after the track, with the hash and
    // ".partial" after it, and no name in a folder can be over 255 bytes.
    Sandbox sandbox;
    sandbox.writeSeparator();

    const auto longName = sandbox.root.getChildFile (juce::String::repeatedString ("long name ", 23) + "ab.wav");
    REQUIRE (longName.getFileNameWithoutExtension().length() == 232);
    REQUIRE (sandbox.song.copyFileTo (longName));

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);
    REQUIRE (separator.prepare (sandbox.formats, longName, {}, neverStop) == StemOutcome::ready);
    REQUIRE (separator.isCached (longName));
    REQUIRE (separator.loadFromCache (sandbox.formats, longName) != nullptr);

    // A name that always fitted keeps the key it had, so its entry is still found.
    REQUIRE (separator.getCacheFolder (sandbox.song).getFileName().startsWith ("song ["));
}

#if JUCE_LINUX
TEST_CASE ("separator helper: prepares until killed", "[.][separator-helper]")
{
    // Not a test by itself. The one below runs it as a process of its own, so
    // as to kill that process and see what happens to the separator it started.
    const juce::File song (juce::SystemStats::getEnvironmentVariable ("OPENDJ_TEST_SONG", {}));
    REQUIRE (song.existsAsFile());

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);
    separator.prepare (formats, song, {}, neverStop);
}

TEST_CASE ("a separator does not outlive the application that started it", "[stems][separator]")
{
    // A crash, a kill or a Ctrl-C ends the application without the queue being
    // told. A separator left running would have every core for minutes.
    Sandbox sandbox;
    sandbox.writeSeparator ("echo $$ > '" + sandbox.marker.getFullPathName() + "'\nexec sleep 60");
    ::setenv ("OPENDJ_TEST_SONG", sandbox.song.getFullPathName().toRawUTF8(), 1);

    juce::ChildProcess helper;
    REQUIRE (helper.start (juce::StringArray { juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName(),
                                               "[separator-helper]" }, 0));

    REQUIRE (eventually ([&] { return sandbox.marker.loadFileAsString().trim().getIntValue() > 0; }));
    const auto separatorPid = sandbox.marker.loadFileAsString().trim().getIntValue();
    REQUIRE (isAlive (separatorPid));

    REQUIRE (helper.kill());
    const auto gone = eventually ([&] { return ! isAlive (separatorPid); }, 5000);

    if (! gone)
        ::kill (separatorPid, SIGKILL);

    ::unsetenv ("OPENDJ_TEST_SONG");
    REQUIRE (gone);
}
#endif

TEST_CASE ("a script stands in for the separator wherever models are installed", "[stems][separator]")
{
    Sandbox sandbox;
    sandbox.writeSeparator();

    // A file is all it takes for the search to find a model, and a machine with
    // real ones would otherwise run them instead of any of what follows.
    const auto pretendModel = sandbox.root.getChildFile ("htdemucs_v4.xml");
    REQUIRE (pretendModel.replaceWithText ("not a model"));
    ::setenv ("OPENDJ_STEM_MODEL", pretendModel.getFullPathName().toRawUTF8(), 1);

    StemSeparator withModels;
    REQUIRE (withModels.hasNativeModel());

    StemSeparator withoutModels (StemSeparator::Models::ignoreInstalled);
    REQUIRE (! withoutModels.hasNativeModel());
    REQUIRE (withoutModels.isAvailable());
    REQUIRE (withoutModels.getStatusDescription().contains ("stand-in-separator"));
}

TEST_CASE ("a separated track lands whole in the cache and is not separated twice", "[stems][separator]")
{
    Sandbox sandbox;
    sandbox.writeSeparator();

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);

    REQUIRE (! separator.isCached (sandbox.song));
    REQUIRE (sandbox.prepare (separator, neverStop) == StemOutcome::ready);

    REQUIRE (separator.isCached (sandbox.song));
    REQUIRE (sandbox.numRuns() == 1);

    // Repacked, and the separator's own float WAVs and folders gone with the
    // rest of the partial one.
    const auto folder = separator.getCacheFolder (sandbox.song);
    REQUIRE (! separator.getPartialFolder (sandbox.song).exists());
    REQUIRE (folder.getChildFile ("vocals.flac").existsAsFile());
    REQUIRE (! folder.getChildFile ("vocals.wav").exists());
    REQUIRE (! folder.getChildFile ("work").exists());
    REQUIRE (folder.getChildFile ("opendj-stems-source.txt").existsAsFile());
    REQUIRE (folder.getParentDirectory().findChildFiles (juce::File::findDirectories, false).size() == 1);

    // The output folder it was handed was in the partial one, where a kill
    // would leave nothing the sweep cannot find.
    REQUIRE (juce::File (sandbox.outputWasGiven.loadFileAsString().trim())
                 .isAChildOf (separator.getPartialFolder (sandbox.song)));

    const auto stems = separator.loadFromCache (sandbox.formats, sandbox.song);
    REQUIRE (stems != nullptr);
    REQUIRE (stems->isWellFormed());
    REQUIRE (stems->getNumSamples() == (int) std::lround (trackSeconds * fileRate));

    for (int stem = 0; stem < opendj::numStems; ++stem)
    {
        std::vector<float> samples (stems->stems[(size_t) stem].getReadPointer (0),
                                    stems->stems[(size_t) stem].getReadPointer (0) + stems->getNumSamples());

        REQUIRE_THAT (toneLevel (samples, stemTones[stem], fileRate), WithinAbs ((double) stemLevel, 0.001));
    }

    // Asked again, it costs a look at the file sizes.
    REQUIRE (sandbox.prepare (separator, neverStop) == StemOutcome::ready);
    REQUIRE (sandbox.numRuns() == 1);
}

TEST_CASE ("a file nothing here can read costs the separator no time", "[stems][separator]")
{
    Sandbox sandbox;
    sandbox.writeSeparator();

    const auto notAudio = sandbox.root.getChildFile ("notes.wav");
    REQUIRE (notAudio.replaceWithText ("not audio at all"));

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);

    REQUIRE (separator.prepare (sandbox.formats, notAudio, {}, neverStop) == StemOutcome::failed);
    REQUIRE (sandbox.numRuns() == 0);
    REQUIRE (! separator.getPartialFolder (notAudio).exists());
}

TEST_CASE ("a separator that kills itself after writing its stems leaves no cache entry", "[stems][separator]")
{
    Sandbox sandbox;

    // Everything looks finished and every stem decodes: the only sign is how the
    // process ended, which JUCE reports as exit code 0.
    sandbox.writeSeparator ({}, "kill -9 $$");

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);

    REQUIRE (sandbox.prepare (separator, neverStop) == StemOutcome::failed);
    REQUIRE (sandbox.numRuns() == 1);
    sandbox.requireNoEntry (separator);
}

TEST_CASE ("a separator that exits with an error leaves no cache entry", "[stems][separator]")
{
    Sandbox sandbox;
    sandbox.writeSeparator ({}, "exit 3");

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);

    REQUIRE (sandbox.prepare (separator, neverStop) == StemOutcome::failed);
    sandbox.requireNoEntry (separator);
}

TEST_CASE ("stems that do not add up to the track never become a cache entry", "[stems][separator]")
{
    Sandbox sandbox;

    SECTION ("one missing")
    {
        sandbox.writeSeparator ({}, "rm \"$4/$name/vocals.wav\"");
    }

    SECTION ("one much shorter than the others")
    {
        sandbox.writeSeparator();
        writeWav (sandbox.templates.getChildFile ("bass.wav"), toneOf (220.0, trackSeconds / 2.0));
    }

    SECTION ("one short by enough to hear, though still near the track")
    {
        sandbox.writeSeparator();
        writeWav (sandbox.templates.getChildFile ("bass.wav"), toneOf (220.0, trackSeconds - 0.5));
    }

    SECTION ("all the same length, and it is not the track's")
    {
        sandbox.writeSeparator();

        for (int stem = 0; stem < opendj::numStems; ++stem)
            writeWav (sandbox.templates.getChildFile (juce::String (stemNames[stem]) + ".wav"),
                      toneOf (stemTones[stem], trackSeconds / 2.0));
    }

    SECTION ("nothing written at all")
    {
        sandbox.writeSeparator ("exit 0");
    }

    // Only now: it looks for its command when it is made.
    StemSeparator separator (StemSeparator::Models::ignoreInstalled);

    REQUIRE (sandbox.prepare (separator, neverStop) == StemOutcome::failed);
    REQUIRE (sandbox.numRuns() == 1);
    sandbox.requireNoEntry (separator);
}

TEST_CASE ("a file that changes during the job is not stamped as the one separated", "[stems][separator]")
{
    Sandbox sandbox;

    // Tags being written, say, by something else, in the minutes a job takes.
    sandbox.writeSeparator ("echo edited >> \"$2\"");

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);

    REQUIRE (sandbox.prepare (separator, neverStop) == StemOutcome::ready);
    REQUIRE (separator.getCacheFolder (sandbox.song).isDirectory());
    REQUIRE (! separator.isCached (sandbox.song));
}

TEST_CASE ("stems a sample or two apart are trimmed to one length", "[stems][separator]")
{
    Sandbox sandbox;
    sandbox.writeSeparator();

    auto drums = toneOf (stemTones[0], trackSeconds);
    drums.setSize (2, drums.getNumSamples() - 2, true);
    writeWav (sandbox.templates.getChildFile ("drums.wav"), drums);

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);
    REQUIRE (sandbox.prepare (separator, neverStop) == StemOutcome::ready);

    const auto stems = separator.loadFromCache (sandbox.formats, sandbox.song);
    REQUIRE (stems != nullptr);
    REQUIRE (stems->isWellFormed());
    REQUIRE (stems->getNumSamples() == drums.getNumSamples());
}

TEST_CASE ("a separator that never shuts up cannot wedge the job", "[stems][separator]")
{
    Sandbox sandbox;

    // Far more than any pipe holds, on both streams, before it gets to the stems.
    sandbox.writeSeparator ("head -c 4000000 /dev/zero\nhead -c 4000000 /dev/zero >&2");

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);
    RunningPrepare job (sandbox, separator);

    const auto finished = eventually ([&] { return job.finished.load(); });
    REQUIRE (finished);
    REQUIRE (job.stopAndWait() == StemOutcome::ready);
}

TEST_CASE ("stopping a separator kills all of it and says stopped", "[stems][separator]")
{
    Sandbox sandbox;

    // A worker of its own, which is what a real one has, and which is what a kill
    // of the one process it started would leave running on every core.
    const auto workerPid = sandbox.root.getChildFile ("worker.txt");
    sandbox.writeSeparator ({}, "sleep 300 &\necho $! > '" + workerPid.getFullPathName() + "'\nwait");

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);
    RunningPrepare job (sandbox, separator);

    REQUIRE (eventually ([&] { return workerPid.loadFileAsString().trim().isNotEmpty(); }));

    const auto pid = workerPid.loadFileAsString().trim().getIntValue();
    REQUIRE (isAlive (pid));

    REQUIRE (job.stopAndWait() == StemOutcome::stopped);
    REQUIRE (eventually ([&] { return ! isAlive (pid); }));
    sandbox.requireNoEntry (separator);
}

TEST_CASE ("a separator still running has nothing a load could find", "[stems][separator]")
{
    Sandbox sandbox;

    // All four stems written, every byte, and the job not over.
    sandbox.writeSeparator ({}, "echo written > '" + sandbox.marker.getFullPathName() + "'\nexec sleep 300");

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);
    RunningPrepare job (sandbox, separator);

    REQUIRE (eventually ([&] { return sandbox.marker.existsAsFile(); }));

    REQUIRE (separator.getPartialFolder (sandbox.song).isDirectory());
    REQUIRE (! separator.isCached (sandbox.song));
    REQUIRE (separator.loadFromCache (sandbox.formats, sandbox.song) == nullptr);

    // Not left lying around the cache either, where the sweep would not know it.
    const auto inCache = separator.getCacheFolder (sandbox.song).getParentDirectory()
                             .findChildFiles (juce::File::findDirectories, false);
    REQUIRE (inCache.size() == 1);
    REQUIRE (inCache.getFirst() == separator.getPartialFolder (sandbox.song));

    REQUIRE (job.stopAndWait() == StemOutcome::stopped);
    sandbox.requireNoEntry (separator);
}

TEST_CASE ("a complete looking partial folder never loads, and is swept at startup", "[stems][separator]")
{
    Sandbox sandbox;
    sandbox.writeSeparator();

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);
    REQUIRE (sandbox.prepare (separator, neverStop) == StemOutcome::ready);

    // The very entry that was just good, under the name a killed job leaves.
    const auto folder = separator.getCacheFolder (sandbox.song);
    const auto partial = separator.getPartialFolder (sandbox.song);
    REQUIRE (folder.moveFileTo (partial));

    REQUIRE (! separator.isCached (sandbox.song));
    REQUIRE (separator.loadFromCache (sandbox.formats, sandbox.song) == nullptr);

    // Someone else's entry, which the sweep is not to touch.
    const auto other = sandbox.root.getChildFile ("other.wav");
    REQUIRE (sandbox.song.copyFileTo (other));
    REQUIRE (separator.prepare (sandbox.formats, other, {}, neverStop) == StemOutcome::ready);

    separator.sweepLeftovers();

    REQUIRE (! partial.exists());
    REQUIRE (separator.isCached (other));
}

TEST_CASE ("a changed file no longer counts as cached", "[stems][separator]")
{
    Sandbox sandbox;
    sandbox.writeSeparator();

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);
    REQUIRE (sandbox.prepare (separator, neverStop) == StemOutcome::ready);
    REQUIRE (separator.isCached (sandbox.song));

    SECTION ("a new date")
    {
        REQUIRE (sandbox.song.setLastModificationTime (sandbox.song.getLastModificationTime()
                                                       + juce::RelativeTime::minutes (5)));
    }

    SECTION ("a new size, on the same date")
    {
        const auto date = sandbox.song.getLastModificationTime();
        REQUIRE (sandbox.song.appendData ("x", 1));
        REQUIRE (sandbox.song.setLastModificationTime (date));
    }

    REQUIRE (! separator.isCached (sandbox.song));
    REQUIRE (separator.loadFromCache (sandbox.formats, sandbox.song) == nullptr);

    // And asking again does the work again, replacing what was there.
    REQUIRE (sandbox.prepare (separator, neverStop) == StemOutcome::ready);
    REQUIRE (sandbox.numRuns() == 2);
    REQUIRE (separator.isCached (sandbox.song));
    REQUIRE (separator.loadFromCache (sandbox.formats, sandbox.song) != nullptr);
}

TEST_CASE ("an entry from before there were stamps is judged by its date", "[stems][separator]")
{
    Sandbox sandbox;
    sandbox.writeSeparator();

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);
    REQUIRE (sandbox.prepare (separator, neverStop) == StemOutcome::ready);

    const auto folder = separator.getCacheFolder (sandbox.song);
    REQUIRE (folder.getChildFile ("opendj-stems-source.txt").deleteFile());

    const auto now = juce::Time::getCurrentTime();

    REQUIRE (sandbox.song.setLastModificationTime (now - juce::RelativeTime::days (2)));
    REQUIRE (folder.setLastModificationTime (now - juce::RelativeTime::days (1)));
    REQUIRE (separator.isCached (sandbox.song));
    REQUIRE (separator.loadFromCache (sandbox.formats, sandbox.song) != nullptr);

    REQUIRE (sandbox.song.setLastModificationTime (now));
    REQUIRE (! separator.isCached (sandbox.song));
    REQUIRE (separator.loadFromCache (sandbox.formats, sandbox.song) == nullptr);
}

TEST_CASE ("an entry missing a stem does not count as cached", "[stems][separator]")
{
    Sandbox sandbox;
    sandbox.writeSeparator();

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);
    REQUIRE (sandbox.prepare (separator, neverStop) == StemOutcome::ready);

    REQUIRE (separator.getCacheFolder (sandbox.song).getChildFile ("bass.flac").deleteFile());
    REQUIRE (! separator.isCached (sandbox.song));
}

TEST_CASE ("stopping the in-process model does not fall through to the command", "[stems][separator]")
{
    Sandbox sandbox;
    sandbox.writeSeparator();

    std::atomic<bool> stop { false };

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);
    separator.useStandInModel ([&stop] (const juce::AudioBuffer<float>&, const std::function<bool (float)>& onProgress)
                               -> std::shared_ptr<const opendj::SeparatedTrack>
    {
        // Told to stop part way, it gives up, as the real model does.
        stop.store (true);
        return onProgress (0.5f) ? std::make_shared<opendj::SeparatedTrack>() : nullptr;
    });

    REQUIRE (sandbox.prepare (separator, stop) == StemOutcome::stopped);
    REQUIRE (sandbox.numRuns() == 0);
    sandbox.requireNoEntry (separator);
}

TEST_CASE ("a stop landing after the model finished still counts as stopped", "[stems][separator]")
{
    Sandbox sandbox;
    sandbox.writeSeparator();

    std::atomic<bool> stop { false };

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);
    separator.useStandInModel ([&stop] (const juce::AudioBuffer<float>& mix, const std::function<bool (float)>&)
                               -> std::shared_ptr<const opendj::SeparatedTrack>
    {
        auto stems = std::make_shared<opendj::SeparatedTrack>();

        for (auto& stem : stems->stems)
        {
            stem.setSize (2, mix.getNumSamples());
            stem.clear();
        }

        // After this comes the check of what was written, which takes a while
        // and gives up on a stop. Read as a failure, this would be written off
        // for the session, for being in the way.
        stop.store (true);
        return stems;
    });

    REQUIRE (sandbox.prepare (separator, stop) == StemOutcome::stopped);
    sandbox.requireNoEntry (separator);
}

TEST_CASE ("a model that fails falls through to the command", "[stems][separator]")
{
    Sandbox sandbox;
    sandbox.writeSeparator();

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);
    separator.useStandInModel ([] (const juce::AudioBuffer<float>&, const std::function<bool (float)>&)
                               -> std::shared_ptr<const opendj::SeparatedTrack> { return nullptr; });

    REQUIRE (sandbox.prepare (separator, neverStop) == StemOutcome::ready);
    REQUIRE (sandbox.numRuns() == 1);
    REQUIRE (separator.isCached (sandbox.song));
}

TEST_CASE ("the model's stems go through the same door and keep their level", "[stems][separator]")
{
    Sandbox sandbox;
    sandbox.writeSeparator();

    auto answer = std::make_shared<opendj::SeparatedTrack>();
    auto stemsToWrite = 0;

    const auto model = [&] (const juce::AudioBuffer<float>& mix, const std::function<bool (float)>&)
                       -> std::shared_ptr<const opendj::SeparatedTrack>
    {
        for (auto& stem : answer->stems)
        {
            stem.setSize (2, stemsToWrite > 0 ? stemsToWrite : mix.getNumSamples());
            stem.clear();
        }

        // Well over unity, as a part of a mix can be, which FLAC cannot hold.
        for (int i = 0; i < answer->getNumSamples(); ++i)
            answer->stems[0].setSample (0, i, 1.5f * (i % 2 == 0 ? 1.0f : -1.0f));

        return answer;
    };

    StemSeparator separator (StemSeparator::Models::ignoreInstalled);
    separator.useStandInModel (model);

    SECTION ("whole, and read back at the level they went in")
    {
        REQUIRE (sandbox.prepare (separator, neverStop) == StemOutcome::ready);
        REQUIRE (sandbox.numRuns() == 0);

        const auto stems = separator.loadFromCache (sandbox.formats, sandbox.song);
        REQUIRE (stems != nullptr);
        REQUIRE_THAT (stems->stems[0].getMagnitude (0, stems->getNumSamples()), WithinAbs (1.5f, 0.001f));
        REQUIRE (separator.getCacheFolder (sandbox.song).getChildFile ("opendj-stems-source.txt").existsAsFile());
    }

    SECTION ("the wrong length is turned away like anyone else's")
    {
        stemsToWrite = (int) (fileRate * trackSeconds / 2.0);

        REQUIRE (sandbox.prepare (separator, neverStop) == StemOutcome::failed);
        sandbox.requireNoEntry (separator);
    }
}

//==============================================================================
// All of it joined up, in the engine. The background lane wants 5 GB free, and
// these tests write a few kilobytes, so they say there is room rather than ask
// the disk. Asking would make them pass on one machine and skip on another.

namespace
{
    void giveRoom (StemSeparator& separator)
    {
        separator.setFreeSpaceProbe ([] { return StemQueue::minimumFreeBytes * 4; });
    }

    /** A second track, with no entry and no connection to the first. */
    juce::File otherTrack (Sandbox& sandbox)
    {
        const auto file = sandbox.root.getChildFile ("other.wav");
        REQUIRE (sandbox.song.copyFileTo (file));
        return file;
    }

    bool isQueueIdle (opendj::AudioEngine& engine)
    {
        return ! engine.isPreparingStems() && ! engine.getStemQueueProgress().running;
    }
}

TEST_CASE ("prepare, then load, then silence one stem, and its tone is gone", "[stems][queue][engine]")
{
    Sandbox sandbox;
    sandbox.writeSeparator();

    opendj::AudioEngine engine (StemSeparator::Models::ignoreInstalled);

    giveRoom (engine.getStemSeparator());

    auto& deck = engine.getDeck (0);

    engine.prepareStems ({ sandbox.song });
    REQUIRE (eventually ([&] { return engine.getStemSeparator().isCached (sandbox.song); }));
    REQUIRE (eventually ([&] { return isQueueIdle (engine); }));

    REQUIRE (engine.getStemQueueProgress().finished == 1);
    REQUIRE (! deck.hasStems());
    REQUIRE (sandbox.numRuns() == 1);

    // Nothing more goes to the separator from here on: the load finds them.
    engine.loadTrackAsync (0, sandbox.song);
    REQUIRE (eventually ([&] { return deck.hasStems(); }));
    REQUIRE (sandbox.numRuns() == 1);

    constexpr int blockSize = 512;
    deck.prepare (fileRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);

    const auto capture = [&] (int blocks)
    {
        std::vector<float> samples;

        for (int i = 0; i < blocks; ++i)
        {
            deck.processBlock (buffer);
            samples.insert (samples.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
        }

        return samples;
    };

    deck.play();
    capture (20);

    const auto before = capture (100);

    for (int stem = 0; stem < opendj::numStems; ++stem)
        REQUIRE_THAT (toneLevel (before, stemTones[stem], fileRate), WithinAbs ((double) stemLevel, 0.01));

    deck.setStemGain (opendj::Stem::vocals, 0.0f);
    capture (20);

    const auto after = capture (100);

    REQUIRE (toneLevel (after, stemTones[3], fileRate) < 0.001);

    for (int stem = 0; stem < 3; ++stem)
        REQUIRE_THAT (toneLevel (after, stemTones[stem], fileRate), WithinAbs ((double) stemLevel, 0.01));
}

TEST_CASE ("a prepared track on a deck with no stems gets them, and no other deck does", "[stems][queue][engine]")
{
    Sandbox sandbox;
    sandbox.writeSeparator();

    opendj::AudioEngine engine (StemSeparator::Models::ignoreInstalled);

    giveRoom (engine.getStemSeparator());

    const auto other = otherTrack (sandbox);

    REQUIRE (engine.getDeck (0).loadFile (sandbox.song));
    REQUIRE (engine.getDeck (1).loadFile (other));
    REQUIRE (engine.getDeck (2).loadFile (sandbox.song));
    REQUIRE (! engine.getDeck (0).hasStems());

    engine.prepareStems ({ sandbox.song });

    REQUIRE (eventually ([&] { return engine.getStemSeparator().isCached (sandbox.song); }));
    REQUIRE (eventually ([&] { return isQueueIdle (engine); }));

    REQUIRE (engine.getDeck (0).hasStems());
    REQUIRE (engine.getDeck (2).hasStems());
    REQUIRE (! engine.getDeck (1).hasStems());
    REQUIRE (! engine.getDeck (3).hasStems());

    // Two decks, and one decode between them.
    REQUIRE (engine.getStemSeparator().getCacheDecodeCount() == 1);
}

TEST_CASE ("a track prepared with no deck holding it keeps no audio in memory", "[stems][queue][engine]")
{
    Sandbox sandbox;
    sandbox.writeSeparator();

    // prepare hands nothing back, so the decks are the only place it could end
    // up, and the one here is holding something else.
    opendj::AudioEngine engine (StemSeparator::Models::ignoreInstalled);

    giveRoom (engine.getStemSeparator());

    const auto other = otherTrack (sandbox);
    REQUIRE (engine.getDeck (0).loadFile (other));

    engine.prepareStems ({ sandbox.song });
    REQUIRE (eventually ([&] { return engine.getStemSeparator().isCached (sandbox.song); }));
    REQUIRE (eventually ([&] { return isQueueIdle (engine); }));

    // A deck would refuse stems for another track anyway, so what matters is
    // that they were never decoded to be offered.
    REQUIRE (engine.getStemSeparator().getCacheDecodeCount() == 0);

    for (int i = 0; i < opendj::AudioEngine::numDecks; ++i)
        REQUIRE (engine.getDeck (i).getSeparation() == nullptr);
}

TEST_CASE ("a playlist's missing files are left out, rather than halting the rest", "[stems][queue][engine]")
{
    Sandbox sandbox;
    sandbox.writeSeparator();

    opendj::AudioEngine engine (StemSeparator::Models::ignoreInstalled);

    giveRoom (engine.getStemSeparator());

    // Moved or deleted since they went in the playlist, three together.
    engine.prepareStems ({ sandbox.root.getChildFile ("gone 1.wav"), sandbox.root.getChildFile ("gone 2.wav"),
                           sandbox.root.getChildFile ("gone 3.wav"), sandbox.song });

    REQUIRE (eventually ([&] { return engine.getStemSeparator().isCached (sandbox.song); }));
    REQUIRE (eventually ([&] { return isQueueIdle (engine); }));

    const auto progress = engine.getStemQueueProgress();
    REQUIRE (progress.halt == StemQueue::Halt::none);
    REQUIRE (progress.total == 1);
    REQUIRE (progress.finished == 1);
}

TEST_CASE ("the engine's decks hold back the background lane", "[stems][queue][engine]")
{
    Sandbox sandbox;
    sandbox.writeSeparator ({}, "echo running >> '" + sandbox.marker.getFullPathName() + "'\nexec sleep 300");

    opendj::AudioEngine engine (StemSeparator::Models::ignoreInstalled);

    giveRoom (engine.getStemSeparator());

    const auto other = otherTrack (sandbox);
    auto& deck = engine.getDeck (1);

    REQUIRE (deck.loadFile (other));
    deck.prepare (fileRate, 512);
    deck.play();
    REQUIRE (deck.isPlaying());

    // Playing, so nothing starts.
    engine.prepareStems ({ sandbox.song });
    giveItTheChanceToGoWrong();
    REQUIRE (sandbox.numRuns() == 0);
    REQUIRE (engine.isPreparingStems());

    // Stopped, so it does.
    deck.pause();
    REQUIRE (! deck.isPlaying());
    REQUIRE (eventually ([&] { return sandbox.numRuns() == 1; }));

    // Playing again, so it stops, and is still in the queue.
    deck.play();
    REQUIRE (eventually ([&] { return ! engine.getStemQueueProgress().running; }));
    REQUIRE (engine.isPreparingStems());
    REQUIRE (sandbox.numRuns() == 1);

    // Which goes through the same door a controller's button does.
    opendj::ActionDispatcher dispatcher (engine);
    dispatcher.dispatch ({ opendj::Action::stemsPrepareCancel, 0, 0, 0.0f });
    REQUIRE (engine.isPreparingStems());

    dispatcher.dispatch ({ opendj::Action::stemsPrepareCancel, 0, 0, 1.0f });
    REQUIRE (! engine.isPreparingStems());

    deck.pause();
    giveItTheChanceToGoWrong();
    REQUIRE (sandbox.numRuns() == 1);
}

TEST_CASE ("a new track on a deck can ask for stems while the last one's are running", "[stems][queue][engine]")
{
    Sandbox sandbox;

    // Slow enough for the deck to move on while it runs.
    sandbox.writeSeparator ("sleep 0.5");

    opendj::AudioEngine engine (StemSeparator::Models::ignoreInstalled);
    auto& deck = engine.getDeck (0);
    auto* messages = juce::MessageManager::getInstance();

    const auto other = otherTrack (sandbox);
    REQUIRE (deck.loadFile (sandbox.song));

    auto firstTold = 0, secondTold = 0;
    auto firstOk = false, secondOk = false;

    engine.separateDeckAsync (0, [&] (bool ok) { ++firstTold; firstOk = ok; });
    REQUIRE (engine.isDeckSeparating (0));

    REQUIRE (deck.loadFile (other));
    REQUIRE (! engine.isDeckSeparating (0));

    engine.separateDeckAsync (0, [&] (bool ok) { ++secondTold; secondOk = ok; });
    REQUIRE (engine.isDeckSeparating (0));

    for (int i = 0; i < 20000 && secondTold == 0; ++i)
        messages->runDispatchLoopUntil (1);

    REQUIRE (firstTold == 1);
    REQUIRE (firstOk);
    REQUIRE (secondTold == 1);
    REQUIRE (secondOk);

    REQUIRE (! engine.isDeckSeparating (0));
    REQUIRE (sandbox.numRuns() == 2);

    // The first track's stems stayed out of it. The second's are what is on it.
    REQUIRE (deck.getLoadedFile() == other);
    REQUIRE (deck.hasStems());
    REQUIRE (engine.getStemSeparator().isCached (sandbox.song));
}

TEST_CASE ("an entry that will not decode is thrown away, so that asking again works", "[stems][queue][engine]")
{
    Sandbox sandbox;
    sandbox.writeSeparator();

    opendj::AudioEngine engine (StemSeparator::Models::ignoreInstalled);
    auto& separator = engine.getStemSeparator();
    auto& deck = engine.getDeck (0);

    std::atomic<bool> stop { false };
    REQUIRE (separator.prepare (sandbox.formats, sandbox.song, {}, stop) == StemOutcome::ready);

    // The sizes still look right, which is all a look at the cache sees.
    REQUIRE (separator.getCacheFolder (sandbox.song).getChildFile ("bass.flac").replaceWithText ("not audio"));
    REQUIRE (separator.isCached (sandbox.song));

    REQUIRE (deck.loadFile (sandbox.song));
    REQUIRE (! deck.hasStems());

    engine.separateDeckAsync (0);

    REQUIRE (eventually ([&] { return ! engine.isDeckSeparating (0); }));
    REQUIRE (! separator.isCached (sandbox.song));
    REQUIRE (sandbox.numRuns() == 1);

    engine.separateDeckAsync (0);

    REQUIRE (eventually ([&] { return deck.hasStems(); }));
    REQUIRE (sandbox.numRuns() == 2);
}

TEST_CASE ("a deck asking with nothing to separate is told so", "[stems][queue][engine]")
{
    Sandbox sandbox;
    opendj::AudioEngine engine (StemSeparator::Models::ignoreInstalled);

    // There before the call, as it is in the application: a message posted when
    // there is no message manager goes nowhere.
    auto* messages = juce::MessageManager::getInstance();

    auto told = 0;
    auto answer = true;

    engine.separateDeckAsync (0, [&] (bool ok) { ++told; answer = ok; });

    for (int i = 0; i < 5000 && told == 0; ++i)
        messages->runDispatchLoopUntil (1);

    REQUIRE (told == 1);
    REQUIRE (! answer);
}

TEST_CASE ("stopping the engine kills a running separator and waits for it", "[stems][queue][engine]")
{
    Sandbox sandbox;

    const auto workerPid = sandbox.root.getChildFile ("worker.txt");
    sandbox.writeSeparator ({}, "sleep 300 &\necho $! > '" + workerPid.getFullPathName() + "'\nwait");

    opendj::AudioEngine engine (StemSeparator::Models::ignoreInstalled);
    REQUIRE (engine.getDeck (0).loadFile (sandbox.song));

    engine.separateDeckAsync (0);
    REQUIRE (eventually ([&] { return workerPid.loadFileAsString().trim().isNotEmpty(); }));

    const auto pid = workerPid.loadFileAsString().trim().getIntValue();
    REQUIRE (isAlive (pid));

    engine.stop();

    REQUIRE (eventually ([&] { return ! isAlive (pid); }));
    REQUIRE (! engine.isDeckSeparating (0));
    REQUIRE (! engine.getStemSeparator().getPartialFolder (sandbox.song).exists());
}

TEST_CASE ("the engine honours a free space probe, which is how these tests get their room", "[stems][queue][engine]")
{
    opendj::AudioEngine engine (StemSeparator::Models::ignoreInstalled);
    auto& separator = engine.getStemSeparator();

    const auto real = separator.getFreeBytes();
    REQUIRE (real > 0);

    separator.setFreeSpaceProbe ([] { return (juce::int64) 1234; });
    REQUIRE (separator.getFreeBytes() == 1234);
}

TEST_CASE ("three real failures halt the engine's background lane", "[stems][queue][engine]")
{
    Sandbox sandbox;
    sandbox.writeSeparator ({}, "kill -9 $$");

    opendj::AudioEngine engine (StemSeparator::Models::ignoreInstalled);

    giveRoom (engine.getStemSeparator());

    std::vector<juce::File> tracks;

    for (int i = 0; i < 5; ++i)
    {
        tracks.push_back (sandbox.root.getChildFile ("track" + juce::String (i) + ".wav"));
        REQUIRE (sandbox.song.copyFileTo (tracks.back()));
    }

    engine.prepareStems (tracks);

    REQUIRE (eventually ([&] { return engine.getStemQueueProgress().halt == StemQueue::Halt::failures; }));
    REQUIRE (eventually ([&] { return isQueueIdle (engine); }));
    REQUIRE (sandbox.numRuns() == StemQueue::failuresBeforeHalting);
}

#endif // ! JUCE_WINDOWS
