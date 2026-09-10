/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

namespace opendj
{

/** Angle helpers for the platters.

    Header only and free of any component, because the one thing that reliably
    goes wrong when you drag something in a circle is the wrap from 359 degrees
    back to 0, and that deserves to be tested on its own rather than only by eye.
*/
namespace angles
{
    inline constexpr double twoPi = juce::MathConstants<double>::twoPi;

    /** Folds any angle into the range 0 up to but not including two pi. */
    inline double wrap (double radians) noexcept
    {
        radians = std::fmod (radians, twoPi);
        return radians < 0.0 ? radians + twoPi : radians;
    }

    /** The shortest way round from one angle to another, signed. Positive is
        anticlockwise in maths terms, which is clockwise on screen because the
        y axis points down. The result is always within half a turn, so dragging
        across the twelve o'clock mark reads as a small step, not a whole turn
        backwards. */
    inline double shortestDelta (double from, double to) noexcept
    {
        auto delta = wrap (to) - wrap (from);

        if (delta > juce::MathConstants<double>::pi)
            delta -= twoPi;
        else if (delta < -juce::MathConstants<double>::pi)
            delta += twoPi;

        return delta;
    }

    /** Converts an angular movement into the tick count a controller would have
        sent for the same movement, so a mouse drag and a real platter arrive at
        the deck through exactly the same path. */
    inline double toTicks (double deltaRadians, int ticksPerRevolution) noexcept
    {
        return deltaRadians / twoPi * ticksPerRevolution;
    }

    /** Where a platter should be pointing, given a position in the track. A
        record turns once every 1.8 seconds at 33 1/3 rpm. */
    inline double platterAngle (double positionSeconds, double revolutionSeconds = 1.8) noexcept
    {
        if (revolutionSeconds <= 0.0)
            return 0.0;

        return wrap (positionSeconds / revolutionSeconds * twoPi);
    }

    /** The angle of a point relative to a centre, measured clockwise from
        twelve o'clock, which is how a platter marker reads.

        Plain numbers rather than a Point, so this header stays free of the
        graphics module and can be tested without one. */
    inline double angleFromCentre (double x, double y, double centreX, double centreY) noexcept
    {
        return wrap (std::atan2 (x - centreX, centreY - y));
    }
}

} // namespace opendj
