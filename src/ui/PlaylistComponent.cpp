/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/PlaylistComponent.h"

namespace opendj
{

namespace
{
    const juce::Colour accentColour { 0xff35c2f0 };
    const juce::Colour rowColour    { 0xff1c1c22 };

    juce::String nameFor (const Library& library, const juce::File& file)
    {
        if (const auto record = library.findTrack (file))
            return record->artist.isNotEmpty() ? record->artist + " - " + record->displayTitle()
                                               : record->displayTitle();

        return file.getFileNameWithoutExtension();
    }
}

PlaylistComponent::PlaylistComponent (Library& libraryToUse, AutoMix& autoMixToControl,
                                      std::function<juce::File()> selectedFileFromBrowser)
    : library (libraryToUse), autoMix (autoMixToControl), selectedFile (std::move (selectedFileFromBrowser))
{
    playlistBox.setTextWhenNothingSelected ("No playlist: press New");
    playlistBox.onChange = [this] { reloadTracks(); pushQueue(); };
    addAndMakeVisible (playlistBox);

    newButton.onClick = [this] { newPlaylist(); };
    deleteButton.onClick = [this] { deletePlaylist(); };
    addButton.onClick = [this]
    {
        if (const auto file = selectedFile ? selectedFile() : juce::File(); file != juce::File())
            addFiles ({ file });
    };
    removeButton.onClick = [this] { removeSelected(); };
    upButton.onClick = [this] { moveSelected (-1); };
    downButton.onClick = [this] { moveSelected (1); };

    playButton.onClick = [this]
    {
        if (autoMix.isRunning())
            autoMix.stop();
        else
            play (juce::jmax (0, list.getSelectedRow()));
    };
    skipButton.onClick = [this] { autoMix.skip(); };

    loopButton.setToggleState (autoMix.isLooping(), juce::dontSendNotification);
    loopButton.onClick = [this] { autoMix.setLoop (loopButton.getToggleState()); };

    fadeSlider.setRange (2.0, 30.0, 1.0);
    fadeSlider.setValue (autoMix.getFadeSeconds(), juce::dontSendNotification);
    fadeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    fadeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 40, 20);
    fadeSlider.onValueChange = [this] { autoMix.setFadeSeconds (fadeSlider.getValue()); };

    statusLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    statusLabel.setFont (juce::FontOptions (12.0f));

    list.setRowHeight (22);
    list.setColour (juce::ListBox::backgroundColourId, rowColour);

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &newButton, &deleteButton, &addButton, &removeButton, &upButton, &downButton,
             &playButton, &skipButton, &loopButton, &fadeLabel, &fadeSlider, &statusLabel, &list })
        addAndMakeVisible (c);

    reloadPlaylists();
    startTimerHz (4);
}

PlaylistComponent::~PlaylistComponent()
{
    stopTimer();
}

juce::int64 PlaylistComponent::currentPlaylistId() const
{
    return (juce::int64) playlistBox.getSelectedId() - 1;
}

void PlaylistComponent::reloadPlaylists (juce::int64 select)
{
    const auto previous = select != 0 ? select : currentPlaylistId();
    const auto lists = library.getPlaylists();

    playlistBox.clear (juce::dontSendNotification);

    for (const auto& p : lists)
        playlistBox.addItem (p.name + " (" + juce::String (p.numTracks) + ")", (int) (p.id + 1));

    if (playlistBox.indexOfItemId ((int) (previous + 1)) >= 0)
        playlistBox.setSelectedId ((int) (previous + 1), juce::dontSendNotification);
    else if (! lists.empty())
        playlistBox.setSelectedId ((int) (lists.front().id + 1), juce::dontSendNotification);

    reloadTracks();
    refreshButtons();
}

void PlaylistComponent::reloadTracks()
{
    files.clear();
    names.clear();

    if (const auto id = currentPlaylistId(); id > 0 || playlistBox.getSelectedId() > 0)
        files = library.getPlaylistFiles (id);

    for (const auto& file : files)
        names.push_back (nameFor (library, file));

    list.updateContent();
    list.repaint();
    refreshButtons();
}

void PlaylistComponent::pushQueue()
{
    // While the auto crossfader runs, what it plays follows what is shown.
    if (autoMix.isRunning())
        autoMix.setQueue (files);
}

void PlaylistComponent::refreshButtons()
{
    const auto haveList = playlistBox.getSelectedId() > 0;
    deleteButton.setEnabled (haveList);
    addButton.setEnabled (haveList);
    removeButton.setEnabled (haveList && list.getSelectedRow() >= 0);
    upButton.setEnabled (haveList && list.getSelectedRow() > 0);
    downButton.setEnabled (haveList && list.getSelectedRow() >= 0 && list.getSelectedRow() + 1 < (int) files.size());
    playButton.setEnabled (autoMix.isRunning() || ! files.empty());
    playButton.setButtonText (autoMix.isRunning() ? "Stop" : "Play");
    skipButton.setEnabled (autoMix.isRunning());
}

void PlaylistComponent::newPlaylist()
{
    auto* window = new juce::AlertWindow ("New playlist", "Name the playlist.", juce::MessageBoxIconType::NoIcon);
    window->addTextEditor ("name", "", "");
    window->addButton ("Create", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    window->enterModalState (true, juce::ModalCallbackFunction::create ([this, window] (int result)
    {
        if (result != 1)
            return;

        // `window` is deleted after this callback returns, so the text is read now.
        if (const auto id = library.createPlaylist (window->getTextEditorContents ("name")); id != 0)
            reloadPlaylists (id);
    }), true);
}

void PlaylistComponent::deletePlaylist()
{
    if (playlistBox.getSelectedId() <= 0)
        return;

    library.deletePlaylist (currentPlaylistId());
    reloadPlaylists (-1);
}

void PlaylistComponent::addFiles (const std::vector<juce::File>& added)
{
    if (playlistBox.getSelectedId() <= 0 || added.empty())
        return;

    const auto id = currentPlaylistId();
    library.addToPlaylist (id, added);
    reloadPlaylists (id);
    pushQueue();
}

void PlaylistComponent::removeSelected()
{
    const auto row = list.getSelectedRow();

    if (row < 0 || playlistBox.getSelectedId() <= 0)
        return;

    const auto id = currentPlaylistId();
    library.removeFromPlaylist (id, row);
    reloadPlaylists (id);
    list.selectRow (juce::jmin (row, (int) files.size() - 1));
    pushQueue();
}

void PlaylistComponent::moveSelected (int delta)
{
    const auto row = list.getSelectedRow();

    if (row < 0 || playlistBox.getSelectedId() <= 0)
        return;

    const auto id = currentPlaylistId();

    if (library.movePlaylistTrack (id, row, row + delta))
    {
        reloadPlaylists (id);
        list.selectRow (row + delta);
        pushQueue();
    }
}

void PlaylistComponent::play (int fromRow)
{
    if (files.empty())
        return;

    autoMix.setQueue (files);
    autoMix.start (juce::jlimit (0, (int) files.size() - 1, fromRow));
}

bool PlaylistComponent::isInterestedInDragSource (const SourceDetails& details)
{
    return playlistBox.getSelectedId() > 0
        && (details.description.isString() || details.description.isArray());
}

void PlaylistComponent::itemDropped (const SourceDetails& details)
{
    std::vector<juce::File> dropped;

    if (details.description.isString())
        dropped.emplace_back (details.description.toString());
    else if (const auto* list = details.description.getArray())
        for (const auto& path : *list)
            dropped.emplace_back (path.toString());

    addFiles (dropped);
}

void PlaylistComponent::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    play (row);
}

void PlaylistComponent::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, (int) names.size()))
        return;

    const auto current = autoMix.isRunning() && row == autoMix.getCurrentIndex();
    const auto next = autoMix.isRunning() && row == autoMix.getNextIndex();

    if (selected)
        g.fillAll (juce::Colour (0xff2c3a44));

    g.setColour (current ? accentColour : next ? juce::Colours::white : juce::Colours::lightgrey);
    g.setFont (juce::FontOptions (13.0f, current ? juce::Font::bold : juce::Font::plain));

    const auto marker = current ? juce::String ("Playing   ") : next ? juce::String ("Next   ") : juce::String();
    g.drawText (juce::String (row + 1) + ".  " + marker + names[(size_t) row],
                juce::Rectangle<int> (8, 0, width - 12, height), juce::Justification::centredLeft, true);
}

void PlaylistComponent::timerCallback()
{
    const auto current = autoMix.isRunning() ? autoMix.getCurrentIndex() : -1;
    const auto next = autoMix.isRunning() ? autoMix.getNextIndex() : -1;
    const auto running = autoMix.isRunning();

    if (current == shownCurrent && next == shownNext && running == shownRunning)
        return;

    shownCurrent = current;
    shownNext = next;
    shownRunning = running;
    list.repaint();
    refreshButtons();

    statusLabel.setText (! running ? juce::String()
                         : autoMix.isFading() ? "Mixing into the next track"
                                              : "Auto crossfade is running",
                         juce::dontSendNotification);
}

void PlaylistComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff15151a));
}

void PlaylistComponent::resized()
{
    auto area = getLocalBounds().reduced (6);

    auto top = area.removeFromTop (26);
    deleteButton.setBounds (top.removeFromRight (64));
    top.removeFromRight (4);
    newButton.setBounds (top.removeFromRight (56));
    top.removeFromRight (6);
    playlistBox.setBounds (top);
    area.removeFromTop (4);

    auto second = area.removeFromTop (26);
    addButton.setBounds (second.removeFromLeft (100));
    second.removeFromLeft (4);
    removeButton.setBounds (second.removeFromLeft (70));
    second.removeFromLeft (4);
    upButton.setBounds (second.removeFromLeft (44));
    second.removeFromLeft (4);
    downButton.setBounds (second.removeFromLeft (52));
    second.removeFromLeft (16);
    playButton.setBounds (second.removeFromLeft (70));
    second.removeFromLeft (4);
    skipButton.setBounds (second.removeFromLeft (84));
    second.removeFromLeft (12);
    loopButton.setBounds (second.removeFromLeft (64));
    fadeLabel.setBounds (second.removeFromLeft (64));
    fadeSlider.setBounds (second.removeFromLeft (200));
    second.removeFromLeft (8);
    statusLabel.setBounds (second);
    area.removeFromTop (4);

    list.setBounds (area);
}

} // namespace opendj
