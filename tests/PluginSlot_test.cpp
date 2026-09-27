/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/MasterEffects.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>

using Catch::Matchers::WithinAbs;
using Type = opendj::MasterEffects::Type;

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    juce::dsp::ProcessSpec specFor (double rate = sampleRate)
    {
        return { rate, (juce::uint32) blockSize, 2 };
    }

    void fillSine (juce::AudioBuffer<float>& buffer, double frequency, double& phase, float amplitude)
    {
        const auto delta = juce::MathConstants<double>::twoPi * frequency / sampleRate;

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto value = static_cast<float> (std::sin (phase)) * amplitude;
            phase += delta;

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample (ch, i, value);
        }
    }

    float settledRms (opendj::MasterEffects& fx, double frequency = 440.0)
    {
        juce::AudioBuffer<float> buffer (2, blockSize);
        double phase = 0.0;

        for (int b = 0; b < 40; ++b)
        {
            fillSine (buffer, frequency, phase, 0.5f);
            fx.process (buffer, blockSize);
        }

        return buffer.getRMSLevel (0, 0, blockSize);
    }

    const float sineRms = 0.5f / std::sqrt (2.0f);

    /** The boilerplate every processor needs, for the stand-ins below. */
    class TestProcessor : public juce::AudioProcessor
    {
    public:
        using AudioProcessor::AudioProcessor;

        void releaseResources() override {}
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        double getTailLengthSeconds() const override { return 0.0; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}
    };

    /** A stand-in for a VST3: halves what it is given, has two parameters,
        and can claim a latency, which it then really has. */
    class HalfGain final : public TestProcessor
    {
    public:
        explicit HalfGain (int latencyToReport = 0, int* destroyedCounter = nullptr)
            : TestProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo())
                                              .withOutput ("Out", juce::AudioChannelSet::stereo())),
              latency (latencyToReport), destroyed (destroyedCounter)
        {
            addParameter (first = new juce::AudioParameterFloat (juce::ParameterID { "first", 1 }, "First", 0.0f, 1.0f, 0.25f));
            addParameter (second = new juce::AudioParameterFloat (juce::ParameterID { "second", 1 }, "Second", 0.0f, 1.0f, 0.75f));
            setLatencySamples (latency);
        }

        ~HalfGain() override
        {
            if (destroyed != nullptr)
                ++*destroyed;
        }

        void prepareToPlay (double rate, int) override
        {
            ++prepareCount;
            preparedRate = rate;
            history.setSize (2, latency + 1);
            history.clear();
            writePosition = 0;
        }

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                // A ring buffer one longer than the latency: what comes out
                // went in that many samples ago.
                const auto readPosition = (writePosition + 1) % (latency + 1);

                for (int ch = 0; ch < 2; ++ch)
                {
                    history.setSample (ch, writePosition, buffer.getSample (ch, i) * 0.5f);
                    buffer.setSample (ch, i, history.getSample (ch, latency == 0 ? writePosition : readPosition));
                }

                writePosition = (writePosition + 1) % (latency + 1);
            }
        }

        const juce::String getName() const override { return "Half gain"; }

        juce::AudioParameterFloat* first = nullptr;
        juce::AudioParameterFloat* second = nullptr;
        int prepareCount = 0;
        double preparedRate = 0.0;

    private:
        int latency = 0;
        int* destroyed = nullptr;
        juce::AudioBuffer<float> history;
        int writePosition = 0;
    };

    /** The slot set up as a plugin slot, fully on, the other slot out of the way. */
    void usePluginIn (opendj::MasterEffects& fx, int slot)
    {
        fx.setType (slot, Type::plugin);
        fx.setType (1 - slot, Type::filter);
        fx.setWet (slot, 1.0f);
        fx.setEnabled (slot, true);
    }
}

TEST_CASE ("A plugin in a master slot is heard at full wet", "[masterfx][plugin]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());

    REQUIRE (fx.setPlugin (0, std::make_unique<HalfGain>()));
    usePluginIn (fx, 0);

    CHECK_THAT (settledRms (fx), WithinAbs (sineRms * 0.5f, 0.01));
}

TEST_CASE ("A plugin slot that is empty, or off, leaves the mix alone", "[masterfx][plugin]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());
    fx.setType (0, Type::plugin);
    fx.setType (1, Type::filter);
    fx.setWet (0, 1.0f);

    SECTION ("empty but on")
    {
        fx.setEnabled (0, true);
        CHECK_THAT (settledRms (fx), WithinAbs (sineRms, 0.001));
    }

    SECTION ("loaded but off, bit for bit")
    {
        REQUIRE (fx.setPlugin (0, std::make_unique<HalfGain>()));

        juce::AudioBuffer<float> buffer (2, blockSize), original (2, blockSize);
        double phase = 0.0;

        for (int b = 0; b < 10; ++b)
        {
            fillSine (buffer, 440.0, phase, 0.5f);
            original.makeCopyOf (buffer);
            fx.process (buffer, blockSize);

            for (int i = 0; i < blockSize; ++i)
                REQUIRE (buffer.getSample (0, i) == original.getSample (0, i));
        }
    }
}

TEST_CASE ("A knob reaches the plugin parameter it is pointed at", "[masterfx][plugin]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());

    auto plugin = std::make_unique<HalfGain>();
    auto* raw = plugin.get();
    REQUIRE (fx.setPlugin (0, std::move (plugin)));
    usePluginIn (fx, 0);

    // The knobs start where the plugin's parameters already are.
    CHECK_THAT (fx.getParam (0, 0), WithinAbs (0.25f, 1e-6));
    CHECK_THAT (fx.getParam (0, 1), WithinAbs (0.75f, 1e-6));
    CHECK (fx.getParamLabel (0, 0) == "First");

    fx.setPluginParamIndex (0, 0, 1);   // knob 1 now drives the second parameter
    CHECK (fx.getParamLabel (0, 0) == "Second");
    CHECK_THAT (fx.getParam (0, 0), WithinAbs (0.75f, 1e-6));

    fx.setParam (0, 0, 0.1f);

    // It lands on the audio thread, with the next block.
    CHECK_THAT (raw->second->get(), WithinAbs (0.75f, 1e-6));
    settledRms (fx);
    CHECK_THAT (raw->second->get(), WithinAbs (0.1f, 1e-6));
    CHECK_THAT (raw->first->get(), WithinAbs (0.25f, 1e-6));

    // A move made in the plugin itself shows on the knob pointed at it.
    raw->first->setValueNotifyingHost (0.6f);
    fx.setPluginParamIndex (0, 1, 0);
    CHECK_THAT (fx.getParam (0, 1), WithinAbs (0.6f, 1e-6));
}

TEST_CASE ("A replaced plugin is freed on the message thread, not the audio one", "[masterfx][plugin]")
{
    int destroyed = 0;

    opendj::MasterEffects fx;
    fx.prepare (specFor());
    usePluginIn (fx, 0);

    REQUIRE (fx.setPlugin (0, std::make_unique<HalfGain> (0, &destroyed)));
    settledRms (fx);

    // Two in a row before the audio thread runs: the first of these was never
    // picked up, so it is freed on the spot.
    REQUIRE (fx.setPlugin (0, std::make_unique<HalfGain> (0, &destroyed)));
    REQUIRE (fx.setPlugin (0, std::make_unique<HalfGain> (0, &destroyed)));
    CHECK (destroyed == 1);

    // Swapping the newest in hands the original back, still alive.
    settledRms (fx);
    CHECK (destroyed == 1);

    fx.collectGarbage();
    CHECK (destroyed == 2);

    // Taking the plugin out leaves a straight wire, and the last one comes
    // back for collection in its turn.
    REQUIRE (fx.setPlugin (0, nullptr));
    CHECK_THAT (settledRms (fx), WithinAbs (sineRms, 0.001));
    fx.collectGarbage();
    CHECK (destroyed == 3);
}

TEST_CASE ("Changing plugin mid-signal does not click", "[masterfx][plugin]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());
    usePluginIn (fx, 0);
    REQUIRE (fx.setPlugin (0, std::make_unique<HalfGain>()));

    juce::AudioBuffer<float> buffer (2, blockSize);
    double phase = 0.0;
    float previous = 0.0f, biggestStep = 0.0f;
    const auto sineStep = 0.5f * (float) (juce::MathConstants<double>::twoPi * 1000.0 / sampleRate);

    for (int b = 0; b < 40; ++b)
    {
        if (b == 20)
            REQUIRE (fx.setPlugin (0, nullptr));   // from half gain to a wire

        fx.collectGarbage();
        fillSine (buffer, 1000.0, phase, 0.5f);
        fx.process (buffer, blockSize);

        for (int i = 0; i < blockSize; ++i)
        {
            if (b >= 10)
                biggestStep = juce::jmax (biggestStep, std::abs (buffer.getSample (0, i) - previous));

            previous = buffer.getSample (0, i);
        }
    }

    CHECK (biggestStep < sineStep * 1.2f);

    // And the wire really went in.
    CHECK_THAT (buffer.getRMSLevel (0, 0, blockSize), WithinAbs (sineRms, 0.01));
}

TEST_CASE ("A plugin follows the device to a new sample rate", "[masterfx][plugin]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());

    auto plugin = std::make_unique<HalfGain>();
    auto* raw = plugin.get();
    REQUIRE (fx.setPlugin (0, std::move (plugin)));
    CHECK (raw->preparedRate == sampleRate);

    fx.prepare (specFor (96000.0));
    CHECK (raw->preparedRate == 96000.0);
    CHECK (raw->prepareCount == 2);
}

TEST_CASE ("A plugin's latency holds the dry signal back to meet it", "[masterfx][plugin]")
{
    constexpr int latency = 100;

    opendj::MasterEffects fx;
    fx.prepare (specFor());
    usePluginIn (fx, 0);
    fx.setWet (0, 0.5f);
    REQUIRE (fx.setPlugin (0, std::make_unique<HalfGain> (latency)));

    // A click once the fades have settled. Half the dry plus half of a
    // half-gain wet, both held back by the latency, is one click of 0.75 and
    // nothing else; a dry path that was not held back would give two.
    const auto total = blockSize * 12;
    const auto click = blockSize * 8 + 50;
    juce::AudioBuffer<float> whole (2, total);
    whole.clear();
    whole.setSample (0, click, 1.0f);
    whole.setSample (1, click, 1.0f);

    for (int start = 0; start < total; start += blockSize)
    {
        juce::AudioBuffer<float> block (whole.getArrayOfWritePointers(), 2, start, blockSize);
        fx.process (block, blockSize);
    }

    CHECK_THAT (whole.getSample (0, click + latency), WithinAbs (0.75f, 1e-4));
    CHECK_THAT (whole.getSample (0, click), WithinAbs (0.0f, 1e-6));
}

TEST_CASE ("A plugin wanting more than two channels is turned away", "[masterfx][plugin]")
{
    struct Surround final : public TestProcessor
    {
        Surround() : TestProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::create5point1())
                                                     .withOutput ("Out", juce::AudioChannelSet::create5point1())) {}
        void prepareToPlay (double, int) override {}
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
        const juce::String getName() const override { return "Surround"; }
    };

    opendj::MasterEffects fx;
    fx.prepare (specFor());
    CHECK_FALSE (fx.setPlugin (0, std::make_unique<Surround>()));
    CHECK (fx.getPlugin (0) == nullptr);
}

TEST_CASE ("Stepping through effects skips an empty plugin slot", "[masterfx][plugin]")
{
    opendj::MasterEffects fx;
    fx.setType (0, Type::filter);
    fx.stepType (0);
    CHECK (fx.getType (0) == Type::echo);

    REQUIRE (fx.setPlugin (0, std::make_unique<HalfGain>()));
    fx.setType (0, Type::filter);
    fx.stepType (0);
    CHECK (fx.getType (0) == Type::plugin);
}
