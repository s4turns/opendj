/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "analysis/KeyDetector.h"

#include <cmath>

using Catch::Matchers::WithinAbs;
using opendj::KeyDetector;
using opendj::MusicalKey;

namespace
{
    constexpr double sampleRate = 44100.0;

    double midiToHz (int midi)
    {
        return 440.0 * std::pow (2.0, (midi - 69) / 12.0);
    }

    /** Adds a note with a few harmonics, which is what makes this a test of the
        detector rather than of a sine wave: real instruments put energy on the
        fifth and the octave, and those have to not drag the answer sideways. */
    void addNote (juce::AudioBuffer<float>& buffer, int midi, int start, int length, float amplitude)
    {
        const auto fundamental = midiToHz (midi);
        const auto numSamples = buffer.getNumSamples();

        for (int harmonic = 1; harmonic <= 4; ++harmonic)
        {
            const auto frequency = fundamental * harmonic;

            if (frequency > sampleRate * 0.45)
                break;

            const auto gain = amplitude / static_cast<float> (harmonic);

            for (int i = 0; i < length; ++i)
            {
                const auto index = start + i;

                if (index >= numSamples)
                    break;

                // Fade each end so the chord changes do not click, which would
                // spread broadband energy across every pitch class.
                const auto rampSamples = static_cast<float> (sampleRate * 0.02);
                const auto edge = static_cast<float> (juce::jmin (i, length - i));
                const auto fade = juce::jmin (1.0f, edge / rampSamples);
                const auto value = std::sin (2.0 * juce::MathConstants<double>::pi * frequency * i / sampleRate);

                buffer.addSample (0, index, static_cast<float> (value) * gain * fade);
            }
        }
    }

    /** A chord progression, each chord a triad given as MIDI notes. */
    juce::AudioBuffer<float> renderProgression (const std::vector<std::array<int, 3>>& chords,
                                                double secondsPerChord = 2.0,
                                                int repeats = 4)
    {
        const auto chordSamples = static_cast<int> (secondsPerChord * sampleRate);
        const auto total = chordSamples * static_cast<int> (chords.size()) * repeats;

        juce::AudioBuffer<float> buffer (2, total);
        buffer.clear();

        auto position = 0;

        for (int repeat = 0; repeat < repeats; ++repeat)
        {
            for (const auto& chord : chords)
            {
                for (const auto note : chord)
                    addNote (buffer, note, position, chordSamples, 0.20f);

                position += chordSamples;
            }
        }

        buffer.copyFrom (1, 0, buffer, 0, 0, total);
        return buffer;
    }

    // MIDI 60 is middle C.
    constexpr int C4 = 60, D4 = 62, E4 = 64, F4 = 65, G4 = 67, A4 = 69, B4 = 71;
    constexpr int Gs4 = 68;

    std::vector<std::array<int, 3>> transpose (std::vector<std::array<int, 3>> chords, int semitones)
    {
        for (auto& chord : chords)
            for (auto& note : chord)
                note += semitones;

        return chords;
    }

    /** Am - Dm - E - Am.

        Deliberately not the Am - F - C - G that every dance record uses, because
        that progression contains only the notes of C major and is genuinely
        ambiguous between A minor and its relative. The E major chord here has a
        G# in it, the leading tone, and that one note is what makes the key A
        minor rather than C major. A detector that cannot use it is not doing the
        job, and a test built on the ambiguous progression would be asserting
        something untrue about music. */
    const std::vector<std::array<int, 3>> aMinorProgression
    {
        { A4, C4 + 12, E4 + 12 },     // Am
        { D4, F4, A4 },               // Dm
        { E4, Gs4, B4 },              // E
        { A4, C4 + 12, E4 + 12 }      // Am
    };

    const std::vector<std::array<int, 3>> cMajorProgression
    {
        { C4, E4, G4 },               // C
        { F4, A4, C4 + 12 },          // F
        { G4, B4, D4 + 12 },          // G
        { C4, E4, G4 }                // C
    };
}

//==============================================================================
// Naming
//==============================================================================

TEST_CASE ("a key writes itself the way DJ software writes it", "[key][naming]")
{
    MusicalKey aMinor { 9, MusicalKey::Mode::minor, 1.0f };
    MusicalKey cMajor { 0, MusicalKey::Mode::major, 1.0f };
    MusicalKey fSharpMinor { 6, MusicalKey::Mode::minor, 1.0f };

    REQUIRE (aMinor.toString() == "Am");
    REQUIRE (cMajor.toString() == "C");
    REQUIRE (fSharpMinor.toString() == "F#m");

    REQUIRE (MusicalKey().toString().isEmpty());
}

TEST_CASE ("the Camelot wheel matches the published table", "[key][camelot]")
{
    // Spot checks against the wheel every DJ has seen on a poster.
    const std::pair<MusicalKey, const char*> expected[]
    {
        { { 9,  MusicalKey::Mode::minor, 1.0f }, "8A"  },   // A minor
        { { 0,  MusicalKey::Mode::major, 1.0f }, "8B"  },   // C major
        { { 4,  MusicalKey::Mode::minor, 1.0f }, "9A"  },   // E minor
        { { 7,  MusicalKey::Mode::major, 1.0f }, "9B"  },   // G major
        { { 8,  MusicalKey::Mode::minor, 1.0f }, "1A"  },   // G# minor
        { { 11, MusicalKey::Mode::major, 1.0f }, "1B"  },   // B major
        { { 5,  MusicalKey::Mode::minor, 1.0f }, "4A"  },   // F minor
        { { 5,  MusicalKey::Mode::major, 1.0f }, "7B"  }    // F major
    };

    for (const auto& [key, camelot] : expected)
    {
        INFO (key.toString() << " should be " << camelot);
        REQUIRE (key.toCamelot() == camelot);
    }
}

TEST_CASE ("a key and its relative share a Camelot number", "[key][camelot]")
{
    // This is the whole point of the wheel: 8A and 8B mix, because they are the
    // same seven notes.
    for (int tonic = 0; tonic < 12; ++tonic)
    {
        const MusicalKey major { tonic, MusicalKey::Mode::major, 1.0f };
        const MusicalKey relativeMinor { (tonic + 9) % 12, MusicalKey::Mode::minor, 1.0f };

        INFO (major.toString() << " and " << relativeMinor.toString());
        REQUIRE (major.toCamelot().dropLastCharacters (1)
                 == relativeMinor.toCamelot().dropLastCharacters (1));
    }
}

TEST_CASE ("every key survives a trip through Camelot and back", "[key][naming]")
{
    for (int tonic = 0; tonic < 12; ++tonic)
    {
        for (const auto mode : { MusicalKey::Mode::major, MusicalKey::Mode::minor })
        {
            const MusicalKey original { tonic, mode, 1.0f };

            const auto viaName = MusicalKey::fromString (original.toString());
            const auto viaCamelot = MusicalKey::fromString (original.toCamelot());

            INFO (original.toString() << " / " << original.toCamelot());
            REQUIRE (viaName.tonic == tonic);
            REQUIRE (viaName.mode == mode);
            REQUIRE (viaCamelot.tonic == tonic);
            REQUIRE (viaCamelot.mode == mode);
        }
    }
}

TEST_CASE ("keys written with flats are understood", "[key][naming]")
{
    // Tags in the wild are full of these, and they have to land on the same
    // pitch class as the sharp spelling or a library cannot be matched up.
    REQUIRE (MusicalKey::fromString ("Bbm").tonic == 10);
    REQUIRE (MusicalKey::fromString ("Bbm").mode == MusicalKey::Mode::minor);
    REQUIRE (MusicalKey::fromString ("Ab").tonic == 8);
    REQUIRE (MusicalKey::fromString ("Ab").mode == MusicalKey::Mode::major);
    REQUIRE (MusicalKey::fromString ("Amin").tonic == 9);
    REQUIRE (MusicalKey::fromString ("Amin").mode == MusicalKey::Mode::minor);
}

TEST_CASE ("nonsense in the key tag is rejected rather than guessed at", "[key][naming]")
{
    for (const char* text : { "", "  ", "H", "banana", "13A", "0A", "Zm" })
    {
        INFO ("input: '" << text << "'");
        REQUIRE_FALSE (MusicalKey::fromString (text).isValid());
    }
}

//==============================================================================
// Chromagram
//==============================================================================

TEST_CASE ("a single note lands on its own pitch class", "[key][chroma]")
{
    juce::AudioBuffer<float> buffer (2, static_cast<int> (sampleRate * 10.0));
    buffer.clear();
    addNote (buffer, A4, 0, buffer.getNumSamples(), 0.5f);
    buffer.copyFrom (1, 0, buffer, 0, 0, buffer.getNumSamples());

    const auto chroma = KeyDetector::chromagram (buffer, sampleRate);
    const auto loudest = std::distance (chroma.begin(),
                                        std::max_element (chroma.begin(), chroma.end()));

    REQUIRE (loudest == 9);   // A
}

TEST_CASE ("silence produces an empty chromagram and no key", "[key][chroma]")
{
    juce::AudioBuffer<float> buffer (2, static_cast<int> (sampleRate * 5.0));
    buffer.clear();

    const auto chroma = KeyDetector::chromagram (buffer, sampleRate);

    REQUIRE (std::accumulate (chroma.begin(), chroma.end(), 0.0f) == 0.0f);
    REQUIRE_FALSE (KeyDetector::detect (buffer, sampleRate).isValid());
}

TEST_CASE ("audio shorter than one frame is handled rather than read past", "[key][chroma]")
{
    juce::AudioBuffer<float> buffer (2, 512);
    buffer.clear();

    REQUIRE_FALSE (KeyDetector::detect (buffer, sampleRate).isValid());
}

//==============================================================================
// Detection
//==============================================================================

TEST_CASE ("a minor progression is detected as that minor key", "[key][detect]")
{
    const auto key = KeyDetector::detect (renderProgression (aMinorProgression), sampleRate);

    INFO ("detected " << key.toString() << " (" << key.toCamelot() << ")");
    REQUIRE (key.isValid());
    REQUIRE (key.tonic == 9);
    REQUIRE (key.mode == MusicalKey::Mode::minor);
}

TEST_CASE ("a major progression is detected as that major key", "[key][detect]")
{
    const auto key = KeyDetector::detect (renderProgression (cMajorProgression), sampleRate);

    INFO ("detected " << key.toString() << " (" << key.toCamelot() << ")");
    REQUIRE (key.isValid());
    REQUIRE (key.tonic == 0);
    REQUIRE (key.mode == MusicalKey::Mode::major);
}

TEST_CASE ("transposing the music transposes the answer", "[key][detect]")
{
    // The same music moved up must come back moved up. This is the check that
    // catches a detector which has quietly learnt to answer "A minor" to
    // everything, and it runs over all twelve so no one lucky rotation passes
    // for the rest.
    for (int semitones = 1; semitones < 12; ++semitones)
    {
        const auto audio = renderProgression (transpose (aMinorProgression, semitones), 2.0, 2);
        const auto key = KeyDetector::detect (audio, sampleRate);
        const auto expected = (9 + semitones) % 12;

        INFO ("up " << semitones << " semitones: detected " << key.toString());
        REQUIRE (key.isValid());
        REQUIRE (key.tonic == expected);
        REQUIRE (key.mode == MusicalKey::Mode::minor);
    }
}

TEST_CASE ("confidence is higher for a clear key than for a chromatic wash", "[key][detect]")
{
    const auto tonal = KeyDetector::detect (renderProgression (aMinorProgression), sampleRate);

    // Every semitone at once belongs to no key at all.
    juce::AudioBuffer<float> wash (2, static_cast<int> (sampleRate * 20.0));
    wash.clear();

    for (int note = C4; note < C4 + 12; ++note)
        addNote (wash, note, 0, wash.getNumSamples(), 0.15f);

    wash.copyFrom (1, 0, wash, 0, 0, wash.getNumSamples());

    const auto washKey = KeyDetector::detect (wash, sampleRate);

    INFO ("tonal " << tonal.confidence << " vs wash " << washKey.confidence);
    REQUIRE (tonal.confidence > washKey.confidence);
}
