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
constexpr float filterOpenLow = 22000.0f;   // a low pass this high is transparent
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

/** Cutoff for the low pass half of the knob: transparent from the centre up. */
inline float lowPassCutoffFor (float position)
{
    if (position >= 0.5f - filterDeadZone)
        return filterOpenLow;

    const auto amount = juce::jlimit (0.0f, 1.0f, (0.5f - filterDeadZone - position) / (0.5f - filterDeadZone));
    return filterOpenLow * std::pow (filterLowestCutoff / filterOpenLow, amount);
}

/** And the high pass half: transparent from the centre down. */
inline float highPassCutoffFor (float position)
{
    if (position <= 0.5f + filterDeadZone)
        return filterOpenHigh;

    const auto amount = juce::jlimit (0.0f, 1.0f, (position - 0.5f - filterDeadZone) / (0.5f - filterDeadZone));
    return filterOpenHigh * std::pow (filterHighestCutoff / filterOpenHigh, amount);
}

} // namespace opendj::effectCurves
