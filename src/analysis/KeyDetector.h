/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>

namespace opendj
{

/** A musical key, as a tonic and a mode.

    Written the way DJ software writes it rather than the way a score does:
    sharps throughout, so F# never appears as Gb, because two spellings of one
    key make a library impossible to sort or match against.
*/
struct MusicalKey
{
    enum class Mode { major, minor };

    int tonic = -1;                  ///< 0 is C through 11 is B; -1 means unknown
    Mode mode = Mode::major;
    float confidence = 0.0f;         ///< 0 to 1, how far clear of the runner-up

    bool isValid() const noexcept { return tonic >= 0 && tonic < 12; }

    /** "Am", "F#", "C". Empty when unknown. */
    juce::String toString() const;

    /** Camelot wheel notation: "8A" for A minor, "8B" for C major. This is what
        harmonic mixing runs on, because neighbouring numbers are the keys that
        mix without clashing. Empty when unknown. */
    juce::String toCamelot() const;

    /** Parses either spelling back, so a key already written into a file's tags
        can be compared with a detected one. Returns an invalid key on nonsense. */
    static MusicalKey fromString (const juce::String& text);
};

/** Finds the key of a track.

    The method is the standard one: fold the spectrum down to twelve pitch
    classes, then see which of the twenty-four major and minor profiles that
    histogram looks most like. It is not a transcription and does not pretend to
    be; on the four-to-the-floor material this is aimed at, where the bass line
    carries the tonality, it is right often enough to be worth sorting by.

    Slow and blocking, so it belongs on the same background thread as the rest
    of the analysis.
*/
class KeyDetector
{
public:
    struct Options
    {
        /** How much audio to look at, taken from the middle of the track.

            The intro and the outro of a club record are frequently just drums,
            which say nothing about key and drag the histogram towards noise.
            The middle is where the music is, and two minutes of it is plenty:
            reading the whole of a ten minute track costs five times as much and
            changes almost nothing. */
        double secondsToExamine = 120.0;

        /** Ignore everything below this. Bins below about C3 are closer together
            than the resolution of the transform, so they smear across pitch
            classes instead of landing in one. */
        double lowestHz = 130.0;

        /** And above this, where harmonics and cymbals outnumber notes. */
        double highestHz = 2100.0;
    };

    /** The twelve pitch class weights, C first, normalised to sum to one.
        Empty audio gives all zeros. */
    static std::array<float, 12> chromagram (const juce::AudioBuffer<float>& audio,
                                             double sampleRate,
                                             Options options);

    static std::array<float, 12> chromagram (const juce::AudioBuffer<float>& audio,
                                             double sampleRate)
    {
        return chromagram (audio, sampleRate, Options());
    }

    /** Matches a chromagram against the twenty-four key profiles. */
    static MusicalKey fromChromagram (const std::array<float, 12>& chroma);

    /** Chromagram and match in one call. Returns an invalid key for silence. */
    static MusicalKey detect (const juce::AudioBuffer<float>& audio,
                              double sampleRate,
                              Options options);

    static MusicalKey detect (const juce::AudioBuffer<float>& audio, double sampleRate)
    {
        return detect (audio, sampleRate, Options());
    }
};

} // namespace opendj
