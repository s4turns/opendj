/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace opendj::loudness
{

/** How loud a whole track sounds, as ITU-R BS.1770 and EBU R128 define it.

    Not its RMS, which a sub-heavy track wins and a bright one loses, and not its
    peak, which says nothing at all about a limited master. The signal is shaped
    to roughly what an ear hears, measured in overlapping blocks of 400 ms, and
    silence and the quiet parts are gated out, so a long breakdown does not make
    a track read as quieter than the part anybody is dancing to.

    In LUFS, always below zero. Zero means there was nothing to measure: a track
    too short for one block, or nothing but silence. */
double measureLufs (const juce::AudioBuffer<float>& audio, double sampleRate);

/** What everything is levelled to. About what streaming services play at, which
    leaves room on the master: a hot club master comes down and an older one comes
    up, and neither runs into the soft clip for being levelled. */
inline constexpr double targetLufs = -14.0;

inline constexpr float quietestGain = 0.25f;    ///< about -12 dB
inline constexpr float loudestGain = 4.0f;      ///< about +12 dB

/** The gain that takes a track to the target, within the limits above. Never
    lifts a track past its own peak, since distortion is a worse fault than being
    a little quiet. 1 when the loudness is not known. */
float gainForLoudness (double lufs, float peak) noexcept;

} // namespace opendj::loudness
