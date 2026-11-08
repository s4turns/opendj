/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/BeatGridEditor.h"

namespace opendj
{

namespace
{
    const juce::Colour panelColour  { 0xff1c1c22 };
    const juce::Colour buttonColour { 0xff2c2c34 };
    const juce::Colour accentColour { 0xff35c2f0 };

    /** One press of the earlier and later buttons. */
    constexpr double shiftSeconds = 0.005;

    constexpr int editorWidth = 330, editorHeight = 150;
}

BeatGridEditor::BeatGridEditor (Deck& deckToEdit) : deck (deckToEdit)
{
    tempoLabel.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    tempoLabel.setColour (juce::Label::textColourId, accentColour);
    addAndMakeVisible (tempoLabel);

    stateLabel.setFont (juce::FontOptions (12.0f));
    stateLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    stateLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (stateLabel);

    const struct { const char* text; double delta; } steps[]
    {
        { "-1", -1.0 }, { "-0.1", -0.1 }, { "-0.01", -0.01 },
        { "+0.01", 0.01 }, { "+0.1", 0.1 }, { "+1", 1.0 }
    };

    for (const auto& step : steps)
    {
        const auto delta = step.delta;
        tempoButtons.add (&addButton (step.text,
                                      "Change the tempo by " + juce::String (delta) + " BPM, keeping the beat "
                                      "nearest the playhead where it is",
                                      [this, delta] { deck.adjustGridBpm (delta); }));
    }

    tempoButtons.add (&addButton ("/2", "Halve the tempo, for a track found at double speed",
                                  [this] { deck.scaleGridBpm (0.5); }));
    tempoButtons.add (&addButton ("x2", "Double the tempo, for a track found at half speed",
                                  [this] { deck.scaleGridBpm (2.0); }));

    phaseButtons.add (&addButton (juce::String::fromUTF8 ("\xe2\x97\x80 Earlier"),
                                  "Slide the whole grid 5 ms earlier",
                                  [this] { deck.shiftGrid (-shiftSeconds); }));
    phaseButtons.add (&addButton (juce::String::fromUTF8 ("Later \xe2\x96\xb6"),
                                  "Slide the whole grid 5 ms later",
                                  [this] { deck.shiftGrid (shiftSeconds); }));
    phaseButtons.add (&addButton ("Downbeat here",
                                  "Make the playhead the first beat of a bar, keeping the tempo",
                                  [this] { deck.setDownbeatHere(); }));

    // A tap counts the moment the button goes down, not when it is let go,
    // which is when a finger keeping time actually lands.
    tapButton.setTooltip ("Tap along with the beat. Four taps or more set the tempo; "
                          "stop for two seconds to start again");
    tapButton.setColour (juce::TextButton::buttonColourId, buttonColour);
    tapButton.setTriggeredOnMouseDown (true);
    tapButton.onClick = [this]
    {
        deck.tapTempo (juce::Time::getMillisecondCounterHiRes() * 0.001);
        refresh();
    };
    addAndMakeVisible (tapButton);

    revertButton.setTooltip ("Put back the grid the track had when it was loaded");
    revertButton.setColour (juce::TextButton::buttonColourId, buttonColour);
    revertButton.onClick = [this] { deck.revertBeatGrid(); refresh(); };
    addAndMakeVisible (revertButton);

    setSize (editorWidth, editorHeight);
    refresh();
    startTimerHz (10);
}

BeatGridEditor::~BeatGridEditor() { stopTimer(); }

juce::TextButton& BeatGridEditor::addButton (const juce::String& text, const juce::String& tooltip,
                                             std::function<void()> action)
{
    // Owned by the arrays they are added to; made visible here.
    auto* button = new juce::TextButton (text);
    button->setTooltip (tooltip);
    button->setColour (juce::TextButton::buttonColourId, buttonColour);
    button->onClick = [this, action = std::move (action)] { action(); refresh(); };
    addAndMakeVisible (button);
    return *button;
}

void BeatGridEditor::showFor (Deck& deck, juce::Component& target)
{
    auto editor = std::make_unique<BeatGridEditor> (deck);
    juce::CallOutBox::launchAsynchronously (std::move (editor), target.getScreenBounds(), nullptr);
}

void BeatGridEditor::paint (juce::Graphics& g)
{
    g.fillAll (panelColour);
}

void BeatGridEditor::resized()
{
    auto area = getLocalBounds().reduced (6);

    auto top = area.removeFromTop (28);
    stateLabel.setBounds (top.removeFromRight (140));
    tempoLabel.setBounds (top);

    const auto layoutRow = [&area] (juce::Array<juce::Component*> items)
    {
        area.removeFromTop (4);
        auto row = area.removeFromTop (26);
        const auto width = row.getWidth() / juce::jmax (1, items.size());

        for (auto* item : items)
            item->setBounds (row.removeFromLeft (width).reduced (1, 0));
    };

    juce::Array<juce::Component*> tempoRow, scaleRow;

    for (int i = 0; i < tempoButtons.size(); ++i)
        (i < 6 ? tempoRow : scaleRow).add (tempoButtons[i]);

    layoutRow (tempoRow);

    scaleRow.add (phaseButtons[0]);
    scaleRow.add (phaseButtons[1]);
    layoutRow (scaleRow);

    layoutRow ({ phaseButtons[2], &tapButton, &revertButton });
}

void BeatGridEditor::timerCallback()
{
    refresh();
}

void BeatGridEditor::refresh()
{
    const auto analysis = deck.getAnalysis();
    const auto hasGrid = analysis != nullptr && analysis->hasTempo();

    const auto tempo = hasGrid ? juce::String (analysis->bpm, 2) + " BPM" : juce::String ("No beat grid");

    if (tempoLabel.getText() != tempo)
        tempoLabel.setText (tempo, juce::dontSendNotification);

    const auto edited = deck.isBeatGridEdited();
    const juce::String state = analysis == nullptr ? "Nothing loaded"
                             : edited ? "Edited, saved with the track"
                             : hasGrid ? "As loaded" : "Tap to start one";

    if (stateLabel.getText() != state)
        stateLabel.setText (state, juce::dontSendNotification);

    for (auto* button : tempoButtons)
        button->setEnabled (hasGrid);

    for (auto* button : phaseButtons)
        button->setEnabled (hasGrid);

    tapButton.setEnabled (analysis != nullptr);
    revertButton.setEnabled (edited);
}

} // namespace opendj
