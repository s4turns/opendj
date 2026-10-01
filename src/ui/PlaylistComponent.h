/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/AutoMix.h"
#include "library/Library.h"

#include <functional>
#include <vector>

namespace opendj
{

/** Build a playlist from the library and let the auto crossfader play it.

    Tracks are added from the browser's selection or by dragging them here.
    "Play" starts the auto crossfader from the highlighted row: it keeps the
    playlist going, mixing each track into the next, which is what keeps a
    live stream moving with nobody at the decks. */
class PlaylistComponent final : public juce::Component,
                                public juce::DragAndDropTarget,
                                private juce::ListBoxModel,
                                private juce::Timer
{
public:
    PlaylistComponent (Library& libraryToUse, AutoMix& autoMixToControl,
                       std::function<juce::File()> selectedFileFromBrowser);
    ~PlaylistComponent() override;

    void resized() override;
    void paint (juce::Graphics& g) override;

    /** Re-reads the playlists after something outside changed them. */
    void refresh() { reloadPlaylists(); pushQueue(); }

    // Dragging a track from the browser onto the list.
    bool isInterestedInDragSource (const SourceDetails& details) override;
    void itemDropped (const SourceDetails& details) override;

private:
    // ListBoxModel
    int getNumRows() override { return (int) files.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;

    void timerCallback() override;

    void reloadPlaylists (juce::int64 select = 0);
    void reloadTracks();
    juce::int64 currentPlaylistId() const;

    void newPlaylist();
    void deletePlaylist();
    void addFiles (const std::vector<juce::File>& added);
    void removeSelected();
    void moveSelected (int delta);
    void play (int fromRow);
    void pushQueue();
    void refreshButtons();

    Library& library;
    AutoMix& autoMix;
    std::function<juce::File()> selectedFile;

    juce::ComboBox playlistBox;
    juce::TextButton newButton { "New" }, deleteButton { "Delete" };
    juce::TextButton addButton { "Add selected" }, removeButton { "Remove" };
    juce::TextButton upButton { "Up" }, downButton { "Down" };
    juce::TextButton playButton { "Play" }, skipButton { "Next now" };
    juce::ToggleButton loopButton { "Loop" };
    juce::Label fadeLabel { {}, "Fade (s)" };
    juce::Slider fadeSlider;
    juce::Label statusLabel;
    juce::ListBox list { {}, this };

    std::vector<juce::File> files;
    std::vector<juce::String> names;

    int shownCurrent = -2, shownNext = -2;
    bool shownRunning = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaylistComponent)
};

} // namespace opendj
