/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/MasterEffects.h"
#include "core/Mixer.h"

#include <cmath>

using Catch::Matchers::WithinAbs;
using Type = opendj::MasterEffects::Type;

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    juce::dsp::ProcessSpec specFor (int maxBlock = blockSize)
    {
        return { sampleRate, (juce::uint32) maxBlock, 2 };
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

    /** Runs a sine through the effects for a while and returns the RMS of the
        last block, once every smoother has long settled. */
    float settledRms (opendj::MasterEffects& fx, double frequency)
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
}

TEST_CASE ("Master effects that are off leave the mix bit for bit alone", "[masterfx]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());

    // One slot of each kind, so the check covers every path that runs idle.
    for (auto type : { Type::echo, Type::reverb, Type::filter })
    {
        fx.setType (0, type);
        fx.setType (1, type);

        juce::AudioBuffer<float> buffer (2, blockSize), original (2, blockSize);
        double phase = 0.0;

        for (int b = 0; b < 20; ++b)
        {
            fillSine (buffer, 440.0, phase, 0.5f);
            original.makeCopyOf (buffer);
            fx.process (buffer, blockSize);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockSize; ++i)
                    REQUIRE (buffer.getSample (ch, i) == original.getSample (ch, i));
        }
    }
}

TEST_CASE ("A master echo repeats on the beat and dies away", "[masterfx]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());

    fx.setType (0, Type::echo);
    fx.setType (1, Type::filter);   // the other slot out of the way
    fx.setBeatSeconds (0.1);        // 4800 samples, a whole number of blocks off
    fx.setParam (0, 0, opendj::MasterEffects::normalisedForEchoDivision (3));   // one beat
    fx.setParam (0, 1, 0.5f);
    fx.setWet (0, 1.0f);
    fx.setEnabled (0, true);

    CHECK_THAT (fx.getEchoSeconds (0), WithinAbs (0.1, 1e-9));

    // A single click, then silence: the repeats are all that can come back.
    // It comes a few blocks in, once the send has faded up.
    const auto total = blockSize * 40;
    const auto click = blockSize * 4 + 100;
    juce::AudioBuffer<float> whole (2, total);
    whole.clear();
    whole.setSample (0, click, 1.0f);
    whole.setSample (1, click, 1.0f);

    for (int start = 0; start < total; start += blockSize)
    {
        juce::AudioBuffer<float> block (whole.getArrayOfWritePointers(), 2, start, blockSize);
        fx.process (block, blockSize);
    }

    const auto delay = 4800;
    const auto first = std::abs (whole.getSample (0, click + delay));
    const auto second = std::abs (whole.getSample (0, click + 2 * delay));

    CHECK (first > 0.5f);
    CHECK (second > 0.05f);
    CHECK (second < first);

    // Nothing in between the taps: an echo you can hear the repeats of.
    CHECK (std::abs (whole.getSample (0, click + delay / 2)) < 1e-4f);
}

TEST_CASE ("The first echo parameter snaps to a musical division", "[masterfx]")
{
    using opendj::MasterEffects;

    CHECK (MasterEffects::echoDivisionIndexFor (0.0f) == 0);
    CHECK (MasterEffects::echoDivisionIndexFor (1.0f) == 5);
    CHECK (MasterEffects::echoDivisionIndexFor (0.6f) == 3);

    for (int i = 0; i < (int) MasterEffects::echoDivisions.size(); ++i)
        CHECK (MasterEffects::echoDivisionIndexFor (MasterEffects::normalisedForEchoDivision (i)) == i);

    MasterEffects fx;
    fx.setBeatSeconds (0.5);
    fx.setParamFor (0, Type::echo, 0, MasterEffects::normalisedForEchoDivision (1));   // a quarter beat
    CHECK_THAT (fx.getEchoSeconds (0), WithinAbs (0.125, 1e-9));
}

TEST_CASE ("A master reverb switched off lets its tail ring out", "[masterfx]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());

    fx.setType (0, Type::reverb);
    fx.setType (1, Type::filter);
    fx.setWet (0, 1.0f);
    fx.setEnabled (0, true);

    juce::AudioBuffer<float> buffer (2, blockSize);
    double phase = 0.0;

    for (int b = 0; b < 20; ++b)
    {
        fillSine (buffer, 440.0, phase, 0.5f);
        fx.process (buffer, blockSize);
    }

    // Off, and the input stops: whatever comes out now is the tail.
    fx.setEnabled (0, false);

    buffer.clear();
    fx.process (buffer, blockSize);
    buffer.clear();
    fx.process (buffer, blockSize);

    CHECK (buffer.getRMSLevel (0, 0, blockSize) > 0.01f);
}

TEST_CASE ("A master filter turned down takes the top off and is open at centre", "[masterfx]")
{
    const auto measure = [] (float cutoff, double frequency)
    {
        opendj::MasterEffects fx;
        fx.prepare (specFor());
        fx.setType (0, Type::filter);
        fx.setType (1, Type::filter);
        fx.setParam (0, 0, cutoff);
        fx.setWet (0, 1.0f);
        fx.setEnabled (0, true);
        return settledRms (fx, frequency);
    };

    const auto sineRms = 0.5f / std::sqrt (2.0f);

    CHECK_THAT (measure (0.5f, 10000.0), WithinAbs (sineRms, 0.02));
    CHECK_THAT (measure (0.5f, 100.0), WithinAbs (sineRms, 0.02));

    CHECK (measure (0.1f, 10000.0) < sineRms * 0.1f);
    CHECK_THAT (measure (0.1f, 100.0), WithinAbs (sineRms, 0.1));

    // And the other way, a high pass.
    CHECK (measure (0.9f, 100.0) < sineRms * 0.1f);
}

TEST_CASE ("Changing a master effect mid-signal does not click", "[masterfx]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());

    fx.setType (0, Type::filter);
    fx.setType (1, Type::filter);
    fx.setParam (0, 0, 0.1f);    // a heavy low pass, so dropping it would jump
    fx.setWet (0, 1.0f);
    fx.setEnabled (0, true);

    juce::AudioBuffer<float> buffer (2, blockSize);
    double phase = 0.0;
    float previous = 0.0f;
    float biggestStep = 0.0f;

    // A sine's own largest step at this frequency and level, for comparison.
    const auto sineStep = 0.5f * (float) (juce::MathConstants<double>::twoPi * 2000.0 / sampleRate);

    for (int b = 0; b < 40; ++b)
    {
        if (b == 20)
            fx.setType (0, Type::echo);

        fillSine (buffer, 2000.0, phase, 0.5f);
        fx.process (buffer, blockSize);

        for (int i = 0; i < blockSize; ++i)
        {
            if (b >= 10)
                biggestStep = juce::jmax (biggestStep, std::abs (buffer.getSample (0, i) - previous));

            previous = buffer.getSample (0, i);
        }
    }

    CHECK (fx.getType (0) == Type::echo);
    CHECK (biggestStep < sineStep * 1.5f);
}

TEST_CASE ("Each master effect keeps its own settings", "[masterfx]")
{
    opendj::MasterEffects fx;

    fx.setType (0, Type::echo);
    fx.setWet (0, 0.9f);
    fx.setParam (0, 1, 0.2f);

    fx.setType (0, Type::reverb);
    fx.setWet (0, 0.1f);
    CHECK_THAT (fx.getWet (0), WithinAbs (0.1f, 1e-6));

    fx.setType (0, Type::echo);
    CHECK_THAT (fx.getWet (0), WithinAbs (0.9f, 1e-6));
    CHECK_THAT (fx.getParam (0, 1), WithinAbs (0.2f, 1e-6));

    fx.stepType (0);
    CHECK (fx.getType (0) == Type::reverb);
    fx.stepType (0);
    fx.stepType (0);
    CHECK (fx.getType (0) == Type::echo);

    fx.toggleEnabled (1);
    CHECK (fx.isEnabled (1));
    CHECK_FALSE (fx.isEnabled (0));
}

TEST_CASE ("A block longer than promised is processed, not read past", "[masterfx]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor (64));

    fx.setType (0, Type::reverb);
    fx.setWet (0, 1.0f);
    fx.setEnabled (0, true);

    juce::AudioBuffer<float> buffer (2, 1000);
    double phase = 0.0;
    fillSine (buffer, 440.0, phase, 0.5f);

    fx.process (buffer, 1000);

    for (int i = 0; i < 1000; ++i)
        REQUIRE (std::isfinite (buffer.getSample (0, i)));
}

TEST_CASE ("The mixer runs its master effects on the master bus", "[masterfx][mixer]")
{
    opendj::Mixer mixer;
    mixer.prepare (sampleRate, blockSize);
    mixer.setMasterGain (1.0f);
    mixer.setChannelFader (0, 1.0f);
    mixer.setCrossfaderCurve (opendj::Mixer::CrossfaderCurve::linear);
    mixer.setCrossfaderPosition (-1.0f);

    auto& fx = mixer.getMasterEffects();
    fx.setType (0, Type::filter);
    fx.setParam (0, 0, 0.05f);
    fx.setWet (0, 1.0f);
    fx.setEnabled (0, true);

    juce::AudioBuffer<float> deckA (2, blockSize), silent (2, blockSize), master (2, blockSize), cue (2, blockSize);
    silent.clear();
    std::array<juce::AudioBuffer<float>*, opendj::Mixer::numChannels> decks { &deckA, &silent, &silent, &silent };

    double phase = 0.0;

    for (int b = 0; b < 40; ++b)
    {
        fillSine (deckA, 10000.0, phase, 0.3f);
        mixer.processBlock (decks, master, cue);
    }

    CHECK (master.getRMSLevel (0, 0, blockSize) < 0.3f / std::sqrt (2.0f) * 0.1f);
}
