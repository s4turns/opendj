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
