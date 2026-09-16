/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/VisualizerComponent.h"

namespace opendj
{

VisualizerComponent::VisualizerComponent (Visualizer& visualizerToUse)
    : visualizer (visualizerToUse)
{
    setOpaque (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);

    // A little faster than the visuals are drawn, so a frame is never shown
    // late by a whole tick of this timer on top of its own.
    startTimerHz (40);
}

VisualizerComponent::~VisualizerComponent()
{
    stopTimer();
}

void VisualizerComponent::timerCallback()
{
    // Only fetched when there is something new: a copy of a frame is not
    // free, and the counter says whether one has been drawn since.
    const auto rendered = visualizer.getFramesRendered();

    if (rendered == framesShown && frame.isValid())
        return;

    framesShown = rendered;

    if (! visualizer.copyLatestFrame (rgb))
    {
        if (frame.isValid())
        {
            frame = {};
            repaint();
        }

        return;
    }

    const auto width = visualizer.getFrameWidth();
    const auto height = visualizer.getFrameHeight();

    if (! frame.isValid() || frame.getWidth() != width || frame.getHeight() != height)
        frame = juce::Image (juce::Image::RGB, width, height, false);

    // Row by row into the image's own memory, rather than a pixel at a time
    // through setPixelAt, which would be thirty million calls a second at
    // 1080p. JUCE's RGB images are three bytes a pixel in BGR order, so the
    // channels are swapped on the way in.
    juce::Image::BitmapData bitmap (frame, juce::Image::BitmapData::writeOnly);

    for (int y = 0; y < height; ++y)
    {
        const auto* source = rgb.data() + (size_t) y * (size_t) width * 3;
        auto* destination = bitmap.getLinePointer (y);

        for (int x = 0; x < width; ++x)
        {
            destination[x * 3 + 0] = source[x * 3 + 2];
            destination[x * 3 + 1] = source[x * 3 + 1];
            destination[x * 3 + 2] = source[x * 3 + 0];
        }
    }

    presetName = visualizer.getCurrentPresetName();
    repaint();
}

void VisualizerComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    if (! frame.isValid())
    {
        g.setColour (juce::Colours::grey);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText (visualizer.isRunning() ? "Waiting for the first frame"
                                           : "Visuals are off",
                    getLocalBounds(), juce::Justification::centred);
        return;
    }

    // Scaled to fit with the frame's own proportions kept, letterboxed in
    // black on whichever axis has room to spare. The window is whatever
    // shape the person made it; the broadcast is 16:9 regardless.
    g.drawImage (frame, getLocalBounds().toFloat(),
                 juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize);

    if (presetName.isNotEmpty())
    {
        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (presetName, getLocalBounds().reduced (8), juce::Justification::bottomLeft);
    }
}

void VisualizerComponent::mouseUp (const juce::MouseEvent& event)
{
    if (event.mouseWasClicked() && event.getNumberOfClicks() == 1)
        visualizer.nextPreset();
}

void VisualizerComponent::mouseDoubleClick (const juce::MouseEvent&)
{
    if (auto* window = findParentComponentOfClass<juce::ResizableWindow>())
        window->setFullScreen (! window->isFullScreen());
}

} // namespace opendj
