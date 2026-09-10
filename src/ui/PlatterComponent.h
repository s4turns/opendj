/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "analysis/TrackAnalysis.h"
#include "core/Deck.h"

#include <memory>

namespace opendj

{

/** A turntable platter you can see and drag.

    It turns at 33 1/3 rpm against the track position, so it is a real readout
    rather than decoration: if the platter is crawling, the deck is crawling.
    Dragging the middle scratches and dragging the outer ring nudges the pitch,
    both through the same jog input a hardware platter uses, so the two cannot
    behave differently.
*/
class PlatterComponent final : public juce::Component
{
public:
    PlatterComponent (Deck& deckToControl, const juce::String& deckName);

    /** Pulls position and beat phase from the deck. Driven by the shell timer. */
    void refresh (const std::shared_ptr<const TrackAnalysis>& analysis);

    void paint (juce::Graphics& g) override;

private:
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;

    juce::Point<float> platterCentre() const;
    float platterRadius() const;

    /** True when a point is in the middle of the platter rather than the ring
        around its edge. The DJ-202 makes the same distinction with a capacitive
        top and a non-sensing outer ring. */
    bool isOnRecord (juce::Point<float> point) const;

    Deck& deck;
    juce::String name;

    double displayedAngle = 0.0;
    double lastDragAngle = 0.0;
    bool draggingRecord = false;
    bool draggingRing = false;

    double beatPhase = -1.0;     ///< 0 to 1 through the current beat, or negative
    bool loaded = false;
    bool playing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlatterComponent)
};

} // namespace opendj
