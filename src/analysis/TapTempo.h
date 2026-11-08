/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <array>
#include <cstddef>

namespace opendj
{

/** A tempo from a finger tapping along: the average gap between the last few
    taps. A pause longer than two seconds starts over, so a stray tap from
    minutes ago does not drag the answer down.

    Pure arithmetic on the times it is given, so it can be tested without a
    clock, and used from whichever thread the tap arrived on as long as only
    one thread taps a given instance.
*/
class TapTempo final
{
public:
    /** Taps needed before there is an answer: three gaps. */
    static constexpr int minimumTaps = 4;

    /** The most taps averaged, so a tempo that drifts can be followed. */
    static constexpr int maximumTaps = 8;

    /** A gap longer than this is a new start rather than a very slow tempo. */
    static constexpr double resetSeconds = 2.0;

    /** Records a tap at `seconds` on any steady clock. Returns the tempo in
        beats a minute, or 0 while there are too few taps to tell. */
    double tap (double seconds) noexcept
    {
        if (count > 0 && (seconds - times[newest()] > resetSeconds || seconds <= times[newest()]))
            count = 0;

        start = count < maximumTaps ? start : (start + 1) % maximumTaps;
        count = count < maximumTaps ? count + 1 : count;
        times[newest()] = seconds;

        return getBpm();
    }

    /** The tempo from the taps so far, or 0 with fewer than minimumTaps. */
    double getBpm() const noexcept
    {
        if (count < minimumTaps)
            return 0.0;

        const auto span = times[newest()] - times[start];
        return span > 0.0 ? 60.0 * (count - 1) / span : 0.0;
    }

    int getTapCount() const noexcept { return count; }

    void reset() noexcept { count = 0; start = 0; }

private:
    std::size_t newest() const noexcept { return (start + (std::size_t) count - 1) % maximumTaps; }

    std::array<double, maximumTaps> times {};
    std::size_t start = 0;
    int count = 0;
};

} // namespace opendj
