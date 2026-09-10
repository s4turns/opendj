/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/BrowserComponent.h"

namespace opendj
{

namespace
{
    enum Column { artist = 1, title, bpm, length, key, album };

    // A refresh no more often than this, however fast the scanner writes.
    constexpr int refreshIntervalMs = 400;

    const juce::Colour panelColour  { 0xff1c1c22 };
    const juce::Colour accentColour { 0xff35c2f0 };
    const juce::Colour rowColour    { 0xff202028 };
    const juce::Colour rowAltColour { 0xff24242c };

    juce::String formatLength (double seconds)
    {
        if (seconds <= 0.0)
            return {};

        const auto total = juce::roundToInt (seconds);
        return juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2);
    }
}

BrowserComponent::BrowserComponent (Library& libraryToUse, LibraryScanner& scannerToUse)
    : library (libraryToUse), scanner (scannerToUse)
{
    searchBox.setTextToShowWhenEmpty ("Search", juce::Colours::grey);
    searchBox.setColour (juce::TextEditor::backgroundColourId, rowColour);
    searchBox.onTextChange = [this] { query.search = searchBox.getText(); requestRefresh(); };
    searchBox.onEscapeKey = [this] { searchBox.clear(); };
    addAndMakeVisible (searchBox);

    addFolderButton.onClick = [this] { addFolderClicked(); };
    addAndMakeVisible (addFolderButton);

    removeFolderButton.onClick = [this] { removeFolderClicked(); };
    addAndMakeVisible (removeFolderButton);

    rescanButton.onClick = [this] { scanner.start(); };
    addAndMakeVisible (rescanButton);

    statusLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    statusLabel.setJustificationType (juce::Justification::centredRight);
    statusLabel.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (statusLabel);

    folderList.setModel (&folderModel);
    folderList.setRowHeight (22);
    folderList.setColour (juce::ListBox::backgroundColourId, rowColour);
    addAndMakeVisible (folderList);

    auto& header = table.getHeader();
    header.addColumn ("Artist", artist, 180, 80, -1, juce::TableHeaderComponent::defaultFlags);
    header.addColumn ("Title", title, 260, 80, -1, juce::TableHeaderComponent::defaultFlags);
    header.addColumn ("BPM", bpm, 60, 50, 80, juce::TableHeaderComponent::defaultFlags);
    header.addColumn ("Length", length, 60, 50, 80, juce::TableHeaderComponent::defaultFlags);
    header.addColumn ("Key", key, 50, 40, 80, juce::TableHeaderComponent::defaultFlags);
    header.addColumn ("Album", album, 180, 80, -1, juce::TableHeaderComponent::defaultFlags);
    header.setSortColumnId (artist, true);
    header.setStretchToFitActive (true);

    table.setModel (this);
    table.setRowHeight (22);
    table.setMultipleSelectionEnabled (false);
    table.setColour (juce::ListBox::backgroundColourId, rowColour);
    addAndMakeVisible (table);

    // The scanner writes from its own thread; all it may do here is ask.
    library.onChanged = [safe = juce::Component::SafePointer<BrowserComponent> (this)]
    {
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->requestRefresh(); });
    };

    refreshFolders();
    refreshNow();
    startTimer (refreshIntervalMs);
}

BrowserComponent::~BrowserComponent()
{
    stopTimer();
    library.onChanged = nullptr;
    table.setModel (nullptr);
    folderList.setModel (nullptr);
}

//==============================================================================
// Selection
//==============================================================================

juce::File BrowserComponent::getSelectedFile() const
{
    const juce::ScopedLock lock (selectionLock);
    return selectedFile;
}

void BrowserComponent::moveSelection (int rowsToMove)
{
    if (rows.empty())
        return;

    const auto current = table.getSelectedRow();
    const auto next = juce::jlimit (0, (int) rows.size() - 1, (current < 0 ? 0 : current) + rowsToMove);
    table.selectRow (next);
    table.scrollToEnsureRowIsOnscreen (next);
}

void BrowserComponent::selectedRowsChanged (int lastRowSelected)
{
    const juce::ScopedLock lock (selectionLock);
    selectedFile = juce::isPositiveAndBelow (lastRowSelected, (int) rows.size())
        ? rows[(size_t) lastRowSelected].file
        : juce::File();
}

//==============================================================================
// Refreshing
//==============================================================================

void BrowserComponent::requestRefresh()
{
    refreshPending = true;
}

void BrowserComponent::timerCallback()
{
    if (refreshPending)
        refreshNow();

    refreshStatus();
}

void BrowserComponent::refreshNow()
{
    refreshPending = false;

    const auto keep = getSelectedFile();
    rows = library.query (query);
    table.updateContent();

    // Keep the highlight on the same track through a re-sort or a scan.
    auto reselected = -1;

    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (rows[i].file == keep)
        {
            reselected = (int) i;
            break;
        }
    }

    if (reselected >= 0)
        table.selectRow (reselected, true, true);
    else
        selectedRowsChanged (-1);

    table.repaint();
}

void BrowserComponent::refreshFolders()
{
    folders = library.getFolders();
    folderList.updateContent();

    if (folderList.getSelectedRow() < 0)
        folderList.selectRow (0, true, true);

    folderList.repaint();
}

void BrowserComponent::refreshStatus()
{
    const auto progress = scanner.getProgress();
    juce::String status;

    if (progress.running && progress.scanning)
        status << "Scanning: " << progress.filesScanned << " files";
    else if (progress.running)
        status << "Analysing " << progress.tracksAnalysed + 1 << " of " << progress.tracksToAnalyse
               << ": " << progress.currentFile;
    else
        status << library.countTracks() << " tracks, " << library.countAnalysed() << " analysed";

    if (rows.size() < (size_t) library.countTracks() && ! progress.running)
        status = juce::String (rows.size()) + " shown  |  " + status;

    statusLabel.setText (status, juce::dontSendNotification);
}

//==============================================================================
// Folders
//==============================================================================

juce::File BrowserComponent::folderForRow (int row) const
{
    // Row 0 is every track; the library folders follow.
    return row > 0 && row - 1 < (int) folders.size() ? folders[(size_t) (row - 1)] : juce::File();
}

void BrowserComponent::addFolderClicked()
{
    folderChooser = std::make_unique<juce::FileChooser> ("Add a folder of music to the library");

    folderChooser->launchAsync (juce::FileBrowserComponent::openMode
                                | juce::FileBrowserComponent::canSelectDirectories,
                                [this] (const juce::FileChooser& chooser)
    {
        const auto folder = chooser.getResult();

        if (folder.isDirectory() && library.addFolder (folder))
        {
            refreshFolders();
            scanner.start();
        }
    });
}

void BrowserComponent::removeFolderClicked()
{
    const auto folder = folderForRow (folderList.getSelectedRow());

    if (folder == juce::File())
        return;

    // The folder stops being watched; the tracks and their beat grids stay,
    // since deleting a few thousand analyses over a misclick would be unkind.
    if (library.removeFolder (folder))
    {
        refreshFolders();
        folderList.selectRow (0);
        query.folder = juce::File();
        requestRefresh();
    }
}

int BrowserComponent::FolderListModel::getNumRows()
{
    return 1 + (int) owner.folders.size();
}

void BrowserComponent::FolderListModel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    g.fillAll (selected ? accentColour.withAlpha (0.25f) : rowColour);
    g.setColour (selected ? juce::Colours::white : juce::Colours::lightgrey);
    g.setFont (juce::FontOptions (13.0f));

    const auto text = row == 0 ? juce::String ("All tracks") : owner.folderForRow (row).getFileName();
    g.drawText (text, 8, 0, width - 12, height, juce::Justification::centredLeft, true);
}

void BrowserComponent::FolderListModel::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    owner.query.folder = owner.folderForRow (row);
    owner.requestRefresh();
}

int BrowserComponent::getNumRows()
{
    return (int) rows.size();
}

//==============================================================================
// Track table
//==============================================================================

void BrowserComponent::paintRowBackground (juce::Graphics& g, int row, int, int, bool selected)
{
    g.fillAll (selected ? accentColour.withAlpha (0.3f) : (row % 2 == 0 ? rowColour : rowAltColour));
}

void BrowserComponent::paintCell (juce::Graphics& g, int row, int columnId, int width, int height, bool)
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size()))
        return;

    const auto& record = rows[(size_t) row];
    juce::String text;
    auto justification = juce::Justification::centredLeft;

    switch (columnId)
    {
        case artist: text = record.artist; break;
        case title:  text = record.displayTitle(); break;
        case album:  text = record.album; break;
        case key:    text = record.key; break;
        case length: text = formatLength (record.durationSeconds); justification = juce::Justification::centredRight; break;
        case bpm:
            text = record.bpm > 0.0 ? juce::String (record.bpm, 1) : (record.analysed ? "--" : "");
            justification = juce::Justification::centredRight;
            break;
        default: break;
    }

    g.setColour (columnId == bpm && record.bpm > 0.0 ? accentColour : juce::Colours::lightgrey);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (text, 6, 0, width - 12, height, justification, true);
}

void BrowserComponent::sortOrderChanged (int columnId, bool forwards)
{
    switch (columnId)
    {
        case artist: query.sortBy = LibraryQuery::SortBy::artist; break;
        case title:  query.sortBy = LibraryQuery::SortBy::title; break;
        case album:  query.sortBy = LibraryQuery::SortBy::album; break;
        case bpm:    query.sortBy = LibraryQuery::SortBy::bpm; break;
        case length: query.sortBy = LibraryQuery::SortBy::duration; break;
        default:     query.sortBy = LibraryQuery::SortBy::artist; break;
    }

    query.ascending = forwards;
    refreshNow();
}

void BrowserComponent::cellDoubleClicked (int row, int, const juce::MouseEvent&)
{
    if (juce::isPositiveAndBelow (row, (int) rows.size()) && onLoad != nullptr)
        onLoad (rows[(size_t) row].file, -1);
}

juce::var BrowserComponent::getDragSourceDescription (const juce::SparseSet<int>& selectedRows)
{
    if (selectedRows.isEmpty())
        return {};

    const auto row = selectedRows[0];

    return juce::isPositiveAndBelow (row, (int) rows.size())
        ? juce::var (rows[(size_t) row].file.getFullPathName())
        : juce::var();
}

//==============================================================================

void BrowserComponent::paint (juce::Graphics& g)
{
    g.setColour (panelColour);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
}

void BrowserComponent::resized()
{
    auto area = getLocalBounds().reduced (8);

    auto toolbar = area.removeFromTop (26);
    addFolderButton.setBounds (toolbar.removeFromLeft (100));
    toolbar.removeFromLeft (6);
    removeFolderButton.setBounds (toolbar.removeFromLeft (70));
    toolbar.removeFromLeft (6);
    rescanButton.setBounds (toolbar.removeFromLeft (70));
    toolbar.removeFromLeft (12);
    statusLabel.setBounds (toolbar.removeFromRight (juce::jmin (420, toolbar.getWidth() / 2)));
    toolbar.removeFromRight (12);
    searchBox.setBounds (toolbar);

    area.removeFromTop (6);

    folderList.setBounds (area.removeFromLeft (juce::jlimit (120, 220, area.getWidth() / 5)));
    area.removeFromLeft (6);
    table.setBounds (area);
}

} // namespace opendj
