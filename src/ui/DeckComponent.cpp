/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/DeckComponent.h"
#include "ui/BeatGridEditor.h"

#include "ui/HotCueColours.h"

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

    swapButton.setTooltip ("Put the other deck on this side");
    swapButton.onClick = [this] { if (onSwapRequested != nullptr) onSwapRequested(); };
    addChildComponent (swapButton);

    timeLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    timeLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (timeLabel);

    bpmLabel.setColour (juce::Label::textColourId, accentColour);
    bpmLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    bpmLabel.setJustificationType (juce::Justification::centred);
    bpmLabel.setTooltip ("Click to edit the beat grid: tempo, downbeat and tap tempo");
    bpmLabel.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    bpmClick.onClick = [this]
    {
        if (deck.isLoaded())
            BeatGridEditor::showFor (deck, bpmLabel);
    };
    bpmLabel.addMouseListener (&bpmClick, false);
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

    slipButton.setTooltip ("Slip: the music keeps running under a scratch or a loop, "
                           "and letting go lands where it would have been");
    slipButton.onClick = [this] { deck.toggleSlip(); refresh(); };
    addAndMakeVisible (slipButton);

    keyLockButton.setTooltip ("Key lock: the tempo fader stops changing the pitch");
    keyLockButton.onClick = [this] { deck.toggleKeyLock(); refresh(); };
    addAndMakeVisible (keyLockButton);

    brakeButton.setTooltip ("Brake: slow to a stop, pitch falling. With Slip on, drops back in on time");
    brakeButton.onClick = [this] { deck.brake(); refresh(); };
    addAndMakeVisible (brakeButton);

    backspinButton.setTooltip ("Backspin: fling the record backwards to a stop. With Slip on, drops back in on time");
    backspinButton.onClick = [this] { deck.backspin(); refresh(); };
    addAndMakeVisible (backspinButton);

    reverseButton.setTooltip ("Reverse: play backwards until pressed again");
    reverseButton.onClick = [this] { deck.toggleReverse(); refresh(); };
    addAndMakeVisible (reverseButton);

    censorButton.setTooltip ("Censor: backwards while held, and letting go lands where the track would have been");
    censorButton.onPress = [this] { deck.setCensor (true); refresh(); };
    censorButton.onRelease = [this] { deck.setCensor (false); refresh(); };
    addAndMakeVisible (censorButton);

    echoOutButton.setTooltip ("Echo out: take the track away and leave its echo dying out. "
                              "Press again to bring it back");
    echoOutButton.onClick = [this]
    {
        auto& mixer = engine.getMixer();
        mixer.setChannelEchoOut (index, ! mixer.isChannelEchoOut (index));
        refresh();
    };
    addAndMakeVisible (echoOutButton);

    keyLabel.setJustificationType (juce::Justification::centred);
    keyLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    keyLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    keyLabel.setTooltip ("The key this deck is playing in. Click to match another deck's key");
    keyLabel.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    keyClick.onClick = [this] { showKeyMenu(); };
    keyLabel.addMouseListener (&keyClick, false);
    addAndMakeVisible (keyLabel);

    keyDownButton.setTooltip ("Down a semitone, without changing the tempo");
    keyDownButton.onClick = [this] { deck.setKeyShift (deck.getKeyShift() - 1); refresh(); };
    addAndMakeVisible (keyDownButton);

    keyUpButton.setTooltip ("Up a semitone, without changing the tempo");
    keyUpButton.onClick = [this] { deck.setKeyShift (deck.getKeyShift() + 1); refresh(); };
    addAndMakeVisible (keyUpButton);

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
                           + " beat loop that stays on, hold for a roll");

        button.onPress = [this, i, beats]
        {
            loopButtonPressedAt[(size_t) i] = juce::Time::getMillisecondCounter();

            // Pressing the length that is already looping turns it off.
            if (deck.isLoopEnabled() && std::abs (deck.getLoopBeats() - beats) < 0.001)
            {
                deck.toggleLoopBeats (beats);
                loopButtonPressedAt[(size_t) i] = 0;
                refresh();
                return;
            }

            // A roll only makes sense over a running deck; on a stopped one the
            // button behaves as a plain loop.
            if (deck.isPlaying() && deck.beginLoopRoll (beats))
                loopButtonRolling[(size_t) i] = true;
            else
                deck.setLoopBeats (beats);

            refresh();
        };

        button.onRelease = [this, i, beats]
        {
            const auto pressedAt = std::exchange (loopButtonPressedAt[(size_t) i], 0u);
            const auto held = pressedAt != 0 ? juce::Time::getMillisecondCounter() - pressedAt : 0;

            if (std::exchange (loopButtonRolling[(size_t) i], false))
            {
                deck.endLoopRoll();

                // Let go quickly and it was a click, not a hold: leave a loop
                // running rather than the moment of roll the press began.
                if (pressedAt != 0 && held < holdIsARollMs)
                    deck.setLoopBeats (beats);
            }

            refresh();
        };

        addAndMakeVisible (button);
    }

    for (int slot = 0; slot < Deck::numHotCues; ++slot)
    {
        auto& pad = hotCueButtons[(size_t) slot];
        pad.setButtonText (juce::String (slot + 1));
        // Setting or clearing a pad can change which names are in play (a reused
        // slot must not keep a cleared cue's name), so each asks for a reload.
        pad.onPress = [this, slot] { deck.hotCuePressed (slot); hotCueNamesDirty = true; refresh(); };
        pad.onClear = [this, slot] { deck.clearHotCue (slot); hotCueNamesDirty = true; refresh(); };
        pad.onRename = [this, slot] { beginHotCueRename (slot); };
        addAndMakeVisible (pad);
    }

    // Floats over the pad row while a cue is being named, hidden otherwise.
    hotCueNameEditor.setJustification (juce::Justification::centredLeft);
    hotCueNameEditor.setSelectAllWhenFocused (true);
    hotCueNameEditor.onReturnKey  = [this] { finishHotCueRename (true); };
    hotCueNameEditor.onEscapeKey  = [this] { finishHotCueRename (false); };
    hotCueNameEditor.onFocusLost  = [this] { finishHotCueRename (true); };
    addChildComponent (hotCueNameEditor);

    shownHotCues.fill (-2.0);   // nothing matches, so the first refresh paints them

    loopToggleButton.setTooltip ("Turn the current loop on or off");
    loopToggleButton.onClick = [this] { deck.toggleLoop(); refresh(); };
    addAndMakeVisible (loopToggleButton);

    loopHalveButton.setTooltip ("Halve the loop, keeping its start");
    loopHalveButton.onClick = [this] { deck.halveLoop(); refresh(); };
    addAndMakeVisible (loopHalveButton);

    loopDoubleButton.setTooltip ("Double the loop, keeping its start");
    loopDoubleButton.onClick = [this] { deck.doubleLoop(); refresh(); };
    addAndMakeVisible (loopDoubleButton);

    beatJumpBackButton.setTooltip ("Jump back by the beat jump size, staying in time");
    beatJumpBackButton.onClick = [this] { deck.beatJump (-loopBeatsForSlot (beatJumpSlot)); refresh(); };
    addAndMakeVisible (beatJumpBackButton);

    beatJumpForwardButton.setTooltip ("Jump forward by the beat jump size, staying in time");
    beatJumpForwardButton.onClick = [this] { deck.beatJump (loopBeatsForSlot (beatJumpSlot)); refresh(); };
    addAndMakeVisible (beatJumpForwardButton);

    // Steps 1, 2, 4, 8, 16, 32 beats and round again.
    beatJumpSizeButton.onClick = [this]
    {
        beatJumpSlot = beatJumpSlot >= 7 ? 2 : beatJumpSlot + 1;
        updateBeatJumpSize();
    };
    addAndMakeVisible (beatJumpSizeButton);
    updateBeatJumpSize();

    quantizeButton.setTooltip ("Quantize: the cue point, new hot cues and loop in and out "
                               "snap to the nearest beat");
    quantizeButton.onClick = [this] { deck.toggleQuantize(); refresh(); };
    addAndMakeVisible (quantizeButton);

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

    // Stems: a knob each, the button that asks for a separation, and the pads.
    updateSeparateTooltip();
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

            // The model is loaded by now, so the tooltip can name the device.
            safe->updateSeparateTooltip();
            safe->refresh();
        }, true);

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

    {
        static constexpr const char* padNames[numStemPads] { "Inst", "Acap", "Drum", "NoDr" };
        static constexpr const char* padTips[numStemPads]
        {
            "Instrumental: everything but the vocals, until pressed again",
            "Acapella: only the vocals, until pressed again",
            "Only the drums, until pressed again",
            "Everything but the drums, until pressed again"
        };

        for (int pad = 0; pad < numStemPads; ++pad)
        {
            auto& button = stemControls.pads[(size_t) pad];
            button.setButtonText (padNames[pad]);
            button.setTooltip (padTips[pad]);
            button.onClick = [this, pad] { deck.toggleStemPad (pad); refresh(); };
            addAndMakeVisible (button);
        }
    }

    // The stem effect: a knob for how much, and a button for on and off with the
    // rest of what there is to choose kept out of the way behind a right click.
    stemControls.fxAmount.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    stemControls.fxAmount.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    stemControls.fxAmount.setRange (0.0, 1.0, 0.0);
    stemControls.fxAmount.setValue (deck.getStemFxAmount(), juce::dontSendNotification);
    stemControls.fxAmount.onValueChange = [this]
    {
        deck.setStemFxAmount ((float) stemControls.fxAmount.getValue());
    };
    addAndMakeVisible (stemControls.fxAmount);

    stemControls.fxButton.onClick = [this] { deck.toggleStemFx(); refresh(); };
    stemControls.fxButton.onClear = [this] { showStemFxMenu(); };
    addAndMakeVisible (stemControls.fxButton);

    scrollingWave.onZoom = [this] (int steps) { if (onZoomRequested != nullptr) onZoomRequested (steps); };
    addAndMakeVisible (scrollingWave);

    overviewWave.onSeek = [this] (double seconds) { deck.seekToSeconds (seconds); refresh(); };
    addAndMakeVisible (overviewWave);

    refresh();
}

DeckComponent::~DeckComponent()
{
    bpmLabel.removeMouseListener (&bpmClick);
    keyLabel.removeMouseListener (&keyClick);
}

void DeckComponent::setKeyNotation (bool camelot)
{
    camelotKeys = camelot;
    updateKeyReadout();
}

void DeckComponent::updateKeyReadout()
{
    const auto key = deck.getEffectiveKey();
    const auto shift = deck.getKeyShift();

    auto text = key.isValid() ? (camelotKeys ? key.toCamelot() : key.toString()) : juce::String ("--");

    // The number says it has been moved, and how far, which the key alone does not.
    if (shift != 0)
        text << (shift > 0 ? " +" : " ") << shift;

    keyLabel.setText (text, juce::dontSendNotification);
    keyLabel.setColour (juce::Label::textColourId, shift != 0 ? accentColour : juce::Colours::lightgrey);
}

void DeckComponent::showStemFxMenu()
{
    // The ids are the choices, offset so each section has a range of its own.
    constexpr int targetIds = 100, typeIds = 200, beatsIds = 300;
    const auto type = deck.getStemFxType();

    juce::PopupMenu menu;
    menu.addSectionHeader ("Effect on");

    for (int stem = 0; stem < numStems; ++stem)
    {
        const juce::String stemName (toString (static_cast<Stem> (stem)));
        menu.addItem (targetIds + stem, stemName.substring (0, 1).toUpperCase() + stemName.substring (1),
                      true, static_cast<int> (deck.getStemFxTarget()) == stem);
    }

    menu.addSectionHeader ("Effect");

    for (int choice = 0; choice < StemEffect::numTypes; ++choice)
        menu.addItem (typeIds + choice, StemEffect::getTypeName (static_cast<StemEffect::Type> (choice)),
                      true, static_cast<int> (type) == choice);

    menu.addSectionHeader ("Echo length, in beats");

    for (int division = 0; division < StemEffect::numDivisions; ++division)
        menu.addItem (beatsIds + division, StemEffect::getDivisionName (division),
                      type == StemEffect::Type::echo, deck.getStemFxBeats() == division);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (stemControls.fxButton),
                        [safe = juce::Component::SafePointer<DeckComponent> (this)] (int result)
    {
        if (safe == nullptr || result <= 0)
            return;

        if (result >= beatsIds)
            safe->deck.setStemFxBeats (result - beatsIds);
        else if (result >= typeIds)
            safe->deck.setStemFxType (static_cast<StemEffect::Type> (result - typeIds));
        else
            safe->deck.setStemFxTarget (static_cast<Stem> (result - targetIds));

        safe->refresh();
    });
}

void DeckComponent::updateStemFxTooltip()
{
    const auto type = deck.getStemFxType();
    auto text = StemEffect::getTypeName (type) + " on the " + toString (deck.getStemFxTarget());

    if (type == StemEffect::Type::echo)
        text << ", " << StemEffect::getDivisionName (deck.getStemFxBeats()) << " beat";

    stemControls.fxButton.setTooltip (text + ".\nClick for on and off, right click to choose.");
}

void DeckComponent::showKeyMenu()
{
    if (! deck.isLoaded())
        return;

    const auto leader = engine.findSyncLeader (index);
    const auto hasKeys = deck.getEffectiveKey().isValid()
                      && leader >= 0 && engine.getDeck (leader).getEffectiveKey().isValid();

    juce::PopupMenu menu;
    constexpr int matchId = 1, resetId = 2;

    menu.addItem (matchId,
                  leader >= 0 ? "Match the key of deck " + juce::String::charToString ((juce::juce_wchar) ('A' + leader))
                              : juce::String ("Match the key of the playing deck"),
                  hasKeys);
    menu.addItem (resetId, "Back to the track's own key", deck.getKeyShift() != 0);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (keyLabel),
                        [safe = juce::Component::SafePointer<DeckComponent> (this), leader] (int result)
    {
        if (safe == nullptr)
            return;

        if (result == matchId)
            safe->engine.matchDeckKey (safe->index, leader);
        else if (result == resetId)
            safe->deck.setKeyShift (0);

        safe->refresh();
    });
}

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

void DeckComponent::setWaveformZoom (double seconds)
{
    scrollingWave.setWindowSeconds (seconds);
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
        hotCueNamesDirty = true;   // a new track brings its own cue names
    }

    platter.refresh (shownAnalysis);
    scrollingWave.setPosition (position, length);
    overviewWave.setPosition (position, length);
    scrollingWave.setCuePoint (deck.getCueSeconds());
    overviewWave.setCuePoint (deck.getCueSeconds());

    const auto title = loading ? juce::String ("analysing...")
                               : (deck.isLoaded() ? deck.getTrackTitle() : juce::String ("empty"));

    titleLabel.setText ("Deck " + name + "  |  " + title, juce::dontSendNotification);

    // What the level is doing to this track, for the one person who will wonder.
    if (shownAnalysis != nullptr && shownAnalysis->loudnessLufs < 0.0)
    {
        const auto db = juce::Decibels::gainToDecibels (deck.getTrackAutoGain());
        titleLabel.setTooltip (title + "\n" + juce::String (shownAnalysis->loudnessLufs, 1) + " LUFS. Auto gain "
                               + (deck.isAutoGainEnabled() ? "is giving it " : "would give it ")
                               + (db >= 0.0f ? "+" : "") + juce::String (db, 1) + " dB");
    }
    else
    {
        titleLabel.setTooltip ({});
    }
    timeLabel.setText (formatTime (position) + " / " + formatTime (length), juce::dontSendNotification);

    playButton.setButtonText (deck.isPlaying() ? "Pause" : "Play");
    playButton.setColour (juce::TextButton::buttonColourId,
                          deck.isPlaying() ? accentColour.darker (0.4f) : juce::Colour (0xff2c2c34));

    syncButton.setEnabled (engine.getEffectiveBpm (index) > 0.0
                           && engine.getEffectiveBpm (1 - index) > 0.0);

    refreshLoopControls();
    refreshHotCues();

    slipButton.setColour (juce::TextButton::buttonColourId,
                          deck.isSlipEnabled() ? loopColour.darker (0.2f)
                                               : juce::Colour (0xff2c2c34));

    keyLockButton.setColour (juce::TextButton::buttonColourId,
                             deck.isKeyLockEnabled() ? accentColour.darker (0.4f) : juce::Colour (0xff2c2c34));

    const auto lit = [] (juce::Button& button, bool on, juce::Colour colour)
    {
        button.setColour (juce::TextButton::buttonColourId, on ? colour : juce::Colour (0xff2c2c34));
    };

    lit (brakeButton, deck.isSpinningDown(), cueColour.darker (0.2f));
    lit (backspinButton, deck.isSpinningDown(), cueColour.darker (0.2f));
    lit (reverseButton, deck.isReverseEnabled(), cueColour.darker (0.2f));
    lit (censorButton, deck.isCensorHeld(), cueColour.darker (0.2f));
    lit (echoOutButton, engine.getMixer().isChannelEchoOut (index), accentColour.darker (0.4f));

    // The stem controls follow the engine: dead until a separation exists, and
    // the button says what it is doing while one is running. A knob a pad has
    // silenced goes dead too, as turning it could not be heard, and it keeps its
    // value for when the pad lets go. The deck puts the gains back to unity for
    // each new track, so a knob only has to show what the deck says.
    const auto separating = engine.isDeckSeparating (index);
    const auto haveStems = deck.hasStems();
    const auto stemPad = deck.getStemPad();

    for (int stem = 0; stem < numStems; ++stem)
    {
        auto& knob = stemControls.knobs[(size_t) stem];
        const auto gain = deck.getStemGain (static_cast<Stem> (stem));

        knob.setEnabled (haveStems && (stemPad < 0 || stemPadMasks[stemPad][stem] > 0.0f));

        if (! knob.isMouseButtonDown() && std::abs (knob.getValue() - gain) > 1.0e-4)
            knob.setValue (gain, juce::dontSendNotification);
    }

    for (int pad = 0; pad < numStemPads; ++pad)
    {
        auto& button = stemControls.pads[(size_t) pad];
        button.setEnabled (haveStems);
        lit (button, pad == stemPad, accentColour.darker (0.4f));
    }

    // Both stay usable without stems, dimmed: what the effect is and how much
    // of it are choices for the next track too, and the right click on the
    // button is the only way to them on screen. A press without stems does
    // nothing, as it does from a controller.
    stemControls.fxAmount.setAlpha (haveStems ? 1.0f : 0.5f);
    stemControls.fxButton.setAlpha (haveStems ? 1.0f : 0.5f);
    lit (stemControls.fxButton, deck.isStemFxEnabled(), accentColour.darker (0.4f));
    updateStemFxTooltip();

    if (! stemControls.fxAmount.isMouseButtonDown()
        && std::abs (stemControls.fxAmount.getValue() - deck.getStemFxAmount()) > 1.0e-4)
        stemControls.fxAmount.setValue (deck.getStemFxAmount(), juce::dontSendNotification);

    stemControls.separate.setEnabled (deck.isLoaded() && ! haveStems && ! separating
                                      && engine.getStemSeparator().isAvailable());
    stemControls.separate.setButtonText (separating
        ? juce::String (juce::roundToInt (engine.getSeparationProgress (index) * 100.0f)) + "%"
        : (haveStems ? juce::String ("Stems on") : juce::String ("Stems")));
    stemControls.separate.setColour (juce::TextButton::buttonColourId,
                                     haveStems ? accentColour.darker (0.4f) : juce::Colour (0xff2c2c34));

    updateTempoReadout();
    updateKeyReadout();

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

/** Says where separation runs, so a slow one can be told from one on the CPU. */
void DeckComponent::updateSeparateTooltip()
{
    stemControls.separate.setTooltip ("Separate this track into drums, bass, other and vocals.\n"
                                      + engine.getStemSeparator().getStatusDescription());
}

void DeckComponent::updateBeatJumpSize()
{
    const auto beats = juce::String (loopBeatsForSlot (beatJumpSlot), 0);
    beatJumpSizeButton.setButtonText (beats);
    beatJumpSizeButton.setTooltip ("Beat jump size: " + beats + (beats == "1" ? " beat" : " beats")
                                   + ". Click to change it");
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

    beatJumpBackButton.setEnabled (hasGrid);
    beatJumpForwardButton.setEnabled (hasGrid);
    beatJumpSizeButton.setEnabled (hasGrid);
    quantizeButton.setColour (juce::TextButton::buttonColourId,
                              deck.isQuantizeEnabled() ? accentColour.darker (0.4f)
                                                       : juce::Colour (0xff2c2c34));

    const auto start = deck.getLoopStartSeconds();
    const auto end = deck.getLoopEndSeconds();

    scrollingWave.setLoop (start, end, looping);
    overviewWave.setLoop (start, end, looping);
}

void DeckComponent::refreshHotCues()
{
    std::array<double, Deck::numHotCues> positions {};

    for (int slot = 0; slot < Deck::numHotCues; ++slot)
        positions[(size_t) slot] = deck.getHotCueSeconds (slot);

    // Pads only change when a cue does, so a full refresh at the frame rate
    // does not restyle eight buttons sixty times a second. Names can change
    // without a position moving, so a rename asks for one refresh by hand.
    if (positions == shownHotCues && ! hotCueNamesDirty)
        return;

    shownHotCues = positions;

    if (hotCueNamesDirty)
    {
        hotCueNames = engine.getAnalysisCache() != nullptr
                        ? engine.getAnalysisCache()->getHotCueNames (deck.getLoadedFile())
                        : noHotCueNames();
        hotCueNamesDirty = false;
    }

    std::vector<HotCueMarker> markers ((size_t) Deck::numHotCues);

    for (int slot = 0; slot < Deck::numHotCues; ++slot)
    {
        auto& pad = hotCueButtons[(size_t) slot];
        const auto seconds = positions[(size_t) slot];
        const auto isSet = seconds >= 0.0;
        const auto colour = hotCueColour (slot);

        pad.setColour (juce::TextButton::buttonColourId, isSet ? colour.darker (0.3f) : juce::Colour (0xff2c2c34));
        pad.setColour (juce::TextButton::textColourOffId, isSet ? colour.darker (0.3f).contrasting (0.8f)
                                                                     : colour.brighter (0.5f));
        pad.setTooltip (isSet ? "Hot cue " + juce::String (slot + 1) + " at " + formatTime (seconds)
                                    + ". Click to jump there, double-click to name it, right-click to clear"
                              : "Hot cue " + juce::String (slot + 1) + ": click to set it here");

        markers[(size_t) slot].seconds = seconds;
        markers[(size_t) slot].name = isSet ? hotCueNames[(size_t) slot] : juce::String();
    }

    scrollingWave.setHotCues (markers);
    overviewWave.setHotCues (markers);
}

void DeckComponent::beginHotCueRename (int slot)
{
    // Only a set cue can be named, and only when there is a library to keep the
    // name in. A rename already in progress is committed first.
    if (! juce::isPositiveAndBelow (slot, Deck::numHotCues)
        || ! deck.hasHotCue (slot)
        || engine.getAnalysisCache() == nullptr)
        return;

    if (editingHotCue >= 0)
        finishHotCueRename (true);

    editingHotCue = slot;
    hotCueNameEditor.setText (hotCueNames[(size_t) slot], false);
    hotCueNameEditor.setBounds (hotCueButtons.front().getBounds()
                                    .getUnion (hotCueButtons.back().getBounds()));
    hotCueNameEditor.setVisible (true);
    hotCueNameEditor.grabKeyboardFocus();
    hotCueNameEditor.selectAll();
}

void DeckComponent::finishHotCueRename (bool keep)
{
    const auto slot = editingHotCue;

    if (slot < 0)
        return;

    editingHotCue = -1;   // cleared first, so hiding does not re-enter through onFocusLost
    hotCueNameEditor.setVisible (false);

    if (! keep)
        return;

    if (auto* cache = engine.getAnalysisCache())
    {
        const auto name = hotCueNameEditor.getText().trim();
        cache->setHotCueName (deck.getLoadedFile(), slot, deck.getHotCueSeconds (slot), name);
        hotCueNames[(size_t) slot] = name;
        hotCueNamesDirty = true;
        refresh();
    }
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

void DeckComponent::setSwapTarget (const juce::String& letter)
{
    swapButton.setButtonText (letter);
    swapButton.setVisible (letter.isNotEmpty());
    resized();
}

void DeckComponent::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto header = area.removeFromTop (24);
    swapButton.setBounds (header.removeFromRight (34).reduced (1, 1));
    header.removeFromRight (4);
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

    // Hot cue pads, a row of their own under the loops.
    area.removeFromTop (4);
    {
        auto padRow = area.removeFromTop (24);

        // Beat jump and quantize at the far end: the pads only carry a number,
        // so they can spare the room better than the loop lengths can.
        quantizeButton.setBounds (padRow.removeFromRight (26).reduced (1));
        padRow.removeFromRight (4);
        beatJumpForwardButton.setBounds (padRow.removeFromRight (24).reduced (1));
        beatJumpSizeButton.setBounds (padRow.removeFromRight (28).reduced (1));
        beatJumpBackButton.setBounds (padRow.removeFromRight (24).reduced (1));
        padRow.removeFromRight (6);

        const auto padWidth = padRow.getWidth() / Deck::numHotCues;

        for (auto& pad : hotCueButtons)
            pad.setBounds (padRow.removeFromLeft (padWidth).reduced (1));

        // Keep the name editor over the row if a rename is open during a resize.
        if (hotCueNameEditor.isVisible())
            hotCueNameEditor.setBounds (hotCueButtons.front().getBounds()
                                            .getUnion (hotCueButtons.back().getBounds()));
    }

    // The performance effects, shared out evenly.
    area.removeFromTop (4);
    {
        auto effectRow = area.removeFromTop (24);

        // The key at the far end, under the beat jump and away from anything
        // that moves the playhead.
        keyUpButton.setBounds (effectRow.removeFromRight (22).reduced (1));
        keyLabel.setBounds (effectRow.removeFromRight (50));
        keyDownButton.setBounds (effectRow.removeFromRight (22).reduced (1));
        effectRow.removeFromRight (4);

        const auto buttons = { static_cast<juce::Button*> (&brakeButton), static_cast<juce::Button*> (&backspinButton),
                               static_cast<juce::Button*> (&reverseButton), static_cast<juce::Button*> (&censorButton),
                               static_cast<juce::Button*> (&echoOutButton) };
        const auto width = effectRow.getWidth() / (int) buttons.size();

        for (auto* button : buttons)
            button->setBounds (effectRow.removeFromLeft (width).reduced (1));
    }

    // Then the stems: the button that asks for a separation, a knob per part, the
    // effect's knob and button, and two rows of pads at the far end.
    area.removeFromTop (6);
    {
        auto stemRow = area.removeFromTop (46);
        stemControls.separate.setBounds (stemRow.removeFromLeft (74).reduced (0, 12));
        stemRow.removeFromLeft (6);

        constexpr int padsPerRow = numStemPads / 2, padWidth = 40;
        auto padGrid = stemRow.removeFromRight (padWidth * padsPerRow);
        const auto padHeight = padGrid.getHeight() / 2;

        for (int pad = 0; pad < numStemPads; ++pad)
            stemControls.pads[(size_t) pad].setBounds (
                juce::Rectangle<int> (padGrid.getX() + (pad % padsPerRow) * padWidth,
                                      padGrid.getY() + (pad / padsPerRow) * padHeight,
                                      padWidth, padHeight).reduced (1));

        stemRow.removeFromRight (4);

        // The row shares out into one more cell than there are knobs, and the
        // last goes to the effect.
        const auto knobWidth = juce::jmax (36, stemRow.getWidth() / (numStems + 1));

        for (int stem = 0; stem < numStems; ++stem)
        {
            auto cell = stemRow.removeFromLeft (juce::jmin (knobWidth, stemRow.getWidth()));
            stemControls.labels[(size_t) stem].setBounds (cell.removeFromBottom (13));
            stemControls.knobs[(size_t) stem].setBounds (cell.reduced (2, 0));
        }

        stemControls.fxButton.setBounds (stemRow.removeFromBottom (16).reduced (1, 0));
        stemControls.fxAmount.setBounds (stemRow.reduced (2, 0));
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

    // Two columns where there is room for them, since six buttons in one do not
    // fit beside a platter this size. Load, Cue and Play stay down the left,
    // where they have always been.
    auto transport = area;

    const std::array<juce::Button*, 6> transportButtons
    {
        &loadButton, &cueButton, &playButton, &syncButton, &keyLockButton, &slipButton
    };

    constexpr int columnGap = 4, narrowestColumn = 72;

    const auto columns = transport.getWidth() >= narrowestColumn * 2 + columnGap ? 2 : 1;
    const auto rows = (int) transportButtons.size() / columns;
    const auto rowHeight = juce::jmin (36, transport.getHeight() / juce::jmax (1, rows));
    const auto columnWidth = juce::jmin (transportWidth, (transport.getWidth() - (columns - 1) * columnGap) / columns);

    for (int i = 0; i < (int) transportButtons.size(); ++i)
        transportButtons[(size_t) i]->setBounds (transport.getX() + (i / rows) * (columnWidth + columnGap),
                                                 transport.getY() + (i % rows) * rowHeight + 3,
                                                 columnWidth,
                                                 juce::jmax (0, rowHeight - 6));
}

} // namespace opendj
