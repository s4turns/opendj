/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_graphics/juce_graphics.h>

namespace opendj
{

/** One colour per hot cue slot, shared by the pads and the waveform markers so
    a pad and its marker can be matched at a glance. Spaced round the colour
    wheel and bright enough to read on the dark panels. */
inline juce::Colour hotCueColour (int slot) noexcept
{
    static const juce::Colour colours[]
    {
        juce::Colour (0xffe5484d),   // red
        juce::Colour (0xffff8b3e),   // orange
        juce::Colour (0xfff5d90a),   // yellow
        juce::Colour (0xff46a758),   // green
        juce::Colour (0xff12a594),   // teal
        juce::Colour (0xff3e63dd),   // blue
        juce::Colour (0xff8e4ec6),   // purple
        juce::Colour (0xffd6409f)    // pink
    };

    constexpr auto count = (int) (sizeof (colours) / sizeof (colours[0]));
    return colours[((slot % count) + count) % count];
}

} // namespace opendj
