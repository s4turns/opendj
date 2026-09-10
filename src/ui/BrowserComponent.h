/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "library/Library.h"
#include "library/LibraryScanner.h"

#include <functional>
#include <vector>

namespace opendj
{

/** The track browser: folders down the side, a searchable, sortable track list,
    and the scanner's progress.

    The list is a view of one library query, re-run when the search, folder or
    sort order changes and, throttled, whenever the scanner writes. Rows are
    dragged onto the decks, double-clicked to load, or picked with the
    controller's browse encoder and load buttons.
*/
class BrowserComponent final : public juce::Component,
                               private juce::TableListBoxModel,
                               private juce::Timer
{
public:
    BrowserComponent (Library& libraryToUse, LibraryScanner& scannerToUse);
    ~BrowserComponent() override;

    /** Called with a file and a deck when the user asks for a load. A deck of
        -1 means "whichever is free", which is what a double-click means. */
    std::function<void (const juce::File&, int deck)> onLoad;

    /** The highlighted track, or an invalid file. Safe from any thread, so the
        controller's load button can ask on the MIDI thread. */
    juce::File getSelectedFile() const;

    /** Moves the highlight by a number of rows, negative for up. Message thread. */
    void moveSelection (int rows);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    // Track table
    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell (juce::Graphics&, int row, int columnId, int width, int height, bool selected) override;
    void sortOrderChanged (int columnId, bool forwards) override;
    void cellDoubleClicked (int row, int columnId, const juce::MouseEvent&) override;
    void selectedRowsChanged (int lastRowSelected) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>& selectedRows) override;

    // Folder list. Its own model object, because ListBoxModel and
    // TableListBoxModel both declare getNumRows() and one class cannot answer
    // the two lists differently.
    struct FolderListModel final : public juce::ListBoxModel
    {
        explicit FolderListModel (BrowserComponent& ownerToUse) : owner (ownerToUse) {}

        int getNumRows() override;
        void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
        void listBoxItemClicked (int row, const juce::MouseEvent&) override;

        BrowserComponent& owner;
    };

    void timerCallback() override;

    void requestRefresh();
    void refreshNow();
    void refreshFolders();
    void refreshStatus();
    void addFolderClicked();
    void removeFolderClicked();
    juce::File folderForRow (int row) const;

    Library& library;
    LibraryScanner& scanner;

    juce::TextEditor searchBox;
    juce::TextButton addFolderButton { "Add folder..." };
    juce::TextButton removeFolderButton { "Remove" };
    juce::TextButton rescanButton { "Rescan" };
    juce::Label statusLabel;

    FolderListModel folderModel { *this };
    juce::ListBox folderList;
    juce::TableListBox table;

    std::vector<juce::File> folders;
    std::vector<TrackRecord> rows;
    LibraryQuery query;

    bool refreshPending = false;
    std::unique_ptr<juce::FileChooser> folderChooser;

    mutable juce::CriticalSection selectionLock;
    juce::File selectedFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BrowserComponent)
};

} // namespace opendj
