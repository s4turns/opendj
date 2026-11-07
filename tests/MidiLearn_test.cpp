/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "control/ActionDispatcher.h"
#include "control/MidiControlSurface.h"
#include "control/MidiLearn.h"
#include "core/AudioEngine.h"

#include <atomic>
#include <cstring>
#include <vector>

using namespace opendj;
using Catch::Matchers::WithinAbs;

namespace
{
    LearnTarget target (Action action, int deck = 0, int slot = 0) { return { action, deck, slot }; }

    /** Feeds a run of control changes on one number until it answers. */
    std::optional<MidiControl> sweep (MidiLearner& learner, int status, int number, const std::vector<int>& values)
    {
        std::optional<MidiControl> result;

        for (const auto value : values)
            if (! result.has_value())
                result = learner.feed (status, number, value);

        return result;
    }

    juce::File findShippedMapping()
    {
        auto directory = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

        for (int i = 0; i < 8; ++i)
        {
            const auto candidate = directory.getChildFile ("mappings/roland-dj-202.json");

            if (candidate.existsAsFile())
                return candidate;

            directory = directory.getParentDirectory();
        }

        return {};
    }

    /** An engine and a surface with a small mapping on disk, and no device: a
        message reaches it the same way, through injectMessage. */
    struct Harness
    {
        Harness()
        {
            const auto base = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("opendj-learn-test-" + juce::String (juce::Random::getSystemRandom().nextInt64()));
            shipped = base.getChildFile ("shipped");
            user = base.getChildFile ("user");
            REQUIRE (shipped.createDirectory());

            REQUIRE (shipped.getChildFile ("test.json").replaceWithText (R"({
                "name": "Test Controller",
                "deviceNameHints": ["Test"],
                "controls": [
                    { "name": "Play", "status": "0x90", "number": "0x00", "action": "deck.play_toggle", "deck": 0 },
                    { "name": "Shift", "status": "0x90", "number": "0x40", "action": "modifier.shift" }
                ]
            })"));

            surface.setUserMappingFolder (user);
            REQUIRE (surface.loadMappingsFromFolder (shipped) == 1);
            REQUIRE (surface.getSelectedMappingName() == "Test Controller");
        }

        ~Harness()
        {
            shipped.getParentDirectory().deleteRecursively();
        }

        void note (int number, int velocity = 127, int channel = 0)
        {
            surface.injectMessage (juce::MidiMessage (0x90 | channel, number, velocity));
        }

        void cc (int number, int value, int channel = 0)
        {
            surface.injectMessage (juce::MidiMessage (0xB0 | channel, number, value));
        }

        void learn (const LearnTarget& what)
        {
            surface.startLearning (what);
            REQUIRE (surface.isLearning());
        }

        juce::File savedFile() const { return user.getChildFile ("Test Controller.json"); }

        std::optional<MidiMapping> readSaved() const
        {
            MidiMapping mapping;
            juce::StringArray warnings;

            if (MidiMapping::loadFromFile (savedFile(), mapping, warnings).failed())
                return {};

            return mapping;
        }

        juce::File shipped, user;
        AudioEngine engine;
        ActionDispatcher dispatcher { engine };
        MidiControlSurface surface { engine, dispatcher };
    };
}

//==============================================================================
// The classifier
//==============================================================================

TEST_CASE ("a press is learned as a button, straight away", "[learn]")
{
    MidiLearner learner (target (Action::deckPlayToggle, 1));
    const auto control = learner.feed (0x91, 0x10, 127);

    REQUIRE (control.has_value());
    REQUIRE (control->status == 0x91);
    REQUIRE (control->number == 0x10);
    REQUIRE (control->mode == ValueMode::button);
    REQUIRE (control->action == Action::deckPlayToggle);
    REQUIRE (control->deck == 1);
    REQUIRE (control->feedbackNumber == 0x10);
    REQUIRE (control->name == "Deck B: deck.play_toggle");
}

TEST_CASE ("a release is not the control being touched", "[learn]")
{
    MidiLearner learner (target (Action::deckPlayToggle));

    // Hardware that was already down when learning began.
    REQUIRE_FALSE (learner.feed (0x80, 0x10, 0).has_value());
    REQUIRE_FALSE (learner.feed (0x90, 0x10, 0).has_value());
    REQUIRE_FALSE (learner.hasSeenAnything());
    REQUIRE_FALSE (learner.finish().has_value());

    REQUIRE (learner.feed (0x90, 0x10, 90).has_value());
}

TEST_CASE ("messages no control is learned from are passed over", "[learn]")
{
    MidiLearner learner (target (Action::deckPlayToggle));

    REQUIRE_FALSE (learner.feed (0xA0, 0x10, 40).has_value());   // aftertouch
    REQUIRE_FALSE (learner.feed (0xC0, 0x05, 0).has_value());    // a program change
    REQUIRE_FALSE (learner.feed (0xD0, 0x40, 0).has_value());    // channel pressure
    REQUIRE_FALSE (learner.feed (0xF8, 0x00, 0).has_value());    // the clock, which never stops
}

TEST_CASE ("a pad mode keeps its velocity as the value", "[learn]")
{
    MidiLearner learner (target (Action::padMode));
    const auto control = learner.feed (0x94, 0x00, 0x11);

    REQUIRE (control.has_value());
    REQUIRE (control->mode == ValueMode::absolute);
}

TEST_CASE ("a control change can be a button", "[learn]")
{
    MidiLearner learner (target (Action::deckCue));

    REQUIRE_FALSE (learner.feed (0xB0, 0x20, 0).has_value());

    const auto control = learner.feed (0xB0, 0x20, 127);
    REQUIRE (control.has_value());
    REQUIRE (control->mode == ValueMode::button);
    REQUIRE (control->feedbackNumber == -1);    // a note is what lights a button
}

TEST_CASE ("a fader is learned as a position once it has moved enough", "[learn]")
{
    MidiLearner learner (target (Action::channelFader, 2));

    for (int i = 0; i < MidiLearner::messagesToDecide - 1; ++i)
        REQUIRE_FALSE (learner.feed (0xB2, 0x13, 40 + i).has_value());

    const auto control = learner.feed (0xB2, 0x13, 50);
    REQUIRE (control.has_value());
    REQUIRE (control->mode == ValueMode::absolute);
    REQUIRE (control->number == 0x13 );
    REQUIRE (control->fineNumber == -1);
    REQUIRE (control->deck == 2);
}

TEST_CASE ("a 14-bit fader is learned as its pair, whichever half came first", "[learn]")
{
    for (const auto coarseFirst : { true, false })
    {
        INFO ("coarse first " << coarseFirst);
        MidiLearner learner (target (Action::deckTempo));
        std::optional<MidiControl> control;

        for (int i = 0; i < MidiLearner::messagesToDecide && ! control.has_value(); ++i)
        {
            control = learner.feed (0xB0, coarseFirst ? 0x09 : 0x29, 60 + i);

            if (! control.has_value())
                control = learner.feed (0xB0, coarseFirst ? 0x29 : 0x09, 20 * i);
        }

        REQUIRE (control.has_value());
        REQUIRE (control->mode == ValueMode::absolute14Bit);
        REQUIRE (control->number == 0x09);
        REQUIRE (control->fineNumber == 0x29);
    }
}

TEST_CASE ("two unrelated knobs are not taken for a pair", "[learn]")
{
    MidiLearner learner (target (Action::channelFilter));
    std::optional<MidiControl> control;

    for (int i = 0; i < MidiLearner::messagesToDecide && ! control.has_value(); ++i)
    {
        control = learner.feed (0xB0, 0x09, 60 + i);

        if (! control.has_value())
            learner.feed (0xB0, 0x0A, 20 + i);    // brushed against on the way
    }

    REQUIRE (control.has_value());
    REQUIRE (control->number == 0x09);
    REQUIRE (control->mode == ValueMode::absolute);
}

TEST_CASE ("a relative encoder is told apart by where its steps sit", "[learn]")
{
    SECTION ("either side of 64")
    {
        MidiLearner learner (target (Action::jogTurn));
        const auto control = sweep (learner, 0xB0, 0x22, { 65, 66, 63, 62, 65, 64 + 3 });
        REQUIRE (control.has_value());
        REQUIRE (control->mode == ValueMode::relativeOffset);
    }

    SECTION ("just above 0 and just below 128")
    {
        MidiLearner learner (target (Action::browseScroll));
        const auto control = sweep (learner, 0xBF, 0x00, { 1, 1, 127, 2, 126, 1 });
        REQUIRE (control.has_value());
        REQUIRE (control->mode == ValueMode::relativeTwosComplement);
    }

    SECTION ("only one way round")
    {
        MidiLearner learner (target (Action::jogTurn));
        const auto control = sweep (learner, 0xB0, 0x22, { 1, 1, 1, 1, 1, 1 });
        REQUIRE (control.has_value());
        REQUIRE (control->mode == ValueMode::relativeTwosComplement);
    }

    SECTION ("a relative action is not given a pair")
    {
        MidiLearner learner (target (Action::jogTurn));
        learner.feed (0xB0, 0x22 + 32, 65);
        const auto control = sweep (learner, 0xB0, 0x22, { 65, 66, 63, 62, 65, 67 });
        REQUIRE (control.has_value());
        REQUIRE (control->fineNumber == -1);
    }
}

TEST_CASE ("a pitch bend is a platter or a fader, by what it is learned for", "[learn]")
{
    {
        MidiLearner learner (target (Action::jogTurn));
        const auto control = learner.feed (0xE1, 0x12, 0x34);
        REQUIRE (control.has_value());
        REQUIRE (control->mode == ValueMode::absolutePosition14);
        REQUIRE (control->number == 0);     // its second byte is half a value, not a number
    }
    {
        MidiLearner learner (target (Action::deckTempo));
        const auto control = learner.feed (0xE1, 0x12, 0x34);
        REQUIRE (control.has_value());
        REQUIRE (control->mode == ValueMode::pitchBend14);
    }
    {
        MidiLearner learner (target (Action::deckPlayToggle));
        const auto control = learner.feed (0xE1, 0x12, 0x34);
        REQUIRE (control.has_value());
        REQUIRE (control->mode == ValueMode::button);
    }
}

TEST_CASE ("a knob only just touched is learned from what was seen", "[learn]")
{
    MidiLearner learner (target (Action::channelEq, 0, 1));

    REQUIRE_FALSE (learner.finish().has_value());
    REQUIRE_FALSE (learner.feed (0xB0, 0x07, 70).has_value());
    REQUIRE (learner.hasSeenAnything());

    const auto control = learner.finish();
    REQUIRE (control.has_value());
    REQUIRE (control->mode == ValueMode::absolute);
    REQUIRE (control->slot == 1);
    REQUIRE (control->name == "Deck A: mixer.eq 2");
}

TEST_CASE ("only what an action uses is in its name", "[learn]")
{
    REQUIRE (nameForLearnedControl (target (Action::crossfader, 3, 3)) == "mixer.crossfader");
    REQUIRE (nameForLearnedControl (target (Action::samplerTrigger, 3, 4)) == "sampler.trigger 5");
    REQUIRE (nameForLearnedControl (target (Action::hotCue, 1, 7)) == "Deck B: pad.hotcue 8");
}

//==============================================================================
// Saving a mapping
//==============================================================================

TEST_CASE ("the shipped mapping survives being written out and read back", "[learn][mapping]")
{
    const auto file = findShippedMapping();
    REQUIRE (file.existsAsFile());

    MidiMapping original;
    juce::StringArray warnings;
    REQUIRE (MidiMapping::loadFromFile (file, original, warnings).wasOk());
    REQUIRE (original.controls.size() > 50);

    juce::TemporaryFile saved (".json");
    REQUIRE (original.saveToFile (saved.getFile()).wasOk());

    MidiMapping reloaded;
    juce::StringArray reloadWarnings;
    REQUIRE (MidiMapping::loadFromFile (saved.getFile(), reloaded, reloadWarnings).wasOk());
    REQUIRE (reloadWarnings.isEmpty());

    REQUIRE (reloaded.name == original.name);
    REQUIRE (reloaded.author == original.author);
    REQUIRE (reloaded.description == original.description);
    REQUIRE (reloaded.deviceNameHints == original.deviceNameHints);
    REQUIRE (reloaded.jogTicksPerRevolution == original.jogTicksPerRevolution);
    REQUIRE (reloaded.keepAliveIntervalMs == original.keepAliveIntervalMs);

    const auto sameMessages = [] (const std::vector<juce::MidiMessage>& a, const std::vector<juce::MidiMessage>& b)
    {
        REQUIRE (a.size() == b.size());

        for (size_t i = 0; i < a.size(); ++i)
        {
            REQUIRE (a[i].getRawDataSize() == b[i].getRawDataSize());
            REQUIRE (std::memcmp (a[i].getRawData(), b[i].getRawData(), (size_t) a[i].getRawDataSize()) == 0);
        }
    };

    REQUIRE_FALSE (original.initMessages.empty());
    sameMessages (original.initMessages, reloaded.initMessages);
    sameMessages (original.keepAliveMessages, reloaded.keepAliveMessages);

    REQUIRE (reloaded.controls.size() == original.controls.size());

    for (size_t i = 0; i < original.controls.size(); ++i)
    {
        const auto& a = original.controls[i];
        const auto& b = reloaded.controls[i];

        INFO (a.name);
        REQUIRE (b.name == a.name);
        REQUIRE (b.status == a.status);
        REQUIRE (b.number == a.number);
        REQUIRE (b.fineNumber == a.fineNumber);
        REQUIRE (b.action == a.action);
        REQUIRE (b.deck == a.deck);
        REQUIRE (b.slot == a.slot);
        REQUIRE (b.mode == a.mode);
        REQUIRE (b.inverted == a.inverted);
        REQUIRE (b.requiresShift == a.requiresShift);
        REQUIRE (b.ticksPerRevolution == a.ticksPerRevolution);
        REQUIRE (b.feedbackNumber == a.feedbackNumber);

        if (a.feedbackNumber >= 0)
        {
            REQUIRE (b.feedbackOnVelocity == a.feedbackOnVelocity);
            REQUIRE (b.feedbackOffVelocity == a.feedbackOffVelocity);
        }
    }
}

TEST_CASE ("a control replaces only what answers to the same message", "[learn][mapping]")
{
    MidiMapping mapping;

    MidiControl play;
    play.name = "Play";
    play.status = 0x90;
    play.number = 1;

    auto shifted = play;
    shifted.name = "Shifted";
    shifted.requiresShift = true;

    auto other = play;
    other.name = "Other";
    other.number = 2;

    auto elsewhere = play;
    elsewhere.name = "Elsewhere";
    elsewhere.status = 0x91;

    REQUIRE (mapping.addOrReplace (play).isEmpty());
    REQUIRE (mapping.addOrReplace (shifted).isEmpty());
    REQUIRE (mapping.addOrReplace (other).isEmpty());
    REQUIRE (mapping.addOrReplace (elsewhere).isEmpty());
    REQUIRE (mapping.controls.size() == 4);

    auto replacement = play;
    replacement.name = "Replacement";
    REQUIRE (mapping.addOrReplace (replacement) == "Play");
    REQUIRE (mapping.controls.size() == 4);
    REQUIRE (mapping.findControl (0x90, 1, false)->name == "Replacement");
    REQUIRE (mapping.findControl (0x90, 1, true)->name == "Shifted");
}

TEST_CASE ("either half of a 14-bit pair counts as the same control", "[learn][mapping]")
{
    MidiMapping mapping;

    MidiControl fader;
    fader.name = "Fader";
    fader.status = 0xB0;
    fader.number = 9;
    fader.fineNumber = 41;
    mapping.addOrReplace (fader);

    MidiControl fine;
    fine.name = "Fine half";
    fine.status = 0xB0;
    fine.number = 41;
    REQUIRE (mapping.addOrReplace (fine) == "Fader");
    REQUIRE (mapping.controls.size() == 1);

    // And the other way about.
    fader.name = "Pair";
    REQUIRE (mapping.addOrReplace (fader) == "Fine half");
    REQUIRE (mapping.controls.size() == 1);
}

//==============================================================================
// Learning from the surface
//==============================================================================

TEST_CASE ("a learned control works at once, and is kept", "[learn][surface]")
{
    Harness h;
    auto& deck = h.engine.getDeck (1);

    h.learn (target (Action::deckKeyUp, 1));
    h.note (0x20);

    // Taken for learning, not acted on, and not yet applied.
    h.surface.pollLearning();

    REQUIRE_FALSE (h.surface.isLearning());
    REQUIRE (h.surface.takeLearnResult().contains ("Deck B: deck.key_up"));
    REQUIRE (h.surface.takeLearnResult().isEmpty());
    REQUIRE (deck.getKeyShift() == 0);

    h.note (0x20);
    REQUIRE (deck.getKeyShift() == 1);

    const auto saved = h.readSaved();
    REQUIRE (saved.has_value());
    REQUIRE (saved->name == "Test Controller");
    REQUIRE (saved->findControl (0x90, 0x20, false) != nullptr);
    REQUIRE (saved->findControl (0x90, 0x00, false) != nullptr);    // what it had before is there too

    // The shipped file is left as it was.
    MidiMapping shipped;
    juce::StringArray warnings;
    REQUIRE (MidiMapping::loadFromFile (h.shipped.getChildFile ("test.json"), shipped, warnings).wasOk());
    REQUIRE (shipped.findControl (0x90, 0x20, false) == nullptr);
}

TEST_CASE ("a saved mapping takes the shipped one's place next time", "[learn][surface]")
{
    Harness h;

    h.learn (target (Action::deckKeyUp));
    h.note (0x21);
    h.surface.pollLearning();

    AudioEngine engine;
    ActionDispatcher dispatcher (engine);
    MidiControlSurface again (engine, dispatcher);

    // In the order the application looks: the user's folder first.
    REQUIRE (again.loadMappingsFromFolder (h.user) == 1);
    REQUIRE (again.loadMappingsFromFolder (h.shipped) == 0);
    REQUIRE (again.getSelectedMappingName() == "Test Controller");

    again.injectMessage (juce::MidiMessage (0x90, 0x21, 127));
    REQUIRE (engine.getDeck (0).getKeyShift() == 1);
}

TEST_CASE ("learning over a control replaces it", "[learn][surface]")
{
    Harness h;
    auto& deck = h.engine.getDeck (0);

    h.learn (target (Action::deckKeyDown));
    h.note (0x00);          // what used to be play
    h.surface.pollLearning();

    const auto result = h.surface.takeLearnResult();
    INFO (result);
    REQUIRE (result.contains ("which was Play"));

    h.note (0x00);
    REQUIRE (deck.getKeyShift() == -1);
    REQUIRE_FALSE (deck.isPlaying());
}

TEST_CASE ("a control learned with Shift held only answers with Shift held", "[learn][surface]")
{
    Harness h;
    auto& deck = h.engine.getDeck (0);

    // Shift goes through to the dispatcher during learning, so it can be held.
    h.learn (target (Action::deckKeyUp));
    h.note (0x40);
    REQUIRE (h.dispatcher.isShiftHeld());
    REQUIRE (h.surface.isLearning());

    h.note (0x22);
    h.note (0x40, 0);
    REQUIRE_FALSE (h.dispatcher.isShiftHeld());
    h.surface.pollLearning();

    h.note (0x22);
    REQUIRE (deck.getKeyShift() == 0);

    h.note (0x40);
    h.note (0x22);
    REQUIRE (deck.getKeyShift() == 1);
}

TEST_CASE ("Shift itself can be learned again", "[learn][surface]")
{
    Harness h;

    h.learn (target (Action::shift));
    h.note (0x40);
    REQUIRE_FALSE (h.dispatcher.isShiftHeld());
    h.surface.pollLearning();
    REQUIRE_FALSE (h.surface.isLearning());
}

TEST_CASE ("cancelling leaves the mapping alone, and messages work again", "[learn][surface]")
{
    Harness h;

    h.learn (target (Action::deckKeyUp));
    h.surface.cancelLearning();
    REQUIRE_FALSE (h.surface.isLearning());

    h.note (0x20);
    h.surface.pollLearning();

    REQUIRE (h.engine.getDeck (0).getKeyShift() == 0);
    REQUIRE_FALSE (h.surface.canUndoLearn());
    REQUIRE_FALSE (h.savedFile().existsAsFile());
}

TEST_CASE ("a knob only just touched is taken once it has stopped", "[learn][surface]")
{
    Harness h;

    h.learn (target (Action::channelFader, 1));
    h.cc (0x07, 40);
    h.cc (0x07, 41);

    // Still moving, as far as anyone can tell.
    h.surface.pollLearning();
    REQUIRE (h.surface.isLearning());

    juce::Thread::sleep (450);
    h.surface.pollLearning();
    REQUIRE_FALSE (h.surface.isLearning());

    h.cc (0x07, 127);
    REQUIRE_THAT (h.engine.getMixer().getChannelFader (1), WithinAbs (1.0f, 1.0e-5f));
}

TEST_CASE ("a fader sent as pitch bend moves its whole range once learned", "[learn][surface]")
{
    Harness h;

    h.learn (target (Action::channelFader, 0));
    h.surface.injectMessage (juce::MidiMessage (0xE3, 0x00, 0x40));
    h.surface.pollLearning();

    h.surface.injectMessage (juce::MidiMessage (0xE3, 0x7F, 0x7F));
    REQUIRE_THAT (h.engine.getMixer().getChannelFader (0), WithinAbs (1.0f, 1.0e-4f));

    h.surface.injectMessage (juce::MidiMessage (0xE3, 0x00, 0x00));
    REQUIRE_THAT (h.engine.getMixer().getChannelFader (0), WithinAbs (0.0f, 1.0e-4f));

    h.surface.injectMessage (juce::MidiMessage (0xE3, 0x00, 0x40));
    REQUIRE_THAT (h.engine.getMixer().getChannelFader (0), WithinAbs (0.5f, 1.0e-3f));
}

TEST_CASE ("undo puts a mapping back, on disk as well", "[learn][surface]")
{
    Harness h;
    auto& deck = h.engine.getDeck (0);

    h.learn (target (Action::deckKeyUp));
    h.note (0x00);
    h.surface.pollLearning();
    REQUIRE (h.surface.canUndoLearn());

    h.surface.undoLearn();
    REQUIRE_FALSE (h.surface.canUndoLearn());

    h.note (0x00);
    REQUIRE (deck.getKeyShift() == 0);
    REQUIRE (deck.getLoadedFile() == juce::File());

    const auto saved = h.readSaved();
    REQUIRE (saved.has_value());
    REQUIRE (saved->findControl (0x90, 0x00, false)->name == "Play");
}

TEST_CASE ("with no mapping chosen, learning starts one, and undo removes it", "[learn][surface]")
{
    Harness h;
    REQUIRE (h.surface.selectMapping ({}));

    h.learn (target (Action::deckKeyUp));
    h.note (0x23);
    h.surface.pollLearning();

    REQUIRE (h.surface.getSelectedMappingName() == "My controller");
    REQUIRE (h.surface.getMappingNames().size() == 2);

    const auto file = h.user.getChildFile ("My controller.json");
    REQUIRE (file.existsAsFile());

    h.note (0x23);
    REQUIRE (h.engine.getDeck (0).getKeyShift() == 1);

    h.surface.undoLearn();
    REQUIRE (h.surface.getMappingNames().size() == 1);
    REQUIRE (h.surface.getSelectedMappingName().isEmpty());
    REQUIRE_FALSE (file.existsAsFile());
}

TEST_CASE ("a mapping that cannot be saved says so, and still works", "[learn][surface]")
{
    Harness h;

    // A file where the folder should go.
    const auto blocked = h.shipped.getParentDirectory().getChildFile ("a file");
    REQUIRE (blocked.replaceWithText ("in the way"));
    h.surface.setUserMappingFolder (blocked);

    h.learn (target (Action::deckKeyUp));
    h.note (0x24);
    h.surface.pollLearning();

    REQUIRE (h.surface.takeLearnResult().contains ("Not saved"));

    h.note (0x24);
    REQUIRE (h.engine.getDeck (0).getKeyShift() == 1);
}

TEST_CASE ("messages and learning can arrive together without a race", "[learn][surface]")
{
    // Run under ThreadSanitizer this is the check on the lock. Without one it
    // still has to finish, with nothing lost and nothing dangling.
    Harness h;
    std::atomic<bool> keepGoing { true };

    juce::Thread::launch ([&h, &keepGoing]
    {
        while (keepGoing.load())
            h.surface.injectMessage (juce::MidiMessage (0xB0, 0x10, 64));
    });

    for (int i = 0; i < 200; ++i)
    {
        h.learn (target (Action::deckKeyUp));
        h.surface.injectMessage (juce::MidiMessage (0x90, 0x30 + (i % 16), 127));
        h.surface.pollLearning();
    }

    keepGoing.store (false);
    juce::Thread::sleep (20);

    REQUIRE (h.surface.getSelectedMappingName() == "Test Controller");
}
