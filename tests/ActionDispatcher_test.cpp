/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "control/ActionDispatcher.h"
#include "core/AudioEngine.h"

#include <cmath>
#include <memory>

using namespace opendj;

namespace
{
    /** Thirty seconds of kick on the beat and hat off it at 120 BPM, the same
        click the loop tests use, so a deck has a beat grid for the pads to
        lock loops to. */
    struct ClickTrack
    {
        ClickTrack()
        {
            constexpr double rate = 44100.0;
            const auto numSamples = static_cast<int> (30.0 * rate);
            const auto samplesPerBeat = 0.5 * rate;

            juce::AudioBuffer<float> content (2, numSamples);
            content.clear();

            const auto addHit = [&content, numSamples] (double at, double frequency, double decay, float amplitude)
            {
                const auto start = static_cast<int> (at);

                for (int i = 0; i < static_cast<int> (decay * 4.0) && start + i < numSamples; ++i)
                {
                    const auto phase = juce::MathConstants<double>::twoPi * frequency * i / 44100.0;
                    const auto value = static_cast<float> (std::sin (phase) * std::exp (-i / decay)) * amplitude;
                    content.addSample (0, start + i, value);
                    content.addSample (1, start + i, value);
                }
            };

            for (double beat = 0.0; beat * samplesPerBeat < numSamples; beat += 1.0)
            {
                addHit (beat * samplesPerBeat, 55.0, rate * 0.06, 0.8f);
                addHit ((beat + 0.5) * samplesPerBeat, 6000.0, rate * 0.01, 0.3f);
            }

            juce::WavAudioFormat wav;
            auto stream = std::make_unique<juce::FileOutputStream> (file.getFile());
            REQUIRE (stream->openedOk());

            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (stream.release(), rate, 2, 16, {}, 0));
            REQUIRE (writer != nullptr);
            REQUIRE (writer->writeFromAudioSampleBuffer (content, 0, numSamples));
        }

        juce::TemporaryFile file { ".wav" };
    };

    /** An engine with a click on deck A and a dispatcher driving it. */
    struct PadHarness
    {
        PadHarness()
        {
            engine.prepareToPlay (48000.0, 512);
            REQUIRE (engine.getDeck (0).loadFile (click.file.getFile()));
            REQUIRE (engine.getDeck (0).getAnalysis() != nullptr);
        }

        void setMode (int code) { dispatcher.dispatch ({ Action::padMode, 0, 0, (float) code / 127.0f }); }
        void pad (int slot, bool down) { dispatcher.dispatch ({ Action::padLoop, 0, slot, down ? 1.0f : 0.0f }); }

        ClickTrack click;
        AudioEngine engine;
        ActionDispatcher dispatcher { engine };
    };
}

TEST_CASE ("a pad mode is kept per deck, as the code the controller sent", "[control][pads]")
{
    AudioEngine engine;
    ActionDispatcher dispatcher (engine);

    REQUIRE (dispatcher.getPadMode (1) == 0);

    dispatcher.dispatch ({ Action::padMode, 1, 0, (float) 0x30 / 127.0f });

    REQUIRE (dispatcher.getPadMode (1) == 0x30);
    REQUIRE (dispatcher.getPadMode (0) == 0);
}

TEST_CASE ("in roll mode a loop pad rolls only while it is held", "[control][pads]")
{
    PadHarness h;
    auto& deck = h.engine.getDeck (0);

    h.setMode (0x11);

    h.pad (0, true);
    REQUIRE (deck.isLoopRolling());

    h.pad (0, false);
    REQUIRE (! deck.isLoopRolling());
    REQUIRE (! deck.isLoopEnabled());
}

TEST_CASE ("in loop mode a loop pad sets a loop that stays on", "[control][pads]")
{
    PadHarness h;
    auto& deck = h.engine.getDeck (0);

    h.setMode (0x10);

    h.pad (1, true);
    REQUIRE (deck.isLoopEnabled());
    REQUIRE (! deck.isLoopRolling());

    h.pad (1, false);
    REQUIRE (deck.isLoopEnabled());
}

TEST_CASE ("the FX depth knob starts armed to echo", "[control][fx]")
{
    AudioEngine engine;
    ActionDispatcher dispatcher (engine);

    REQUIRE (dispatcher.getFxDepthTarget (0) == 0);

    dispatcher.dispatch ({ Action::channelFxDepth, 0, 0, 0.6f });

    REQUIRE (engine.getMixer().getChannelEcho (0) > 0.0f);
    REQUIRE (engine.getMixer().getChannelReverb (0) == 0.0f);
}

TEST_CASE ("arming reverb moves the depth knob to reverb, not echo", "[control][fx]")
{
    AudioEngine engine;
    ActionDispatcher dispatcher (engine);

    // Echo first, so the test would fail if arming reverb only added a second
    // target rather than replacing the first.
    dispatcher.dispatch ({ Action::channelFxDepth, 0, 0, 0.5f });
    REQUIRE (engine.getMixer().getChannelEcho (0) > 0.0f);

    dispatcher.dispatch ({ Action::channelFxSelect, 0, 1, 1.0f });
    REQUIRE (dispatcher.getFxDepthTarget (0) == 1);

    dispatcher.dispatch ({ Action::channelFxDepth, 0, 0, 0.8f });

    REQUIRE (engine.getMixer().getChannelReverb (0) > 0.0f);

    // Echo is left exactly where the first turn put it: arming a different
    // effect must not touch the one that is no longer selected.
    REQUIRE_THAT (engine.getMixer().getChannelEcho (0), Catch::Matchers::WithinAbs (0.5, 0.0001));
}

TEST_CASE ("a select button only arms an effect on its own channel", "[control][fx]")
{
    AudioEngine engine;
    ActionDispatcher dispatcher (engine);

    dispatcher.dispatch ({ Action::channelFxSelect, 0, 1, 1.0f });

    REQUIRE (dispatcher.getFxDepthTarget (0) == 1);
    REQUIRE (dispatcher.getFxDepthTarget (1) == 0);   // untouched, still echo
}

TEST_CASE ("a select button releasing does not re-arm the effect it named", "[control][fx]")
{
    AudioEngine engine;
    ActionDispatcher dispatcher (engine);

    dispatcher.dispatch ({ Action::channelFxSelect, 0, 1, 1.0f });   // press
    dispatcher.dispatch ({ Action::channelFxSelect, 0, 0, 0.0f });   // a different button's release

    // The release carries value 0, which reads as "not pressed" for any
    // button action; the selection made by the press must still stand.
    REQUIRE (dispatcher.getFxDepthTarget (0) == 1);
}

TEST_CASE ("waveform zoom is a named action carrying signed rungs", "[control][waveform]")
{
    REQUIRE (actionFromString ("deck.waveform_zoom") == Action::deckWaveformZoom);
    REQUIRE (toString (Action::deckWaveformZoom) == "deck.waveform_zoom");

    // Rungs, not a normalised position, the same as the browse encoder.
    REQUIRE (isContinuous (Action::deckWaveformZoom));
}

TEST_CASE ("a zoom action reaches the handler with the rungs it carried", "[control][waveform]")
{
    AudioEngine engine;
    ActionDispatcher dispatcher (engine);

    auto steps = 0;
    dispatcher.waveformZoomHandler = [&steps] (int moved) { steps += moved; };

    dispatcher.dispatch ({ Action::deckWaveformZoom, 0, 0, 2.0f });
    dispatcher.dispatch ({ Action::deckWaveformZoom, 0, 0, -1.0f });

    REQUIRE (steps == 1);
}

TEST_CASE ("a zoom action with nobody listening is harmless", "[control][waveform]")
{
    AudioEngine engine;
    ActionDispatcher dispatcher (engine);

    dispatcher.dispatch ({ Action::deckWaveformZoom, 0, 0, 1.0f });
}

TEST_CASE ("master effect actions work the unit the deck number names", "[control][masterfx]")
{
    AudioEngine engine;
    ActionDispatcher dispatcher (engine);
    auto& fx = engine.getMixer().getMasterEffects();

    dispatcher.dispatch ({ Action::masterFxToggle, 1, 0, 1.0f });
    REQUIRE (fx.isEnabled (1));
    REQUIRE_FALSE (fx.isEnabled (0));

    // A release is not a second press.
    dispatcher.dispatch ({ Action::masterFxToggle, 1, 0, 0.0f });
    REQUIRE (fx.isEnabled (1));

    dispatcher.dispatch ({ Action::masterFxOn, 0, 0, 1.0f });
    REQUIRE (fx.isEnabled (0));
    dispatcher.dispatch ({ Action::masterFxOn, 0, 0, 0.0f });
    REQUIRE_FALSE (fx.isEnabled (0));

    dispatcher.dispatch ({ Action::masterFxType, 0, 2, 1.0f });
    REQUIRE (fx.getType (0) == MasterEffects::Type::filter);

    dispatcher.dispatch ({ Action::masterFxWet, 0, 0, 0.3f });
    dispatcher.dispatch ({ Action::masterFxParam, 0, 1, 0.9f });
    REQUIRE_THAT (fx.getWetFor (0, MasterEffects::Type::filter), Catch::Matchers::WithinAbs (0.3, 0.0001));
    REQUIRE_THAT (fx.getParamFor (0, MasterEffects::Type::filter, 1), Catch::Matchers::WithinAbs (0.9, 0.0001));

    dispatcher.dispatch ({ Action::masterFxNextType, 0, 0, 1.0f });
    REQUIRE (fx.getType (0) == MasterEffects::Type::flanger);

    // A third unit does not exist, and must not fall through to the second.
    dispatcher.dispatch ({ Action::masterFxToggle, 2, 0, 1.0f });
    REQUIRE (fx.isEnabled (1));

    REQUIRE (actionFromString ("master_fx.param") == Action::masterFxParam);
    REQUIRE (isContinuous (Action::masterFxWet));
    REQUIRE_FALSE (isContinuous (Action::masterFxToggle));
}

TEST_CASE ("the quantize button toggles on the press only", "[control][quantize]")
{
    AudioEngine engine;
    ActionDispatcher dispatcher (engine);
    auto& deck = engine.getDeck (1);

    REQUIRE_FALSE (deck.isQuantizeEnabled());

    dispatcher.dispatch ({ Action::deckQuantizeToggle, 1, 0, 1.0f });
    dispatcher.dispatch ({ Action::deckQuantizeToggle, 1, 0, 0.0f });
    REQUIRE (deck.isQuantizeEnabled());
    REQUIRE_FALSE (engine.getDeck (0).isQuantizeEnabled());

    dispatcher.dispatch ({ Action::deckQuantizeToggle, 1, 0, 1.0f });
    REQUIRE_FALSE (deck.isQuantizeEnabled());
}

TEST_CASE ("beat jump actions have names a mapping file can use", "[control][beatjump]")
{
    REQUIRE (actionFromString ("deck.beatjump_forward") == Action::beatJumpForward);
    REQUIRE (actionFromString ("deck.beatjump_back") == Action::beatJumpBack);
    REQUIRE (actionFromString ("deck.quantize_toggle") == Action::deckQuantizeToggle);
    REQUIRE (toString (Action::beatJumpBack) == "deck.beatjump_back");
}

TEST_CASE ("a sampler pad triggers the slot of the bank on show", "[control][sampler]")
{
    AudioEngine engine;
    ActionDispatcher dispatcher (engine);
    auto& sampler = engine.getSampler();

    auto sound = std::make_unique<DecodedAudio>();
    sound->audio.setSize (2, 4800);
    sound->audio.clear();
    REQUIRE (sampler.installSlot (Sampler::numPads + 2, std::move (sound), "bank B pad 3"));

    dispatcher.dispatch ({ Action::samplerNextBank, 0, 0, 1.0f });
    REQUIRE (sampler.getBank() == 1);

    dispatcher.dispatch ({ Action::samplerTrigger, 0, 2, 1.0f });
    REQUIRE (sampler.isSlotPlaying (Sampler::numPads + 2));
    REQUIRE_FALSE (sampler.isSlotPlaying (2));

    dispatcher.dispatch ({ Action::samplerBank, 0, 3, 1.0f });
    REQUIRE (sampler.getBank() == 3);
    REQUIRE (actionFromString ("sampler.bank") == Action::samplerBank);
}

TEST_CASE ("the deck's performance effects are reachable by name", "[control][performance]")
{
    PadHarness h;
    auto& deck = h.engine.getDeck (0);
    juce::AudioBuffer<float> buffer (2, 512);

    deck.play();
    deck.processBlock (buffer);

    // Only the press acts: a release is what follows it on any real button.
    h.dispatcher.dispatch ({ actionFromString ("deck.brake"), 0, 0, 0.0f });
    deck.processBlock (buffer);
    REQUIRE_FALSE (deck.isSpinningDown());

    h.dispatcher.dispatch ({ actionFromString ("deck.brake"), 0, 0, 1.0f });
    deck.processBlock (buffer);
    REQUIRE (deck.isSpinningDown());

    deck.play();
    deck.processBlock (buffer);
    REQUIRE_FALSE (deck.isSpinningDown());

    h.dispatcher.dispatch ({ actionFromString ("deck.backspin"), 0, 0, 1.0f });
    deck.processBlock (buffer);
    REQUIRE (deck.isSpinningDown());

    deck.play();
    deck.processBlock (buffer);

    h.dispatcher.dispatch ({ actionFromString ("deck.reverse_toggle"), 0, 0, 1.0f });
    REQUIRE (deck.isReverseEnabled());
    h.dispatcher.dispatch ({ actionFromString ("deck.reverse_toggle"), 0, 0, 0.0f });
    REQUIRE (deck.isReverseEnabled());
    h.dispatcher.dispatch ({ actionFromString ("deck.reverse_toggle"), 0, 0, 1.0f });
    REQUIRE_FALSE (deck.isReverseEnabled());

    // A censor is held, so the release counts as much as the press.
    h.dispatcher.dispatch ({ actionFromString ("deck.censor"), 0, 0, 1.0f });
    REQUIRE (deck.isCensorHeld());
    h.dispatcher.dispatch ({ actionFromString ("deck.censor"), 0, 0, 0.0f });
    REQUIRE_FALSE (deck.isCensorHeld());
}

TEST_CASE ("an echo out is a toggle on the channel, and loading a track ends it", "[control][echo]")
{
    PadHarness h;
    auto& mixer = h.engine.getMixer();

    h.dispatcher.dispatch ({ actionFromString ("mixer.echo_out_toggle"), 1, 0, 1.0f });
    REQUIRE (mixer.isChannelEchoOut (1));
    REQUIRE_FALSE (mixer.isChannelEchoOut (0));

    h.dispatcher.dispatch ({ actionFromString ("mixer.echo_out_toggle"), 1, 0, 0.0f });
    REQUIRE (mixer.isChannelEchoOut (1));

    h.dispatcher.dispatch ({ actionFromString ("mixer.echo_out_toggle"), 1, 0, 1.0f });
    REQUIRE_FALSE (mixer.isChannelEchoOut (1));

    h.dispatcher.dispatch ({ actionFromString ("mixer.echo_out_toggle"), 0, 0, 1.0f });
    REQUIRE (mixer.isChannelEchoOut (0));

    h.engine.loadTrackAsync (0, h.click.file.getFile());
    REQUIRE_FALSE (mixer.isChannelEchoOut (0));

    // The load runs on the engine's own pool; let it finish before the harness goes.
    for (int i = 0; i < 2000 && h.engine.isDeckLoading (0); ++i)
        juce::Thread::sleep (5);
}

TEST_CASE ("a master effect slot can be asked for each new effect by number", "[control][masterfx]")
{
    PadHarness h;
    auto& fx = h.engine.getMixer().getMasterEffects();

    for (const auto type : { MasterEffects::Type::flanger, MasterEffects::Type::phaser, MasterEffects::Type::gate,
                             MasterEffects::Type::bitcrusher, MasterEffects::Type::stutter })
    {
        h.dispatcher.dispatch ({ Action::masterFxType, 0, (int) type, 1.0f });
        REQUIRE (fx.getType (0) == type);
    }
}

TEST_CASE ("the engine tells the master effects where the music is", "[control][masterfx]")
{
    PadHarness h;
    auto& deck = h.engine.getDeck (0);
    auto& fx = h.engine.getMixer().getMasterEffects();
    const auto analysis = deck.getAnalysis();
    REQUIRE (analysis->hasTempo());

    // Audible and on the grid, seventeen and a quarter beats in.
    const auto seconds = analysis->firstBeatSeconds + 17.25 * analysis->secondsPerBeat();
    juce::AudioBuffer<float> deckBuffer (2, 512), fxBuffer (2, 512);

    deck.seekToSeconds (seconds);
    deck.processBlock (deckBuffer);
    deck.play();
    deck.processBlock (deckBuffer);

    h.engine.setEchoBeats (0, 1.0);    // which asks the engine to bring every tempo up to date

    fxBuffer.clear();
    fx.process (fxBuffer, 512);

    // Measured against the clock after the one block of position it moved.
    REQUIRE (std::abs (fx.getBeatClock() - (deck.getPositionSeconds() - analysis->firstBeatSeconds)
                                               / analysis->secondsPerBeat()) < 0.1);
}

TEST_CASE ("key shift and key match are reachable by name", "[control][key]")
{
    PadHarness h;
    auto& deck = h.engine.getDeck (0);
    REQUIRE (deck.getKeyShift() == 0);

    // Only the press acts.
    h.dispatcher.dispatch ({ actionFromString ("deck.key_up"), 0, 0, 1.0f });
    h.dispatcher.dispatch ({ actionFromString ("deck.key_up"), 0, 0, 0.0f });
    REQUIRE (deck.getKeyShift() == 1);

    h.dispatcher.dispatch ({ actionFromString ("deck.key_down"), 0, 0, 1.0f });
    h.dispatcher.dispatch ({ actionFromString ("deck.key_down"), 0, 0, 1.0f });
    REQUIRE (deck.getKeyShift() == -1);

    h.dispatcher.dispatch ({ actionFromString ("deck.key_reset"), 0, 0, 1.0f });
    REQUIRE (deck.getKeyShift() == 0);

    // Up against the end of its range it stays there.
    for (int i = 0; i < 30; ++i)
        h.dispatcher.dispatch ({ actionFromString ("deck.key_up"), 0, 0, 1.0f });

    REQUIRE (deck.getKeyShift() == Deck::maximumKeyShift);
}

TEST_CASE ("key match shifts to the deck a sync would follow", "[control][key]")
{
    PadHarness h;
    auto& follower = h.engine.getDeck (0);
    auto& leader = h.engine.getDeck (1);
    juce::AudioBuffer<float> buffer (2, 512);

    KnownTrack known;
    known.analysed = true;
    known.bpm = 120.0;
    known.key = MusicalKey::fromString ("Am");
    REQUIRE (follower.loadFile (h.click.file.getFile(), &known));

    known.key = MusicalKey::fromString ("Cm");
    REQUIRE (leader.loadFile (h.click.file.getFile(), &known));

    // With nothing playing there is nothing to follow, and nothing moves.
    h.dispatcher.dispatch ({ actionFromString ("deck.key_match"), 0, 0, 1.0f });
    REQUIRE (follower.getKeyShift() == 0);

    leader.play();
    leader.processBlock (buffer);

    h.dispatcher.dispatch ({ actionFromString ("deck.key_match"), 0, 0, 1.0f });
    REQUIRE (follower.getKeyShift() == 3);
    REQUIRE (follower.getEffectiveKey().toString() == "Cm");
}
