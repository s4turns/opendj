/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_core/juce_core.h>

#include "core/Mixer.h"
#include "core/OutputRouter.h"
#include "core/Sampler.h"
#include "stream/IcecastConnection.h"

#include <array>

namespace opendj
{

/** Everything worth finding the way it was left.

    Kept as plain data with no reference to a running engine, for two reasons.
    It can be read and written without a sound card, which is what lets it be
    tested at all. And the rules about what is restored live in one place rather
    than being spread across whatever happened to call a setter at startup.

    Deliberately not stored: deck contents, deck positions, and the channel
    faders. A DJ application that reopens playing where it crashed, or with a
    fader somewhere the user cannot see, is worse than one that starts quiet.
*/
struct SessionState
{
    float masterGain = 0.8f;
    float cueGain = 0.7f;
    float cueMix = 0.0f;
    Mixer::CrossfaderCurve crossfaderCurve = Mixer::CrossfaderCurve::constantPower;

    std::array<Mixer::CrossfaderAssign, Mixer::numChannels> crossfaderAssigns
    {
        Mixer::CrossfaderAssign::a,
        Mixer::CrossfaderAssign::b,
        Mixer::CrossfaderAssign::thru,
        Mixer::CrossfaderAssign::thru
    };

    /** How far each tempo fader travels, as a percentage either side of zero. */
    std::array<double, Mixer::numChannels> tempoRanges { 8.0, 8.0, 8.0, 8.0 };

    /** Which deck is on screen on each side. */
    std::array<int, 2> visibleDecks { 0, 1 };

    /** Where the master and cue busses go. Separate pairs by default: a split
        turns the master mono and puts it in one speaker, which nobody should
        get without having asked for it. */
    OutputMode outputMode = OutputMode::separatePairs;

    float samplerGain = 0.8f;
    bool samplerCue = false;

    /** The file behind each pad, so a set of sounds survives a restart. Empty
        means the pad was empty, or its file has since moved. */
    std::array<juce::String, Sampler::numSlots> samplerFiles {};
    std::array<bool, Sampler::numSlots> samplerLooping {};
    std::array<float, Sampler::numSlots> samplerGains { 1.0f, 1.0f, 1.0f, 1.0f,
                                                        1.0f, 1.0f, 1.0f, 1.0f };

    /** The broadcast server, password included. Kept in clear text, which the
        dialog says out loud: it is what every DJ application does, and telling
        somebody is better than quietly deciding for them. */
    BroadcastSettings broadcast;

    /** The window, so it opens where it was left rather than in the middle of
        whichever monitor the system picks. Empty until one has been saved. */
    juce::String windowBounds;

    //==========================================================================

    /** Reads the mixer and the sampler. Does not touch the file. The output
        mode belongs to the engine rather than either of them, so it is set by
        whoever owns this. */
    void captureFrom (const Mixer& mixer, const Sampler& sampler);

    /** Writes this state onto a mixer and a sampler. Sampler slots are named
        rather than loaded here, since loading decodes a file and this must stay
        usable from a test with no audio files in reach. */
    void applyTo (Mixer& mixer, Sampler& sampler) const;

    /** Round trips through JSON. Anything missing keeps its default, so a file
        written by an older build still loads and only the new settings start
        fresh. */
    juce::var toVar() const;
    static SessionState fromVar (const juce::var& source);

    /** Where the settings live: beside the library, in `~/.config/OpenDJ` on
        Linux and the equivalent on Windows. */
    static juce::File defaultFile();

    /** True on success. A failure here is worth reporting but never worth
        stopping for: a session that cannot save its settings still mixes. */
    bool writeTo (const juce::File& file) const;
    static SessionState readFrom (const juce::File& file);
};

} // namespace opendj
