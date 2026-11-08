/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

namespace opendj::effectCurves
{

/** How knob positions become filter cutoffs and echo feedback, shared by the
    channel strips and the master effects so that a filter knob at the same
    place sounds the same on either. */

constexpr double smoothingSeconds = 0.02;

// The dead zone either side of centre, so that a knob resting a hair off
// the middle is still audibly out of the way.
constexpr float filterDeadZone = 0.02f;

constexpr float filterLowestCutoff = 120.0f;
constexpr float filterHighestCutoff = 8000.0f;
constexpr float filterOpenLow = 22000.0f;   // open at music playback sample rates
constexpr float filterOpenHigh = 15.0f;     // and a high pass this low likewise

// Long enough for two beats at 60 BPM, which is slower than anything anyone
// will echo. The line is sized once in prepare and never again.
constexpr double maxEchoSeconds = 4.0;

constexpr double shortestEchoSeconds = 0.02;

/** The knob turned up raises the wet level and the feedback together. Kept
    under one so the echo always dies away: a DJ mixer that could be left
    self-oscillating is a mixer that will be. */
inline float echoFeedbackFor (float amount) noexcept
{
    return juce::jlimit (0.0f, 0.85f, amount * 0.85f);
}

/** A cutoff must stay below Nyquist, with a little room before the tangent's
    pole. Bluetooth voice profiles can run at 8 or 16 kHz, so the usual 22 kHz
    open cutoff would otherwise make a sweep unstable and poison the audio. */
inline float limitFilterCutoff (float cutoff, double sampleRate) noexcept
{
    return juce::jmin (cutoff, static_cast<float> (sampleRate * 0.49));
}

/** Cutoff for the low pass half of the knob: transparent from the centre up.
    Scale the whole sweep to the device's usable range, avoiding a plateau
    that simply clamping a fixed 22 kHz curve would leave near the centre. */
inline float lowPassCutoffFor (float position, double sampleRate)
{
    const auto open = limitFilterCutoff (filterOpenLow, sampleRate);

    if (position >= 0.5f - filterDeadZone)
        return open;

    const auto amount = juce::jlimit (0.0f, 1.0f, (0.5f - filterDeadZone - position) / (0.5f - filterDeadZone));
    return open * std::pow (limitFilterCutoff (filterLowestCutoff, sampleRate) / open, amount);
}

/** And the high pass half: transparent from the centre down. */
inline float highPassCutoffFor (float position, double sampleRate)
{
    const auto open = limitFilterCutoff (filterOpenHigh, sampleRate);

    if (position <= 0.5f + filterDeadZone)
        return open;

    const auto amount = juce::jlimit (0.0f, 1.0f, (position - 0.5f - filterDeadZone) / (0.5f - filterDeadZone));
    return open * std::pow (limitFilterCutoff (filterHighestCutoff, sampleRate) / open, amount);
}

} // namespace opendj::effectCurves
