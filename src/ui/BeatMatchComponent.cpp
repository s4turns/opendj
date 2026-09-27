/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/BeatMatchComponent.h"

#include "ui/WaveformMath.h"

namespace opendj
{

namespace
{
    const juce::Colour backgroundColour { 0xff0e0e12 };
    const juce::Colour laneColour       { 0xff17171d };
    const juce::Colour beatColour       { 0x85ffffff };
    const juce::Colour barColour        { 0xd0ffffff };
    const juce::Colour labelColour      { 0xff6a6a78 };
    const juce::Colour playheadColour   { 0xffe8a33d };

    /** What the strip shows when neither deck has a tempo to draw. Two seconds
        either side is about the width of a phrase at any sane tempo. */
    constexpr double fallbackHalfSpan = 2.0;

    constexpr int labelWidth = 22;
    constexpr int rowGap = 3;
}

BeatMatchComponent::BeatMatchComponent (AudioEngine& engineToUse)
    : engine (engineToUse)
{
    setOpaque (true);
    setInterceptsMouseClicks (false, false);
    setDecks (0, 1);
}

void BeatMatchComponent::setDecks (int leftDeck, int rightDeck)
{
    rows[0].deck = leftDeck;
    rows[1].deck = rightDeck;

    for (auto& row : rows)
        row.name = juce::String::charToString ('A' + static_cast<juce::juce_wchar> (row.deck));

    refresh();
}

void BeatMatchComponent::refresh()
{
    auto changed = false;

    for (auto& row : rows)
    {
        auto& deck = engine.getDeck (row.deck);
        const auto analysis = deck.getAnalysis();

        Row fresh;
        fresh.deck = row.deck;
        fresh.name = row.name;
        fresh.loaded = deck.isLoaded();
        fresh.bpm = analysis != nullptr ? analysis->bpm : 0.0;
        fresh.firstBeatSeconds = analysis != nullptr ? analysis->firstBeatSeconds : 0.0;
        fresh.positionSeconds = deck.getPositionSeconds();
        fresh.tempoRatio = deck.getTempoRatio();

        if (! juce::approximatelyEqual (fresh.positionSeconds, row.positionSeconds)
            || ! juce::approximatelyEqual (fresh.bpm, row.bpm)
            || ! juce::approximatelyEqual (fresh.firstBeatSeconds, row.firstBeatSeconds)
            || ! juce::approximatelyEqual (fresh.tempoRatio, row.tempoRatio)
            || fresh.loaded != row.loaded)
        {
            changed = true;
        }

        row = fresh;
    }

    if (changed)
        repaint();
}

double BeatMatchComponent::halfSpanSeconds() const
{
    auto total = 0.0;
    auto counted = 0;

    for (const auto& row : rows)
        if (const auto beat = waveform::playedSecondsPerBeat (row.bpm, row.tempoRatio); beat > 0.0)
        {
            total += beat;
            ++counted;
        }

    if (counted == 0)
        return fallbackHalfSpan;

    // Four beats either side. Averaged when both decks have a tempo, so a strip
    // holding two decks that are nearly matched does not jump about depending on
    // which of them happens to be fractionally faster.
    return total / counted * 4.0;
}

void BeatMatchComponent::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);

    auto area = getLocalBounds().reduced (2);
    const auto span = halfSpanSeconds();
    const auto rowHeight = (area.getHeight() - rowGap) / 2;

    paintRow (g, rows[0], area.removeFromTop (rowHeight), span);
    area.removeFromTop (rowGap);
    paintRow (g, rows[1], area.removeFromTop (rowHeight), span);

    // One playhead for both rows, so the eye has something to read the stagger
    // against rather than only the two rows against each other.
    g.setColour (playheadColour);
    g.fillRect (static_cast<float> (getWidth()) * 0.5f - 1.0f, 0.0f,
                2.0f, static_cast<float> (getHeight()));
}

void BeatMatchComponent::paintRow (juce::Graphics& g, const Row& row,
                                   juce::Rectangle<int> area, double span) const
{
    const auto label = area.removeFromLeft (labelWidth);

    // A lane behind each row, so the two read as two things being compared
    // rather than as one field of ticks at two heights.
    g.setColour (laneColour);
    g.fillRect (area);

    // Dim when there is nothing to line up, which is the truth, rather than a
    // row that looks ready and is showing a made up grid.
    const auto hasGrid = row.loaded && row.bpm > 0.0;

    g.setColour (hasGrid ? barColour : labelColour);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (row.name, label, juce::Justification::centred);

    if (! hasGrid)
        return;

    const auto marks = waveform::beatMarks (row.bpm, row.firstBeatSeconds,
                                            row.positionSeconds, row.tempoRatio, span);

    // Measured across the whole strip rather than this row, so a beat at the
    // same moment on both decks lands in the same column.
    const auto centreX = static_cast<double> (getWidth()) * 0.5;
    const auto perSecond = centreX / span;

    const auto top = static_cast<float> (area.getY());
    const auto height = static_cast<float> (area.getHeight());

    for (const auto& mark : marks)
    {
        const auto x = static_cast<float> (centreX + mark.offsetSeconds * perSecond);

        if (x < static_cast<float> (area.getX()))
            continue;

        // The downbeat is the one worth finding: matching tempo is not the same
        // as matching the one in four that the phrase turns on.
        g.setColour (mark.isDownbeat ? barColour : beatColour);
        g.fillRect (x - 1.0f,
                    mark.isDownbeat ? top : top + height * 0.28f,
                    mark.isDownbeat ? 2.0f : 1.0f,
                    mark.isDownbeat ? height : height * 0.44f);
    }
}

} // namespace opendj
