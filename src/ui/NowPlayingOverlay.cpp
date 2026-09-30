/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/NowPlayingOverlay.h"

#include <cstring>

namespace opendj
{

void NowPlayingOverlay::setTitle (const juce::String& newTitle)
{
    const std::lock_guard<std::mutex> lock (mutex);

    if (newTitle == title)
        return;

    title = newTitle;
    changedAtSeconds = juce::Time::getMillisecondCounterHiRes() / 1000.0;
}

float NowPlayingOverlay::opacityAt (double seconds) noexcept
{
    if (seconds < 0.0 || seconds >= holdSeconds + fadeSeconds)
        return 0.0f;

    if (seconds < fadeSeconds)
        return (float) (seconds / fadeSeconds);

    if (seconds < holdSeconds)
        return 1.0f;

    return (float) (1.0 - (seconds - holdSeconds) / fadeSeconds);
}

void NowPlayingOverlay::draw (unsigned char* rgb, int width, int height)
{
    juce::String text;
    double since = 0.0;

    {
        const std::lock_guard<std::mutex> lock (mutex);
        text = title;
        since = juce::Time::getMillisecondCounterHiRes() / 1000.0 - changedAtSeconds;
    }

    const auto opacity = opacityAt (since);

    if (text.isEmpty() || opacity <= 0.0f || width <= 0 || height <= 0)
        return;

    // The strip the text lives in, and only that: a full frame through a
    // software renderer thirty times a second is work the visuals can do
    // without, and the strip is a twelfth of it.
    const auto stripHeight = juce::jmax (8, height / 6);
    const auto stripTop = height - stripHeight;

    if (! canvas.isValid() || canvas.getWidth() != width || canvas.getHeight() != stripHeight)
        canvas = juce::Image (juce::Image::ARGB, width, stripHeight, true);

    canvas.clear (canvas.getBounds());

    {
        juce::Graphics g (canvas);
        const auto margin = (float) height / 24.0f;
        const auto fontHeight = (float) height / 18.0f;
        const auto area = juce::Rectangle<float> (margin, 0.0f, (float) width - margin * 2.0f, (float) stripHeight)
                              .withTrimmedBottom (margin * 0.6f);

        g.setFont (juce::FontOptions (fontHeight, juce::Font::bold));

        // A soft dark band so white text survives a white preset, then a
        // shadow and the text itself.
        g.setGradientFill (juce::ColourGradient (juce::Colours::transparentBlack, 0.0f, 0.0f,
                                                 juce::Colours::black.withAlpha (0.65f), 0.0f, (float) stripHeight,
                                                 false));
        g.fillAll();

        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.drawFittedText (text, area.translated (2.0f, 2.0f).toNearestInt(),
                          juce::Justification::bottomLeft, 2, 0.8f);
        g.setColour (juce::Colours::white);
        g.drawFittedText (text, area.toNearestInt(), juce::Justification::bottomLeft, 2, 0.8f);
    }

    juce::Image::BitmapData bitmap (canvas, juce::Image::BitmapData::readOnly);

    for (int y = 0; y < stripHeight; ++y)
    {
        const auto* source = bitmap.getLinePointer (y);
        auto* destination = rgb + ((size_t) (stripTop + y) * (size_t) width) * 3;

        for (int x = 0; x < width; ++x)
        {
            // JUCE's ARGB images are premultiplied BGRA in memory.
            const auto* pixel = source + x * 4;
            const auto coverage = (float) pixel[3] / 255.0f * opacity;
            const auto keep = 1.0f - coverage;
            const auto premultipliedScale = opacity;

            destination[x * 3 + 0] = (unsigned char) juce::jmin (255.0f, destination[x * 3 + 0] * keep + pixel[2] * premultipliedScale);
            destination[x * 3 + 1] = (unsigned char) juce::jmin (255.0f, destination[x * 3 + 1] * keep + pixel[1] * premultipliedScale);
            destination[x * 3 + 2] = (unsigned char) juce::jmin (255.0f, destination[x * 3 + 2] * keep + pixel[0] * premultipliedScale);
        }
    }
}

} // namespace opendj
