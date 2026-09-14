/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/AudioEngine.h"

#include <array>
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;
using opendj::AudioEngine;
using opendj::Mixer;
using opendj::OutputMode;

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    /** A WAV of a steady tone, loud enough that any gain staging still leaves
        something to measure. Written to a real file because loading a deck is
        the path under test and it takes a file, not a buffer. */
    struct ToneFile
    {
        ToneFile (double seconds = 2.0, double frequency = 440.0)
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();

            const auto numSamples = (int) std::lround (seconds * sampleRate);
            juce::AudioBuffer<float> audio (2, numSamples);

            const auto delta = juce::MathConstants<double>::twoPi * frequency / sampleRate;

            for (int i = 0; i < numSamples; ++i)
            {
                const auto value = (float) std::sin (delta * i) * 0.8f;

                for (int ch = 0; ch < 2; ++ch)
                    audio.setSample (ch, i, value);
            }

            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> stream (file.getFile().createOutputStream());

            if (stream != nullptr)
                if (std::unique_ptr<juce::AudioFormatWriter> writer (
                        wav.createWriterFor (stream.get(), sampleRate, 2, 24, {}, 0));
                    writer != nullptr)
                {
                    stream.release();   // the writer owns it now
                    writer->writeFromAudioSampleBuffer (audio, 0, numSamples);
                }
        }

        juce::File get() const { return file.getFile(); }

        juce::TemporaryFile file { ".wav" };
    };

    /** An engine with somewhere to render into, and no sound card anywhere. */
    struct Harness
    {
        Harness()
        {
            engine.prepareToPlay (sampleRate, blockSize);

            // Everything off the crossfader, so a measurement is about the deck
            // and not about where the crossfader happens to be sitting.
            auto& mixer = engine.getMixer();
            mixer.setMasterGain (1.0f);

            for (int c = 0; c < Mixer::numChannels; ++c)
            {
                mixer.setChannelFader (c, 1.0f);
                mixer.setChannelCrossfaderAssign (c, Mixer::CrossfaderAssign::thru);
            }

            for (auto& channel : storage)
                channel.assign ((size_t) blockSize, 0.0f);

            for (size_t i = 0; i < storage.size(); ++i)
                outputs[i] = storage[i].data();
        }

        /** Renders enough blocks for the gain ramps to settle, then answers the
            loudest sample on the given output. */
        float render (int outputChannel, int blocks = 24)
        {
            auto peak = 0.0f;

            for (int block = 0; block < blocks; ++block)
            {
                engine.renderNextBlock (outputs.data(), (int) outputs.size(), blockSize);

                peak = 0.0f;

                for (const auto sample : storage[(size_t) outputChannel])
                    peak = juce::jmax (peak, std::abs (sample));
            }

            return peak;
        }

        /** The same, with something on the device's inputs. `input` is written
            to the given input channel before every block, which is what lets a
            test point an input at one of the engine's own output channels. */
        float renderWithInput (int outputChannel, std::vector<float>& input, float level, int blocks = 24)
        {
            auto peak = 0.0f;
            const float* inputs[] = { input.data() };

            for (int block = 0; block < blocks; ++block)
            {
                std::fill (input.begin(), input.end(), level);
                engine.renderNextBlock (outputs.data(), (int) outputs.size(), blockSize, inputs, 1);

                peak = 0.0f;

                for (const auto sample : storage[(size_t) outputChannel])
                    peak = juce::jmax (peak, std::abs (sample));
            }

            return peak;
        }

        void loadAndPlay (int deckIndex, const juce::File& file)
        {
            auto& deck = engine.getDeck (deckIndex);
            REQUIRE (deck.loadFile (file));
            REQUIRE (deck.isLoaded());
            deck.play();
        }

        AudioEngine engine;
        std::array<std::vector<float>, 4> storage;
        std::array<float*, 4> outputs {};
    };

    // The engine builds thread pools and a device manager, which want the JUCE
    // singletons to exist even though no device is ever opened here.
    struct ScopedJuce
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };
}

TEST_CASE ("every deck reaches the master bus", "[engine][decks]")
{
    ScopedJuce scoped;
    ToneFile tone;

    // The one that mattered: decks C and D were rendered into buffers with no
    // channels and no samples, because the callback built two views for four
    // decks. They were silent for as long as four decks existed, and the mixer
    // was handed pointers to nothing, which is where the crash on exit came
    // from. Every deck, every time, is the only assertion that catches it.
    for (int deckIndex = 0; deckIndex < AudioEngine::numDecks; ++deckIndex)
    {
        Harness h;
        h.loadAndPlay (deckIndex, tone.get());

        INFO ("deck " << deckIndex);
        REQUIRE (h.render (0) > 0.1f);
        REQUIRE (h.render (1) > 0.1f);
    }
}

TEST_CASE ("a deck with nothing on it is silent", "[engine][decks]")
{
    ScopedJuce scoped;
    Harness h;

    REQUIRE (h.render (0) == 0.0f);
}

TEST_CASE ("a loaded deck that is not playing is silent", "[engine][decks]")
{
    ScopedJuce scoped;
    ToneFile tone;
    Harness h;

    auto& deck = h.engine.getDeck (2);
    REQUIRE (deck.loadFile (tone.get()));

    REQUIRE (h.render (0) == 0.0f);
}

TEST_CASE ("two decks together are louder than one", "[engine][decks]")
{
    ScopedJuce scoped;
    ToneFile tone;

    // Well under the master soft clipper, which would otherwise squash the two
    // deck case back towards the one deck case and make this prove nothing.
    constexpr float quiet = 0.3f;

    auto one = 0.0f;

    {
        Harness h;
        h.engine.getMixer().setMasterGain (quiet);
        h.loadAndPlay (2, tone.get());
        one = h.render (0);
    }

    Harness h;
    h.engine.getMixer().setMasterGain (quiet);
    h.loadAndPlay (2, tone.get());
    h.loadAndPlay (3, tone.get());

    // Two of the four decks, and neither of them A or B. A build that quietly
    // dropped one would read the same as one deck.
    REQUIRE (one > 0.05f);
    REQUIRE (h.render (0) > one * 1.4f);
}

TEST_CASE ("the engine writes nothing past the end of a block", "[engine][decks]")
{
    ScopedJuce scoped;
    ToneFile tone;
    Harness h;

    h.loadAndPlay (0, tone.get());

    // A canary either side of a short block, to catch a render that trusts the
    // length it was prepared with rather than the one it was given.
    constexpr int shortBlock = 64;
    constexpr float canary = -12345.0f;

    for (auto& channel : h.storage)
        std::fill (channel.begin(), channel.end(), canary);

    h.engine.renderNextBlock (h.outputs.data(), (int) h.outputs.size(), shortBlock);

    for (const auto& channel : h.storage)
        for (size_t i = (size_t) shortBlock; i < channel.size(); ++i)
            REQUIRE (channel[i] == canary);
}

TEST_CASE ("a block longer than the engine prepared for comes out silent", "[engine][decks]")
{
    ScopedJuce scoped;
    ToneFile tone;
    Harness h;

    h.loadAndPlay (0, tone.get());

    // Growing a buffer here would allocate on the audio thread, so the block is
    // not rendered. The outputs are still cleared first, because silence is the
    // right answer for a block that cannot be filled: leaving whatever the
    // device had in the buffer would be noise.
    std::vector<float> wide ((size_t) blockSize * 4, -1.0f);
    std::array<float*, 4> outputs { wide.data(), wide.data(), wide.data(), wide.data() };

    h.engine.renderNextBlock (outputs.data(), (int) outputs.size(), blockSize * 4);

    for (const auto sample : wide)
        REQUIRE (sample == 0.0f);
}

TEST_CASE ("a mic that is off is not heard", "[engine][mic]")
{
    ScopedJuce scoped;
    Harness h;
    std::vector<float> input ((size_t) blockSize);

    REQUIRE (h.renderWithInput (0, input, 0.25f) == 0.0f);
}

TEST_CASE ("a mic reaches the speakers but not the headphones", "[engine][mic]")
{
    ScopedJuce scoped;
    Harness h;
    std::vector<float> input ((size_t) blockSize);

    h.engine.getMic().setEnabled (true);

    // Master on outputs 1 and 2, cue on 3 and 4: the voice is on the first
    // pair at its own level and nowhere in the second.
    REQUIRE_THAT (h.renderWithInput (0, input, 0.25f), WithinAbs (0.25f, 1.0e-3f));
    REQUIRE_THAT (h.renderWithInput (1, input, 0.25f), WithinAbs (0.25f, 1.0e-3f));
    REQUIRE (h.renderWithInput (2, input, 0.25f) == 0.0f);
}

TEST_CASE ("a mic kept out of the speakers is not heard in them", "[engine][mic]")
{
    ScopedJuce scoped;
    Harness h;
    std::vector<float> input ((size_t) blockSize);

    h.engine.getMic().setEnabled (true);
    h.engine.getMic().setRouting (opendj::MicInput::Routing::recordingOnly);

    REQUIRE (h.renderWithInput (0, input, 0.25f) == 0.0f);
}

TEST_CASE ("a mic on an input that shares memory with an output is still heard", "[engine][mic]")
{
    ScopedJuce scoped;
    Harness h;

    h.engine.getMic().setEnabled (true);

    // The input is the engine's own first output channel. Clearing the outputs
    // before reading the inputs would silence it, and the voice would never
    // arrive.
    REQUIRE_THAT (h.renderWithInput (0, h.storage[0], 0.25f), WithinAbs (0.25f, 1.0e-3f));
}
