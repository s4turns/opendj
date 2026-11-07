/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/MasterEffects.h"
#include "core/Mixer.h"

#include <array>
#include <cmath>
#include <utility>
#include <vector>

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

    void fillSine (juce::AudioBuffer<float>& buffer, double frequency, double& phase, float amplitude,
                    double deviceSampleRate = sampleRate)
    {
        const auto delta = juce::MathConstants<double>::twoPi * frequency / deviceSampleRate;

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

TEST_CASE ("master filters survive sweeps and Bluetooth sample rate changes", "[masterfx][filter][bluetooth]")
{
    opendj::MasterEffects fx;
    fx.setType (0, Type::filter);
    fx.setType (1, Type::filter);
    fx.setWet (0, 1.0f);
    fx.setEnabled (0, true);
    fx.setParam (0, 1, 1.0f);   // maximum resonance
    juce::AudioBuffer<float> buffer (2, blockSize);

    for (const auto rate : { 16000.0, 8000.0, 22050.0, 32000.0, 44100.0, 48000.0, 96000.0, 16000.0 })
    {
        INFO ("device sample rate: " << rate);
        fx.prepare ({ rate, (juce::uint32) blockSize, 2 });
        double phase = 0.0;

        for (const auto position : { 0.5f, 0.4f, 0.0f, 0.25f, 0.5f, 0.6f, 1.0f, 0.75f, 0.5f })
        {
            INFO ("filter position: " << position);
            fx.setParam (0, 0, position);

            for (int block = 0; block < 20; ++block)
            {
                fillSine (buffer, 400.0, phase, 0.1f, rate);
                fx.process (buffer, blockSize);

                bool finite = true;
                float peak = 0.0f;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                    {
                        const auto value = buffer.getSample (ch, i);
                        finite = finite && std::isfinite (value);
                        peak = juce::jmax (peak, std::abs (value));
                    }

                REQUIRE (finite);
                REQUIRE (peak < 2.0f);  // allows resonance, catches runaway feedback
            }
        }

        REQUIRE_THAT (buffer.getRMSLevel (0, 0, blockSize),
                      WithinAbs (0.1f / std::sqrt (2.0f), 0.015f));
    }
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

    // All the way round, with no plugin to stop on.
    for (int i = 0; i < opendj::MasterEffects::numTypes - 2; ++i)
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

//==============================================================================
// Flanger, phaser, gate, bitcrusher and stutter
//==============================================================================

namespace
{
    constexpr std::array<Type, 5> newEffects { Type::flanger, Type::phaser, Type::gate,
                                               Type::bitcrusher, Type::stutter };

    /** The loudest and quietest a tone comes out, block by block, over a span. */
    std::pair<float, float> levelRange (opendj::MasterEffects& fx, double frequency, int blocks, int settleBlocks = 8)
    {
        juce::AudioBuffer<float> buffer (2, blockSize);
        double phase = 0.0;
        auto lowest = 1.0e6f, highest = 0.0f;

        for (int b = 0; b < blocks; ++b)
        {
            fillSine (buffer, frequency, phase, 0.5f);
            fx.process (buffer, blockSize);

            if (b >= settleBlocks)
            {
                const auto level = buffer.getRMSLevel (0, 0, blockSize);
                lowest = juce::jmin (lowest, level);
                highest = juce::jmax (highest, level);
            }
        }

        return { lowest, highest };
    }
}

TEST_CASE ("Each new master effect, turned off, leaves the mix bit for bit alone", "[masterfx]")
{
    for (const auto type : newEffects)
    {
        opendj::MasterEffects fx;
        fx.prepare (specFor());
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

TEST_CASE ("Effects are stepped through in menu order, whatever their numbers", "[masterfx]")
{
    opendj::MasterEffects fx;
    fx.setType (0, Type::filter);

    fx.stepType (0);
    CHECK (fx.getType (0) == Type::flanger);

    // The numbers mapping files use have to stay where they were.
    CHECK ((int) Type::filter == 2);
    CHECK ((int) Type::plugin == 3);
}

TEST_CASE ("A knob's label reads as the length it stands for", "[masterfx]")
{
    using FX = opendj::MasterEffects;

    CHECK (FX::getDivisionText (Type::gate, 0, FX::normalisedForEchoDivision (1)) == "1/4");
    CHECK (FX::getDivisionText (Type::flanger, 0, FX::normalisedForEchoDivision (3)) == "8");
    CHECK (FX::getDivisionText (Type::gate, 1, 0.5f).isEmpty());
    CHECK (FX::getDivisionText (Type::bitcrusher, 0, 0.5f).isEmpty());
}

TEST_CASE ("A flanger sweeps a comb across a steady tone", "[masterfx]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());
    fx.setBeatSeconds (0.5);
    fx.setType (0, Type::flanger);
    fx.setParam (0, 0, 0.0f);    // a sweep every beat, so a short test sees several
    fx.setParam (0, 1, 0.0f);
    fx.setEnabled (0, true);

    // Half a second of sweep at 48 kHz, read in small steps.
    const auto [lowest, highest] = levelRange (fx, 1000.0, 200);

    INFO ("lowest " << lowest << ", highest " << highest);
    CHECK (highest > 0.3f);                 // in phase, at about the level of the tone
    CHECK (lowest < highest * 0.5f);        // and a notch going past it
}

TEST_CASE ("A phaser sweeps a notch across a steady tone", "[masterfx]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());
    fx.setBeatSeconds (0.5);
    fx.setType (0, Type::phaser);
    fx.setParam (0, 0, 0.0f);
    fx.setParam (0, 1, 0.0f);
    fx.setEnabled (0, true);

    const auto [lowest, highest] = levelRange (fx, 1000.0, 200);

    INFO ("lowest " << lowest << ", highest " << highest);
    CHECK (highest > 0.25f);
    CHECK (lowest < highest * 0.5f);
}

TEST_CASE ("Feedback at full cannot make a flanger or a phaser run away", "[masterfx]")
{
    for (const auto type : { Type::flanger, Type::phaser })
    {
        opendj::MasterEffects fx;
        fx.prepare (specFor());
        fx.setBeatSeconds (0.5);
        fx.setType (0, type);
        fx.setParam (0, 0, 0.0f);
        fx.setParam (0, 1, 1.0f);
        fx.setEnabled (0, true);

        juce::AudioBuffer<float> buffer (2, blockSize);
        double phase = 0.0;
        auto loudest = 0.0f;

        // Ten seconds of a tone with the feedback all the way up.
        for (int b = 0; b < 1000; ++b)
        {
            fillSine (buffer, 700.0, phase, 0.5f);
            fx.process (buffer, blockSize);
            loudest = juce::jmax (loudest, buffer.getMagnitude (0, blockSize));
        }

        INFO (opendj::MasterEffects::getTypeName (type) << " loudest " << loudest);
        CHECK (std::isfinite (loudest));
        CHECK (loudest < 2.0f);
    }
}

TEST_CASE ("A gate is silent between its open windows, on the beat", "[masterfx]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());
    fx.setBeatSeconds (0.5);
    fx.setType (0, Type::gate);
    fx.setParam (0, 0, opendj::MasterEffects::normalisedForEchoDivision (2));   // half a beat
    fx.setParam (0, 1, 0.5f);   // open for the first half of that
    fx.setEnabled (0, true);

    // A quarter of a second for each cycle: one and a half thousand samples of
    // a settle at the start of the run, then a window of each kind to read.
    constexpr int cycle = 12000;
    juce::AudioBuffer<float> buffer (2, blockSize);
    double phase = 0.0;
    std::vector<float> output;

    for (int b = 0; b < 2 * cycle / blockSize + 4; ++b)
    {
        fillSine (buffer, 1000.0, phase, 0.5f);
        fx.process (buffer, blockSize);
        const auto* left = buffer.getReadPointer (0);
        output.insert (output.end(), left, left + blockSize);
    }

    const auto rmsAround = [&output] (int centre)
    {
        auto sum = 0.0f;

        for (int i = centre - 500; i < centre + 500; ++i)
            sum += output[(size_t) i] * output[(size_t) i];

        return std::sqrt (sum / 1000.0f);
    };

    for (int c = 0; c < 2; ++c)
    {
        // Past the 20 ms ramp the on switch itself takes.
        CHECK (rmsAround (c * cycle + 3000 + (c == 0 ? 500 : 0)) > 0.3f);
        CHECK (rmsAround (c * cycle + 9000) < 0.001f);
    }
}

TEST_CASE ("A gate follows the music rather than the moment it was pressed", "[masterfx]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());
    fx.setBeatSeconds (0.5);
    fx.setType (0, Type::gate);
    fx.setParam (0, 0, opendj::MasterEffects::normalisedForEchoDivision (2));
    fx.setParam (0, 1, 0.5f);
    fx.setEnabled (0, true);

    settledRms (fx, 1000.0);

    // 0.4 of a half beat cycle in the wrong direction is a jump, not a drift,
    // and 16.45 is 0.45 into its half beat, which is the closed half.
    fx.setBeatPosition (16.45);

    juce::AudioBuffer<float> buffer (2, blockSize);
    double phase = 0.0;
    fillSine (buffer, 1000.0, phase, 0.5f);
    fx.process (buffer, blockSize);

    CHECK_THAT (fx.getBeatClock(), WithinAbs (16.45 + blockSize / (0.5 * sampleRate), 1.0e-6));
    CHECK (buffer.getRMSLevel (0, blockSize / 2, blockSize / 2) < 0.01f);
}

TEST_CASE ("The beat clock eases out a small difference and jumps a large one", "[masterfx]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());
    fx.setBeatSeconds (0.5);

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();
    const auto oneBlock = blockSize / (0.5 * sampleRate);

    fx.setBeatPosition (10.0);
    fx.process (buffer, blockSize);
    CHECK_THAT (fx.getBeatClock(), WithinAbs (10.0 + oneBlock, 1.0e-6));

    // A tenth of a beat off moves by a quarter of that.
    fx.setBeatPosition (10.0 + oneBlock + 0.1);
    fx.process (buffer, blockSize);
    CHECK_THAT (fx.getBeatClock(), WithinAbs (10.0 + 2.0 * oneBlock + 0.025, 1.0e-6));

    // Across the wrap goes the short way round.
    fx.setBeatPosition (63.0);
    fx.process (buffer, blockSize);
    fx.setBeatPosition (0.0);
    fx.process (buffer, blockSize);
    CHECK_THAT (fx.getBeatClock(), WithinAbs (oneBlock, 1.0e-6));
}

TEST_CASE ("A bitcrusher leaves only as many levels as it has bits for", "[masterfx]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());
    fx.setType (0, Type::bitcrusher);
    fx.setParam (0, 0, 1.0f);    // two bits
    fx.setParam (0, 1, 0.0f);    // no held samples
    fx.setEnabled (0, true);

    settledRms (fx, 440.0);

    juce::AudioBuffer<float> buffer (2, blockSize);
    double phase = 0.0;
    fillSine (buffer, 440.0, phase, 0.9f);
    fx.process (buffer, blockSize);

    for (int i = 0; i < blockSize; ++i)
    {
        const auto doubled = buffer.getSample (0, i) * 2.0f;
        REQUIRE_THAT (doubled, WithinAbs (std::round (doubled), 1.0e-5f));
    }
}

TEST_CASE ("A bitcrusher's hold repeats samples, which folds the top down", "[masterfx]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());
    fx.setType (0, Type::bitcrusher);
    fx.setParam (0, 0, 0.0f);    // sixteen bits, so the hold is all that is heard
    fx.setParam (0, 1, 1.0f);    // forty samples at a time
    fx.setEnabled (0, true);

    settledRms (fx, 440.0);

    juce::AudioBuffer<float> buffer (2, blockSize);
    double phase = 0.0;
    fillSine (buffer, 440.0, phase, 0.5f);
    fx.process (buffer, blockSize);

    auto unchanged = 0;

    for (int i = 1; i < blockSize; ++i)
        if (std::abs (buffer.getSample (0, i) - buffer.getSample (0, i - 1)) < 1.0e-5f)
            ++unchanged;

    // Thirty nine out of every forty repeat the sample before.
    CHECK (unchanged > (int) (blockSize * 0.9));
}

TEST_CASE ("A stutter repeats what has just played until it is let go", "[masterfx]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());
    fx.setBeatSeconds (0.5);
    fx.setType (0, Type::stutter);
    fx.setParam (0, 0, opendj::MasterEffects::normalisedForEchoDivision (2));   // half a beat: 12000 samples
    fx.setParam (0, 1, 0.0f);

    constexpr int slice = 12000;

    // A second of a tone that it can hold on to.
    settledRms (fx, 500.0);

    juce::AudioBuffer<float> tone (2, blockSize);
    double phase = 0.0;

    for (int b = 0; b < 100; ++b)
    {
        fillSine (tone, 500.0, phase, 0.5f);
        fx.process (tone, blockSize);
    }

    fx.setEnabled (0, true);

    // Silence goes in from here on, so anything that comes out is the repeat.
    juce::AudioBuffer<float> silence (2, blockSize);
    std::vector<float> output;

    for (int b = 0; b < 3 * slice / blockSize; ++b)
    {
        silence.clear();
        fx.process (silence, blockSize);
        const auto* left = silence.getReadPointer (0);
        output.insert (output.end(), left, left + blockSize);
    }

    // Loud throughout, and the same one slice later.
    const auto at = [&output] (int i) { return output[(size_t) i]; };
    auto largestDifference = 0.0f, loudest = 0.0f;

    for (int i = slice / 2; i < slice / 2 + 2000; ++i)
    {
        largestDifference = juce::jmax (largestDifference, std::abs (at (i) - at (i + slice)));
        loudest = juce::jmax (loudest, std::abs (at (i)));
    }

    CHECK (loudest > 0.4f);
    CHECK (largestDifference < 1.0e-4f);

    // Let go, and it is the input that is heard again.
    fx.setEnabled (0, false);

    for (int b = 0; b < 8; ++b)
    {
        silence.clear();
        fx.process (silence, blockSize);
    }

    CHECK (silence.getMagnitude (0, 0, blockSize) < 1.0e-5f);

    // Nor does anything of the old slice come back with the next press sooner
    // than it should: the input has been recording again, and it was silence.
    for (int b = 0; b < 3 * slice / blockSize; ++b)
    {
        silence.clear();
        fx.process (silence, blockSize);
    }

    fx.setEnabled (0, true);
    silence.clear();
    fx.process (silence, blockSize);
    fx.process (silence, blockSize);
    CHECK (silence.getMagnitude (0, 0, blockSize) < 1.0e-5f);
}

TEST_CASE ("A stutter's decay fades each repeat by half at full", "[masterfx]")
{
    opendj::MasterEffects fx;
    fx.prepare (specFor());
    fx.setBeatSeconds (0.5);
    fx.setType (0, Type::stutter);
    fx.setParam (0, 0, opendj::MasterEffects::normalisedForEchoDivision (1));   // a quarter of a beat: 6000 samples
    fx.setParam (0, 1, 1.0f);

    settledRms (fx, 500.0);
    fx.setEnabled (0, true);

    constexpr int slice = 6000;
    juce::AudioBuffer<float> silence (2, blockSize);
    std::vector<float> output;

    for (int b = 0; b < 3 * slice / blockSize + 1; ++b)
    {
        silence.clear();
        fx.process (silence, blockSize);
        const auto* left = silence.getReadPointer (0);
        output.insert (output.end(), left, left + blockSize);
    }

    const auto peakOf = [&output] (int repeat)
    {
        auto peak = 0.0f;

        for (int i = repeat * slice + 2000; i < repeat * slice + 4000; ++i)
            peak = juce::jmax (peak, std::abs (output[(size_t) i]));

        return peak;
    };

    CHECK_THAT (peakOf (1) / peakOf (0), WithinAbs (0.5f, 0.01f));
    CHECK_THAT (peakOf (2) / peakOf (1), WithinAbs (0.5f, 0.01f));
}

TEST_CASE ("A new effect taken over mid-signal does not click", "[masterfx]")
{
    for (const auto type : newEffects)
    {
        opendj::MasterEffects fx;
        fx.prepare (specFor());
        fx.setBeatSeconds (0.5);
        fx.setType (0, Type::filter);
        fx.setEnabled (0, true);

        juce::AudioBuffer<float> buffer (2, blockSize);
        double phase = 0.0;
        auto previous = 0.0f, biggestStep = 0.0f;

        const auto sineStep = 0.5f * (float) (juce::MathConstants<double>::twoPi * 200.0 / sampleRate);

        // A gate is allowed its own edges, which are slower than a click, and
        // the stutter its slice ends. It is the handover being measured.
        for (int b = 0; b < 30; ++b)
        {
            if (b == 15)
                fx.setType (0, type);

            fillSine (buffer, 200.0, phase, 0.5f);
            fx.process (buffer, blockSize);

            for (int i = 0; i < blockSize; ++i)
            {
                if (b >= 2 && b <= 16)
                    biggestStep = juce::jmax (biggestStep, std::abs (buffer.getSample (0, i) - previous));

                previous = buffer.getSample (0, i);
            }
        }

        INFO (opendj::MasterEffects::getTypeName (type) << " biggest step " << biggestStep);
        CHECK (biggestStep < sineStep * 6.0f);
    }
}
