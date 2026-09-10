/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/PlatterComponent.h"

#include "ui/AngleMath.h"

namespace opendj
{

namespace
{
    const juce::Colour recordColour   { 0xff17171c };
    const juce::Colour ringColour     { 0xff26262e };
    const juce::Colour grooveColour   { 0x14ffffff };
    const juce::Colour markerColour   { 0xffe8e8ee };
    const juce::Colour accentColour   { 0xff35c2f0 };
    const juce::Colour labelColour    { 0xff2a2a33 };
    const juce::Colour touchedColour  { 0xffe8a33d };

    // The outer fifth of the platter is the ring: nudge territory rather than
    // scratch territory, the same split the hardware makes.
    constexpr float ringProportion = 0.8f;

    // Match the DJ-202 so a mouse drag and a real platter speak the same units.
    constexpr int mouseTicksPerRevolution = 512;

    // A beat marker is only worth drawing for a moment after the beat lands.
    constexpr double beatFlashProportion = 0.12;
}

PlatterComponent::PlatterComponent (Deck& deckToControl, const juce::String& deckName)
    : deck (deckToControl), name (deckName)
{
    setInterceptsMouseClicks (true, false);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

juce::Point<float> PlatterComponent::platterCentre() const
{
    return getLocalBounds().toFloat().getCentre();
}

float PlatterComponent::platterRadius() const
{
    return juce::jmin (getWidth(), getHeight()) * 0.5f - 2.0f;
}

bool PlatterComponent::isOnRecord (juce::Point<float> point) const
{
    return point.getDistanceFrom (platterCentre()) < platterRadius() * ringProportion;
}

void PlatterComponent::refresh (const std::shared_ptr<const TrackAnalysis>& analysis)
{
    const auto position = deck.getPositionSeconds();
    const auto angle = angles::platterAngle (position);

    auto phase = -1.0;

    if (analysis != nullptr && analysis->hasTempo())
    {
        const auto period = analysis->secondsPerBeat();
        phase = std::fmod (position - analysis->firstBeatSeconds, period) / period;

        if (phase < 0.0)
            phase += 1.0;
    }

    const auto nowLoaded = deck.isLoaded();
    const auto nowPlaying = deck.isPlaying() || deck.isJogTouched();

    // Only repaint when something actually moved. At thirty frames a second
    // across two decks this is the difference between idle and busy.
    if (juce::approximatelyEqual (angle, displayedAngle)
        && juce::approximatelyEqual (phase, beatPhase)
        && nowLoaded == loaded
        && nowPlaying == playing)
        return;

    displayedAngle = angle;
    beatPhase = phase;
    loaded = nowLoaded;
    playing = nowPlaying;
    repaint();
}

void PlatterComponent::paint (juce::Graphics& g)
{
    const auto centre = platterCentre();
    const auto radius = platterRadius();

    if (radius <= 4.0f)
        return;

    const auto circle = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);

    // Outer ring.
    g.setColour (draggingRing ? touchedColour.withAlpha (0.35f) : ringColour);
    g.fillEllipse (circle);

    // The record itself.
    const auto recordRadius = radius * ringProportion;
    const auto record = juce::Rectangle<float> (recordRadius * 2.0f, recordRadius * 2.0f)
                            .withCentre (centre);

    g.setColour (deck.isJogTouched() ? recordColour.brighter (0.12f) : recordColour);
    g.fillEllipse (record);

    // Grooves, so rotation is legible even without looking at the marker.
    g.setColour (grooveColour);

    for (auto r = recordRadius * 0.45f; r < recordRadius; r += juce::jmax (3.0f, recordRadius * 0.09f))
        g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (centre), 1.0f);

    // Beat marker on the ring: it lights as each beat passes, which is a second
    // way to see two decks drifting apart without reading the waveforms. It only
    // flashes while the deck is moving, or a deck parked on a beat would sit
    // there permanently lit and read as selected rather than as on the beat.
    if (playing && beatPhase >= 0.0 && beatPhase < beatFlashProportion)
    {
        const auto brightness = 1.0f - static_cast<float> (beatPhase / beatFlashProportion);

        g.setColour (accentColour.withAlpha (brightness));
        g.drawEllipse (circle.reduced (1.0f), juce::jmax (2.0f, radius * 0.05f));
    }

    if (loaded)
    {
        // The marker stripe. This is the whole point of a platter: one glance
        // says how fast the track is running and where in the bar it is.
        juce::Path marker;
        marker.addRoundedRectangle (-radius * 0.022f, -recordRadius * 0.94f,
                                    radius * 0.044f, recordRadius * 0.62f,
                                    radius * 0.02f);

        g.setColour (deck.isJogTouched() ? touchedColour : markerColour);
        g.fillPath (marker, juce::AffineTransform::rotation (static_cast<float> (displayedAngle))
                                .translated (centre));
    }

    // Centre label and spindle.
    const auto labelRadius = recordRadius * 0.42f;
    g.setColour (labelColour);
    g.fillEllipse (juce::Rectangle<float> (labelRadius * 2.0f, labelRadius * 2.0f).withCentre (centre));

    g.setColour (loaded ? (playing ? accentColour : juce::Colours::grey) : juce::Colours::dimgrey);
    g.setFont (juce::FontOptions (juce::jmax (11.0f, labelRadius * 0.75f), juce::Font::bold));
    g.drawText (name,
                juce::Rectangle<float> (labelRadius * 2.0f, labelRadius * 2.0f).withCentre (centre),
                juce::Justification::centred);

    g.setColour (juce::Colours::black);
    g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre (centre));
}

void PlatterComponent::mouseDown (const juce::MouseEvent& e)
{
    if (! deck.isLoaded())
        return;

    const auto centre = platterCentre();
    lastDragAngle = angles::angleFromCentre (e.position.x, e.position.y, centre.x, centre.y);
    deck.setJogTicksPerRevolution (mouseTicksPerRevolution);

    if (isOnRecord (e.position))
    {
        draggingRecord = true;
        deck.setJogTouched (true);
    }
    else
    {
        draggingRing = true;
    }

    repaint();
}

void PlatterComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingRecord && ! draggingRing)
        return;

    const auto centre = platterCentre();
    const auto angle = angles::angleFromCentre (e.position.x, e.position.y, centre.x, centre.y);
    const auto delta = angles::shortestDelta (lastDragAngle, angle);
    lastDragAngle = angle;

    deck.addJogTicks (angles::toTicks (delta, mouseTicksPerRevolution));
}

void PlatterComponent::mouseUp (const juce::MouseEvent&)
{
    if (draggingRecord)
        deck.setJogTouched (false);

    draggingRecord = false;
    draggingRing = false;
    repaint();
}

} // namespace opendj
