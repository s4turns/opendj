/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/DeckComponent.h"

#include "control/Action.h"

#include <utility>

#include <cmath>

namespace opendj
{

namespace
{
    juce::String formatTime (double seconds)
    {
        if (seconds < 0.0 || ! std::isfinite (seconds))
            seconds = 0.0;

        const auto total = static_cast<int> (seconds);
        return juce::String::formatted ("%d:%02d.%d",
                                        total / 60,
                                        total % 60,
                                        static_cast<int> ((seconds - total) * 10.0));
    }

    const juce::Colour panelColour   { 0xff1c1c22 };
    const juce::Colour accentColour  { 0xff35c2f0 };
    const juce::Colour cueColour     { 0xffe8a33d };
    const juce::Colour loopColour    { 0xff4ad991 };
}

DeckComponent::DeckComponent (AudioEngine& engineToUse, int deckIndex, const juce::String& deckName)
    : engine (engineToUse), index (deckIndex), name (deckName),
      deck (engineToUse.getDeck (deckIndex)), platter (deck, deckName)
{
    addAndMakeVisible (platter);

    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    titleLabel.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    addAndMakeVisible (titleLabel);

    timeLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    timeLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (timeLabel);

    bpmLabel.setColour (juce::Label::textColourId, accentColour);
    bpmLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    bpmLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (bpmLabel);

    loadButton.onClick = [this] { loadButtonClicked(); };
    addAndMakeVisible (loadButton);

    playButton.onClick = [this] { deck.togglePlay(); refresh(); };
    addAndMakeVisible (playButton);

    cueButton.setColour (juce::TextButton::buttonColourId, cueColour.darker (0.6f));
    cueButton.onPress = [this] { deck.cuePressed(); refresh(); };
    cueButton.onRelease = [this] { deck.cueReleased(); refresh(); };
    addAndMakeVisible (cueButton);

    syncButton.onClick = [this]
    {
        // With two decks the other one is always the leader.
        if (engine.syncDeck (index, 1 - index))
            updateTempoReadout();
    };
    addAndMakeVisible (syncButton);

    keyLockButton.setTooltip ("Key lock: the tempo fader stops changing the pitch");
    keyLockButton.onClick = [this] { deck.toggleKeyLock(); refresh(); };
    addAndMakeVisible (keyLockButton);

    // Loop lengths. A click sets a loop of that many beats and leaves it
    // running; holding turns the same button into a roll, which repeats while
    // it is down and then drops you where the track would have reached. One
    // button, both behaviours, decided by how long it is held.
    for (int i = 0; i < numLoopButtons; ++i)
    {
        const auto slot = i + 1;                 // start at half a beat
        const auto beats = loopBeatsForSlot (slot);
        auto& button = loopButtons[(size_t) i];

        button.setButtonText (beats < 1.0 ? "1/" + juce::String (juce::roundToInt (1.0 / beats))
                                          : juce::String (beats, 0));
        button.setTooltip ("Click for a " + button.getButtonText()
                           + " beat loop, hold for a roll");

        button.onPress = [this, i, beats]
        {
            // A roll only makes sense over a running deck; on a stopped one the
            // button behaves as a plain loop.
            if (deck.isPlaying() && deck.beginLoopRoll (beats))
                loopButtonRolling[(size_t) i] = true;
            else
                deck.setLoopBeats (beats);

            refresh();
        };

        button.onRelease = [this, i]
        {
            if (std::exchange (loopButtonRolling[(size_t) i], false))
                deck.endLoopRoll();

            refresh();
        };

        addAndMakeVisible (button);
    }

    loopToggleButton.setTooltip ("Turn the current loop on or off");
    loopToggleButton.onClick = [this] { deck.toggleLoop(); refresh(); };
    addAndMakeVisible (loopToggleButton);

    loopHalveButton.setTooltip ("Halve the loop, keeping its start");
    loopHalveButton.onClick = [this] { deck.halveLoop(); refresh(); };
    addAndMakeVisible (loopHalveButton);

    loopDoubleButton.setTooltip ("Double the loop, keeping its start");
    loopDoubleButton.onClick = [this] { deck.doubleLoop(); refresh(); };
    addAndMakeVisible (loopDoubleButton);

    tempoSlider.setSliderStyle (juce::Slider::LinearVertical);
    tempoSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    tempoSlider.setRange (-1.0, 1.0, 0.0);
    tempoSlider.setValue (0.0, juce::dontSendNotification);
    tempoSlider.setDoubleClickReturnValue (true, 0.0);
    tempoSlider.onValueChange = [this] { applyTempoFromSlider(); };
    addAndMakeVisible (tempoSlider);

    tempoRangeBox.addItemList ({ "8%", "16%", "50%" }, 1);
    tempoRangeBox.setSelectedId (1, juce::dontSendNotification);
    tempoRangeBox.onChange = [this] { applyTempoFromSlider(); };
    addAndMakeVisible (tempoRangeBox);

    tempoLabel.setJustificationType (juce::Justification::centred);
    tempoLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible (tempoLabel);

    // Stems: a knob each, and the button that asks for a separation.
    stemControls.separate.setTooltip ("Separate this track into drums, bass, other and vocals");
    stemControls.separate.onClick = [this]
    {
        if (deck.hasStems() || engine.isDeckSeparating (index))
            return;

        engine.separateDeckAsync (index, [safe = juce::Component::SafePointer<DeckComponent> (this)] (bool ok)
        {
            if (safe == nullptr)
                return;

            if (! ok)
                juce::NativeMessageBox::showMessageBoxAsync (
                    juce::MessageBoxIconType::WarningIcon,
                    "Could not separate the track",
                    safe->engine.getStemSeparator().getStatusDescription());

            safe->refresh();
        });

        refresh();
    };
    addAndMakeVisible (stemControls.separate);

    for (int stem = 0; stem < numStems; ++stem)
    {
        auto& knob = stemControls.knobs[(size_t) stem];
        knob.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob.setRange (0.0, 1.0, 0.0);
        knob.setValue (1.0, juce::dontSendNotification);
        knob.setDoubleClickReturnValue (true, 1.0);
        knob.setEnabled (false);
        knob.onValueChange = [this, stem]
        {
            deck.setStemGain (static_cast<Stem> (stem),
                              (float) stemControls.knobs[(size_t) stem].getValue());
        };
        addAndMakeVisible (knob);

        auto& label = stemControls.labels[(size_t) stem];
        label.setText (juce::String (toString (static_cast<Stem> (stem))).substring (0, 1).toUpperCase()
                           + juce::String (toString (static_cast<Stem> (stem))).substring (1),
                       juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, juce::Colours::grey);
        label.setFont (juce::FontOptions (11.0f));
        addAndMakeVisible (label);
    }

    addAndMakeVisible (scrollingWave);

    overviewWave.onSeek = [this] (double seconds) { deck.seekToSeconds (seconds); refresh(); };
    addAndMakeVisible (overviewWave);

    refresh();
}

DeckComponent::~DeckComponent() = default;

void DeckComponent::load (const juce::File& file)
{
    engine.loadTrackAsync (index, file, [safe = juce::Component::SafePointer<DeckComponent> (this),
                                         file] (bool succeeded)
    {
        if (safe == nullptr)
            return;

        if (! succeeded)
            juce::NativeMessageBox::showMessageBoxAsync (
                juce::MessageBoxIconType::WarningIcon,
                "Could not load " + file.getFileName(),
                // Say which of the possible causes it was. A dialog that lists
                // three and commits to none leaves the reader no better off.
                safe->deck.getLastLoadError().isNotEmpty()
                    ? safe->deck.getLastLoadError()
                    : juce::String ("The reason is not known."));

        safe->refresh();
    });

    refresh();
}

void DeckComponent::loadButtonClicked()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Load a track onto deck " + name,
                                                       juce::File(),
                                                       "*.wav;*.aiff;*.aif;*.flac;*.mp3;*.ogg");

    const auto browserFlags = juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles;

    fileChooser->launchAsync (browserFlags, [this] (const juce::FileChooser& chooser)
    {
        const auto file = chooser.getResult();

        if (file != juce::File())
            load (file);
    });
}

double DeckComponent::rangePercent() const
{
    switch (tempoRangeBox.getSelectedId())
    {
        case 2:  return 16.0;
        case 3:  return 50.0;
        default: return 8.0;
    }
}

void DeckComponent::applyTempoFromSlider()
{
    // The fader reads the way a DJ expects: up is faster.
    deck.setTempoRatio (1.0 + tempoSlider.getValue() * rangePercent() / 100.0);
    updateTempoReadout();
}

void DeckComponent::updateTempoReadout()
{
    const auto percent = (deck.getTempoRatio() - 1.0) * 100.0;

    // Follow the engine, so the hardware tempo fader moves the one on screen.
    if (const auto range = rangePercent(); range > 0.0 && ! tempoSlider.isMouseButtonDown())
        if (const auto position = percent / range; std::abs (tempoSlider.getValue() - position) > 1.0e-4)
            tempoSlider.setValue (juce::jlimit (-1.0, 1.0, position), juce::dontSendNotification);

    tempoLabel.setText (juce::String (percent, 1) + "%", juce::dontSendNotification);

    const auto bpm = engine.getEffectiveBpm (index);
    bpmLabel.setText (bpm > 0.0 ? juce::String (bpm, 1) + " BPM" : juce::String ("-- BPM"),
                      juce::dontSendNotification);
}

void DeckComponent::refresh()
{
    const auto loading = engine.isDeckLoading (index);
    const auto length = deck.getLengthSeconds();
    const auto position = deck.getPositionSeconds();

    if (auto analysis = deck.getAnalysis(); analysis != shownAnalysis)
    {
        shownAnalysis = std::move (analysis);
        scrollingWave.setAnalysis (shownAnalysis);
        overviewWave.setAnalysis (shownAnalysis);
    }

    platter.refresh (shownAnalysis);
    scrollingWave.setPosition (position, length);
    overviewWave.setPosition (position, length);
    scrollingWave.setCuePoint (deck.getCueSeconds());
    overviewWave.setCuePoint (deck.getCueSeconds());

    const auto title = loading ? juce::String ("analysing...")
                               : (deck.isLoaded() ? deck.getTrackTitle() : juce::String ("empty"));

    titleLabel.setText ("Deck " + name + "  |  " + title, juce::dontSendNotification);
    timeLabel.setText (formatTime (position) + " / " + formatTime (length), juce::dontSendNotification);

    playButton.setButtonText (deck.isPlaying() ? "Pause" : "Play");
    playButton.setColour (juce::TextButton::buttonColourId,
                          deck.isPlaying() ? accentColour.darker (0.4f) : juce::Colour (0xff2c2c34));

    syncButton.setEnabled (engine.getEffectiveBpm (index) > 0.0
                           && engine.getEffectiveBpm (1 - index) > 0.0);

    refreshLoopControls();

    keyLockButton.setColour (juce::TextButton::buttonColourId,
                             deck.isKeyLockEnabled() ? accentColour.darker (0.4f) : juce::Colour (0xff2c2c34));

    // The stem controls follow the engine: dead until a separation exists, and
    // the button says what it is doing while one is running.
    const auto separating = engine.isDeckSeparating (index);
    const auto haveStems = deck.hasStems();

    if (haveStems != stemsWereAvailable)
    {
        stemsWereAvailable = haveStems;

        for (auto& knob : stemControls.knobs)
        {
            knob.setEnabled (haveStems);
            knob.setValue (1.0, juce::dontSendNotification);
        }
    }

    for (int stem = 0; stem < numStems; ++stem)
    {
        auto& knob = stemControls.knobs[(size_t) stem];
        const auto gain = deck.getStemGain (static_cast<Stem> (stem));

        if (! knob.isMouseButtonDown() && std::abs (knob.getValue() - gain) > 1.0e-4)
            knob.setValue (gain, juce::dontSendNotification);
    }

    stemControls.separate.setEnabled (deck.isLoaded() && ! haveStems && ! separating
                                      && engine.getStemSeparator().isAvailable());
    stemControls.separate.setButtonText (separating
        ? juce::String (juce::roundToInt (engine.getSeparationProgress (index) * 100.0f)) + "%"
        : (haveStems ? juce::String ("Stems on") : juce::String ("Stems")));
    stemControls.separate.setColour (juce::TextButton::buttonColourId,
                                     haveStems ? accentColour.darker (0.4f) : juce::Colour (0xff2c2c34));

    updateTempoReadout();

    if (loading != wasLoading)
    {
        wasLoading = loading;
        loadButton.setEnabled (! loading);
    }
}

bool DeckComponent::isInterestedInDragSource (const SourceDetails& details)
{
    // The browser hands over a path; anything else is someone else's drag.
    return details.description.isString() && juce::File (details.description.toString()).existsAsFile();
}

void DeckComponent::itemDragEnter (const SourceDetails&)
{
    dragHovering = true;
    repaint();
}

void DeckComponent::itemDragExit (const SourceDetails&)
{
    dragHovering = false;
    repaint();
}

void DeckComponent::itemDropped (const SourceDetails& details)
{
    dragHovering = false;
    repaint();
    load (juce::File (details.description.toString()));
}

void DeckComponent::refreshLoopControls()
{
    const auto hasGrid = shownAnalysis != nullptr && shownAnalysis->hasTempo();
    const auto looping = deck.isLoopEnabled();
    const auto activeBeats = deck.getLoopBeats();

    for (int i = 0; i < numLoopButtons; ++i)
    {
        auto& button = loopButtons[(size_t) i];
        const auto beats = loopBeatsForSlot (i + 1);
        const auto isActive = looping && std::abs (activeBeats - beats) < 0.001;

        button.setEnabled (hasGrid);
        button.setColour (juce::TextButton::buttonColourId,
                          isActive ? loopColour.darker (0.2f) : juce::Colour (0xff2c2c34));
    }

    loopToggleButton.setEnabled (deck.hasLoop());
    loopToggleButton.setColour (juce::TextButton::buttonColourId,
                                looping ? loopColour.darker (0.2f) : juce::Colour (0xff2c2c34));

    loopHalveButton.setEnabled (deck.hasLoop());
    loopDoubleButton.setEnabled (deck.hasLoop());

    const auto start = deck.getLoopStartSeconds();
    const auto end = deck.getLoopEndSeconds();

    scrollingWave.setLoop (start, end, looping);
    overviewWave.setLoop (start, end, looping);
}

void DeckComponent::paint (juce::Graphics& g)
{
    g.setColour (panelColour);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);

    if (dragHovering)
    {
        g.setColour (accentColour);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 6.0f, 3.0f);
    }
}

void DeckComponent::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto header = area.removeFromTop (24);
    timeLabel.setBounds (header.removeFromRight (130));
    bpmLabel.setBounds (header.removeFromRight (90));
    titleLabel.setBounds (header);

    area.removeFromTop (8);
    scrollingWave.setBounds (area.removeFromTop (96));
    area.removeFromTop (4);
    overviewWave.setBounds (area.removeFromTop (34));

    // Loop row, directly under the waveform the loop is drawn on.
    area.removeFromTop (6);
    auto loopRow = area.removeFromTop (24);

    loopToggleButton.setBounds (loopRow.removeFromLeft (52).reduced (1));
    loopRow.removeFromLeft (4);
    loopHalveButton.setBounds (loopRow.removeFromLeft (30).reduced (1));
    loopDoubleButton.setBounds (loopRow.removeFromLeft (30).reduced (1));
    loopRow.removeFromLeft (6);

    const auto lengthWidth = juce::jmax (24, loopRow.getWidth() / numLoopButtons);

    for (auto& button : loopButtons)
        button.setBounds (loopRow.removeFromLeft (lengthWidth).reduced (1));

    // Then the stems: the button that asks for a separation, and a knob per part.
    area.removeFromTop (6);
    {
        auto stemRow = area.removeFromTop (46);
        stemControls.separate.setBounds (stemRow.removeFromLeft (74).reduced (0, 12));
        stemRow.removeFromLeft (6);

        const auto knobWidth = juce::jmax (36, stemRow.getWidth() / numStems);

        for (int stem = 0; stem < numStems; ++stem)
        {
            auto cell = stemRow.removeFromLeft (juce::jmin (knobWidth, stemRow.getWidth()));
            stemControls.labels[(size_t) stem].setBounds (cell.removeFromBottom (13));
            stemControls.knobs[(size_t) stem].setBounds (cell.reduced (2, 0));
        }
    }

    area.removeFromTop (8);

    auto tempoColumn = area.removeFromRight (74);
    tempoRangeBox.setBounds (tempoColumn.removeFromTop (24));
    tempoLabel.setBounds (tempoColumn.removeFromBottom (20));
    tempoSlider.setBounds (tempoColumn.reduced (12, 4));

    area.removeFromRight (10);

    // The platter takes whatever square it can get, leaving room beside it for
    // the transport column, and the window can be made small enough that it has
    // to give up rather than push the buttons off the panel.
    const auto transportWidth = 88;
    const auto platterSize = juce::jlimit (0,
                                           juce::jmax (0, area.getWidth() - transportWidth - 10),
                                           juce::jmin (240, area.getHeight()));

    if (platterSize > 60)
    {
        auto platterArea = area.removeFromLeft (platterSize);
        platter.setBounds (platterArea.removeFromTop (platterSize));
        area.removeFromLeft (10);
        platter.setVisible (true);
    }
    else
    {
        platter.setVisible (false);
    }

    auto transport = area.removeFromLeft (juce::jmin (transportWidth, area.getWidth()));

    // Spelled out, because the cue button is a different type to the others.
    const std::initializer_list<juce::Button*> transportButtons
    {
        &loadButton, &cueButton, &playButton, &syncButton, &keyLockButton
    };

    for (auto* button : transportButtons)
    {
        button->setBounds (transport.removeFromTop (36).reduced (0, 3));

        if (transport.getHeight() <= 0)
            break;
    }
}

} // namespace opendj
