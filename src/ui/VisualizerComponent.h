/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "visual/Visualizer.h"

#include <vector>

namespace opendj
{

/** Shows what the visualiser is drawing.

    Nothing here draws with OpenGL. The visualiser renders on its own thread
    into its own context and hands back finished frames; this is a picture of
    the latest one, refreshed on a timer, scaled to fit whatever size the
    window has been given. That is deliberately the cheap design: a second
    OpenGL context for the window would be more to get wrong and would tie
    the visuals' lifetime to the window's, which the broadcast cannot have.

    Click for the next preset. Double-click fills the screen and again
    restores it, because a projector on a second monitor is where this most
    wants to be.
*/
class VisualizerComponent final : public juce::Component,
                                  private juce::Timer
{
public:
    explicit VisualizerComponent (Visualizer& visualizerToUse);
    ~VisualizerComponent() override;

    void paint (juce::Graphics& g) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseDoubleClick (const juce::MouseEvent& event) override;

private:
    void timerCallback() override;

    Visualizer& visualizer;
    juce::Image frame;
    std::vector<unsigned char> rgb;
    juce::int64 framesShown = 0;
    juce::String presetName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VisualizerComponent)
};

} // namespace opendj
