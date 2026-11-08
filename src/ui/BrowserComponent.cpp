/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/BrowserComponent.h"

#include "library/RekordboxImport.h"

#include <algorithm>

namespace opendj
{

namespace
{
    enum Column { artist = 1, title, bpm, length, key, album, rating, comment };

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

    importButton.setTooltip ("Import a rekordbox collection: File > Export Collection in xml format");
    importButton.onClick = [this] { importClicked(); };
    addAndMakeVisible (importButton);

    removeFolderButton.onClick = [this] { removeFolderClicked(); };
    addAndMakeVisible (removeFolderButton);

    camelotButton.setTooltip ("Show keys as Camelot numbers rather than notes. Keys in green mix with what is playing");
    camelotButton.setClickingTogglesState (true);
    camelotButton.setColour (juce::TextButton::buttonOnColourId, accentColour.darker (0.4f));
    camelotButton.onClick = [this]
    {
        setKeyNotation (camelotButton.getToggleState());

        if (onKeyNotationChanged != nullptr)
            onKeyNotationChanged (camelotKeys);
    };
    addAndMakeVisible (camelotButton);

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

    // Not sortable: the library has no index for them, and they are mostly
    // empty until a collection is imported.
    const auto unsortable = juce::TableHeaderComponent::defaultFlags & ~juce::TableHeaderComponent::sortable;
    header.addColumn ("Rating", rating, 70, 50, 90, unsortable);
    header.addColumn ("Comment", comment, 160, 60, -1, unsortable);
    header.setSortColumnId (artist, true);
    header.setStretchToFitActive (true);

    table.setModel (this);
    table.setRowHeight (22);
    table.setMultipleSelectionEnabled (true);
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

std::vector<juce::File> BrowserComponent::getSelectedFiles() const
{
    std::vector<juce::File> files;
    const auto selected = table.getSelectedRows();

    for (int i = 0; i < selected.size(); ++i)
        if (const auto r = selected[i]; juce::isPositiveAndBelow (r, (int) rows.size()))
            files.push_back (rows[(size_t) r].file);

    return files;
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

    if (sortByKey)
    {
        // Unknown keys stay at the end in both directions: nobody sorting by key
        // is looking for the tracks that have none.
        std::vector<std::pair<int, size_t>> order;
        order.reserve (rows.size());

        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto wheel = MusicalKey::fromString (rows[i].key).wheelOrder();
            order.emplace_back (wheel < 0 ? 1000 : (keySortForwards ? wheel : 100 - wheel), i);
        }

        std::stable_sort (order.begin(), order.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });

        std::vector<TrackRecord> sorted;
        sorted.reserve (rows.size());

        for (const auto& entry : order)
            sorted.push_back (std::move (rows[entry.second]));

        rows = std::move (sorted);
    }

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

    if (stemStatus != nullptr)
        if (const auto stems = stemStatus(); stems.isNotEmpty())
            status = stems + "  |  " + status;

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

void BrowserComponent::importClicked()
{
    importChooser = std::make_unique<juce::FileChooser> ("Import a rekordbox collection (xml)",
                                                         juce::File(), "*.xml");

    importChooser->launchAsync (juce::FileBrowserComponent::openMode
                                | juce::FileBrowserComponent::canSelectFiles,
                                [this] (const juce::FileChooser& chooser)
    {
        const auto xml = chooser.getResult();

        if (! xml.existsAsFile())
            return;

        importButton.setEnabled (false);
        importButton.setButtonText ("Importing...");

        // A collection of thousands of tracks takes a while to write, so it is
        // done away from the message thread and reported when it is done.
        juce::Thread::launch ([safe = juce::Component::SafePointer<BrowserComponent> (this), &target = library, xml]
        {
            const auto collection = RekordboxImport::read (xml);
            const auto summary = collection.error.isEmpty() ? RekordboxImport::importInto (target, collection)
                                                            : RekordboxImportSummary();

            juce::MessageManager::callAsync ([safe, collection, summary]
            {
                if (safe == nullptr)
                    return;

                safe->importButton.setEnabled (true);
                safe->importButton.setButtonText ("Import...");
                safe->refreshNow();

                if (safe->onPlaylistsChanged != nullptr)
                    safe->onPlaylistsChanged();

                juce::AlertWindow::showMessageBoxAsync (
                    collection.error.isEmpty() ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
                    "Import rekordbox collection",
                    collection.error.isEmpty() ? summary.describe() : collection.error);
            });
        });
    });
}

void BrowserComponent::removeFolderClicked()
{
    const auto folder = folderForRow (folderList.getSelectedRow());

    if (folder == juce::File())
        return;

    const auto count = library.countTracksOnlyIn (folder);
    const auto tracks = count == 1 ? juce::String ("1 track") : juce::String (count) + " tracks";

    // The tracks go with the folder, and their analyses with them, so this asks first.
    auto* window = new juce::AlertWindow ("Remove folder",
                                          "Remove \"" + folder.getFileName() + "\" and its " + tracks
                                              + " from the library?\n\nThe files stay on disk. Playlists keep their entries.",
                                          juce::MessageBoxIconType::QuestionIcon);
    window->addButton ("Remove", 1);
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    window->enterModalState (true, juce::ModalCallbackFunction::create ([this, folder] (int result)
    {
        if (result != 1 || ! library.removeFolder (folder))
            return;

        refreshFolders();
        folderList.selectRow (0);
        query.folder = juce::File();
        requestRefresh();
    }), true);
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

void BrowserComponent::paintRowBackground (juce::Graphics& g, int row, int, int height, bool selected)
{
    g.fillAll (selected ? accentColour.withAlpha (0.3f) : (row % 2 == 0 ? rowColour : rowAltColour));

    // A track's colour, as rekordbox and Serato show it: a stripe at the start.
    if (juce::isPositiveAndBelow (row, (int) rows.size()) && rows[(size_t) row].colour != 0)
    {
        g.setColour (juce::Colour (rows[(size_t) row].colour));
        g.fillRect (0, 1, 4, height - 2);
    }
}

void BrowserComponent::paintCell (juce::Graphics& g, int row, int columnId, int width, int height, bool)
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size()))
        return;

    const auto& record = rows[(size_t) row];
    juce::String text;
    auto justification = juce::Justification::centredLeft;
    auto colour = juce::Colours::lightgrey;

    switch (columnId)
    {
        case artist: text = record.artist; break;
        case title:  text = record.displayTitle(); break;
        case album:  text = record.album; break;
        case key:
        {
            // A key the parser cannot read is shown as it was written, rather
            // than hiding what a file's tags actually say.
            const auto parsed = MusicalKey::fromString (record.key);
            text = ! parsed.isValid() ? record.key : (camelotKeys ? parsed.toCamelot() : parsed.toString());

            switch (parsed.harmonyWith (harmonicReference))
            {
                case MusicalKey::Harmony::same:      colour = juce::Colour (0xff4ad991); break;
                case MusicalKey::Harmony::relative:
                case MusicalKey::Harmony::neighbour: colour = juce::Colour (0xffa6e8c6); break;
                case MusicalKey::Harmony::clash:     break;
            }
            break;
        }
        case comment: text = record.comment; break;
        case rating:  text = juce::String::repeatedString (juce::String::fromUTF8 ("\xe2\x98\x85"), record.rating); break;
        case length: text = formatLength (record.durationSeconds); justification = juce::Justification::centredRight; break;
        case bpm:
            text = record.bpm > 0.0 ? juce::String (record.bpm, 1) : (record.analysed ? "--" : "");
            justification = juce::Justification::centredRight;
            break;
        default: break;
    }

    g.setColour (columnId == bpm && record.bpm > 0.0 ? accentColour : colour);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (text, 6, 0, width - 12, height, justification, true);
}

void BrowserComponent::setKeyNotation (bool camelot)
{
    camelotKeys = camelot;
    camelotButton.setToggleState (camelot, juce::dontSendNotification);
    table.repaint();
}

void BrowserComponent::setHarmonicReference (const MusicalKey& reference)
{
    if (reference.tonic == harmonicReference.tonic && reference.mode == harmonicReference.mode)
        return;

    harmonicReference = reference;
    table.repaint();
}

juce::String BrowserComponent::getCellTooltip (int row, int columnId)
{
    if (columnId != key || ! juce::isPositiveAndBelow (row, (int) rows.size()) || ! harmonicReference.isValid())
        return {};

    const auto parsed = MusicalKey::fromString (rows[(size_t) row].key);
    const auto playing = camelotKeys ? harmonicReference.toCamelot() : harmonicReference.toString();

    switch (parsed.harmonyWith (harmonicReference))
    {
        case MusicalKey::Harmony::same:      return "The same key as what is playing, " + playing;
        case MusicalKey::Harmony::relative:  return "The relative of what is playing, " + playing + ": the same notes";
        case MusicalKey::Harmony::neighbour: return "One step round the wheel from what is playing, " + playing;
        case MusicalKey::Harmony::clash:     break;
    }

    return {};
}

void BrowserComponent::sortOrderChanged (int columnId, bool forwards)
{
    sortByKey = columnId == key;
    keySortForwards = forwards;

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

void BrowserComponent::cellClicked (int row, int, const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu() || ! juce::isPositiveAndBelow (row, (int) rows.size()))
        return;

    // Right-clicking inside a selection is about the whole selection; outside
    // it, about the row under the pointer alone, which becomes the selection.
    if (! table.isRowSelected (row))
        table.selectRow (row);

    const auto chosenFiles = getSelectedFiles();

    if (chosenFiles.empty())
        return;

    const auto count = (int) chosenFiles.size();

    // Item ids are the playlist's id plus one, so zero stays "nothing chosen".
    juce::PopupMenu playlists;
    const auto lists = library.getPlaylists();

    for (const auto& playlist : lists)
        playlists.addItem ((int) (playlist.id + 1), playlist.name + " (" + juce::String (playlist.numTracks) + ")");

    if (lists.empty())
        playlists.addItem (-1, "No playlists yet: make one in the Playlists tab", false, false);

    // Straight onto a chosen deck, numbered the way the mixer labels them.
    // A deck takes one track, so this is only there for a single selection.
    juce::PopupMenu decks;

    for (int deck = 0; deck < 4; ++deck)
        decks.addItem ("Deck " + juce::String (deck + 1),
                       [safe = juce::Component::SafePointer<BrowserComponent> (this), file = chosenFiles.front(), deck]
                       {
                           if (safe != nullptr && safe->onLoad != nullptr)
                               safe->onLoad (file, deck);
                       });

    juce::PopupMenu menu;

    if (count == 1)
        menu.addSubMenu ("Load to deck", decks);

    menu.addSubMenu (count == 1 ? juce::String ("Add to playlist")
                                : "Add " + juce::String (count) + " tracks to playlist", playlists);

    stemPrepareMenu.addTo (menu, count == 1 ? juce::String ("Prepare stems")
                                            : "Prepare stems for " + juce::String (count) + " tracks", true);

    menu.showMenuAsync (juce::PopupMenu::Options(),
        [safe = juce::Component::SafePointer<BrowserComponent> (this), chosenFiles] (int chosen)
        {
            if (safe == nullptr || chosen <= 0)
                return;

            safe->library.addToPlaylist ((juce::int64) chosen - 1, chosenFiles);

            if (safe->onPlaylistsChanged != nullptr)
                safe->onPlaylistsChanged();
        });
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

    // One track is a plain path, which decks and the playlist both take. Several
    // are a list of paths, which only the playlist takes: a deck holds one.
    juce::Array<juce::var> paths;

    for (int i = 0; i < selectedRows.size(); ++i)
        if (const auto r = selectedRows[i]; juce::isPositiveAndBelow (r, (int) rows.size()))
            paths.add (rows[(size_t) r].file.getFullPathName());

    if (paths.isEmpty())
        return {};

    return paths.size() == 1 ? paths[0] : juce::var (paths);
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
    toolbar.removeFromLeft (6);
    importButton.setBounds (toolbar.removeFromLeft (80));
    toolbar.removeFromLeft (6);
    camelotButton.setBounds (toolbar.removeFromLeft (70));
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
