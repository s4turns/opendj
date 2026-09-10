/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/WaveformComponent.h"

#include <cmath>

namespace opendj
{

namespace
{
    const juce::Colour backgroundColour { 0xff0e0e12 };
    const juce::Colour waveColour       { 0xff35c2f0 };
    const juce::Colour playedColour     { 0xff1d6a86 };
    const juce::Colour cueColour        { 0xffe8a33d };
    const juce::Colour beatColour       { 0x40ffffff };
    const juce::Colour barColour        { 0x90ffffff };
}

WaveformComponent::WaveformComponent (Mode modeToUse)
    : mode (modeToUse)
{
    setOpaque (true);
    setInterceptsMouseClicks (mode == Mode::overview, false);
}

void WaveformComponent::setAnalysis (std::shared_ptr<const TrackAnalysis> newAnalysis)
{
    if (analysis == newAnalysis)
        return;

    analysis = std::move (newAnalysis);
    repaint();
}

void WaveformComponent::setPosition (double seconds, double lengthSeconds)
{
    if (juce::approximatelyEqual (seconds, positionSeconds)
        && juce::approximatelyEqual (lengthSeconds, trackLengthSeconds))
        return;

    positionSeconds = seconds;
    trackLengthSeconds = lengthSeconds;
    repaint();
}

void WaveformComponent::setCuePoint (double seconds)
{
    if (juce::approximatelyEqual (seconds, cueSeconds))
        return;

    cueSeconds = seconds;
    repaint();
}

void WaveformComponent::setWindowSeconds (double seconds)
{
    windowSeconds = juce::jmax (0.25, seconds);
    repaint();
}

void WaveformComponent::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);

    if (analysis == nullptr || analysis->overview.isEmpty())
    {
        paintEmpty (g);
        return;
    }

    if (mode == Mode::overview)
        paintOverview (g);
    else
        paintScrolling (g);
}

void WaveformComponent::paintEmpty (juce::Graphics& g)
{
    g.setColour (juce::Colours::darkgrey);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("Drop a track here or press Load", getLocalBounds(), juce::Justification::centred);
}

void WaveformComponent::paintOverview (juce::Graphics& g)
{
    const auto& peaks = analysis->overview;
    const auto width = getWidth();
    const auto height = static_cast<float> (getHeight());
    const auto centre = height * 0.5f;

    const auto playedX = trackLengthSeconds > 0.0
        ? static_cast<float> (positionSeconds / trackLengthSeconds) * width
        : 0.0f;

    for (int x = 0; x < width; ++x)
    {
        const auto bucketIndex = static_cast<size_t> (
            static_cast<double> (x) / width * static_cast<double> (peaks.buckets.size()));

        if (bucketIndex >= peaks.buckets.size())
            break;

        const auto& bucket = peaks.buckets[bucketIndex];
        const auto top = centre - bucket.maximum * centre;
        const auto bottom = centre - bucket.minimum * centre;

        g.setColour (static_cast<float> (x) <= playedX ? playedColour : waveColour);
        g.drawVerticalLine (x, top, juce::jmax (top + 1.0f, bottom));
    }

    if (trackLengthSeconds > 0.0)
    {
        const auto cueX = static_cast<float> (cueSeconds / trackLengthSeconds) * width;
        g.setColour (cueColour);
        g.fillRect (cueX - 1.0f, 0.0f, 2.0f, height);

        g.setColour (juce::Colours::white);
        g.fillRect (playedX - 0.5f, 0.0f, 1.0f, height);
    }
}

void WaveformComponent::paintScrolling (juce::Graphics& g)
{
    const auto& peaks = analysis->detail;

    if (peaks.isEmpty())
        return;

    const auto width = getWidth();
    const auto height = static_cast<float> (getHeight());
    const auto centre = height * 0.5f;

    const auto spanSeconds = windowSeconds * 2.0;
    const auto startSeconds = positionSeconds - windowSeconds;
    const auto secondsPerPixel = spanSeconds / juce::jmax (1, width);

    // Beat grid first, so the waveform draws over it.
    if (analysis->hasTempo())
    {
        const auto period = analysis->secondsPerBeat();
        const auto firstBeatIndex = std::floor ((startSeconds - analysis->firstBeatSeconds) / period);

        for (double beat = firstBeatIndex; ; beat += 1.0)
        {
            const auto beatSeconds = analysis->firstBeatSeconds + beat * period;

            if (beatSeconds > startSeconds + spanSeconds)
                break;

            if (beatSeconds < startSeconds)
                continue;

            const auto x = static_cast<float> ((beatSeconds - startSeconds) / secondsPerPixel);

            // Every fourth beat is a bar line, drawn brighter, which is what
            // makes phrasing readable at a glance.
            const auto isBarLine = std::abs (std::fmod (beat, 4.0)) < 0.001;
            g.setColour (isBarLine ? barColour : beatColour);
            g.fillRect (x, 0.0f, isBarLine ? 2.0f : 1.0f, height);
        }
    }

    for (int x = 0; x < width; ++x)
    {
        const auto seconds = startSeconds + x * secondsPerPixel;

        if (seconds < 0.0 || seconds > trackLengthSeconds)
            continue;

        const auto sampleIndex = static_cast<juce::int64> (seconds * analysis->sampleRate);
        const auto& bucket = peaks.bucketAt (sampleIndex);

        const auto top = centre - bucket.maximum * centre;
        const auto bottom = centre - bucket.minimum * centre;

        g.setColour (seconds <= positionSeconds ? playedColour : waveColour);
        g.drawVerticalLine (x, top, juce::jmax (top + 1.0f, bottom));
    }

    // The playhead is fixed in the middle; the track moves past it.
    g.setColour (juce::Colours::white);
    g.fillRect (width * 0.5f - 1.0f, 0.0f, 2.0f, height);
}

void WaveformComponent::mouseDown (const juce::MouseEvent& e)
{
    seekFromMouse (e);
}

void WaveformComponent::mouseDrag (const juce::MouseEvent& e)
{
    seekFromMouse (e);
}

void WaveformComponent::seekFromMouse (const juce::MouseEvent& e)
{
    if (onSeek == nullptr || trackLengthSeconds <= 0.0 || mode != Mode::overview)
        return;

    const auto proportion = juce::jlimit (0.0, 1.0,
                                          static_cast<double> (e.position.x)
                                              / juce::jmax (1.0, static_cast<double> (getWidth())));
    onSeek (proportion * trackLengthSeconds);
}

} // namespace opendj
