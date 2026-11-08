/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace opendj
{

/** The stem items a list's right-click menu offers. A list knows what it has
    selected and nothing about the engine, so the shell fills these in; what it
    fills them with goes through the dispatcher. */
struct StemPrepareMenu
{
    /** `hasStatus` is whether the queue has something on show that is over: a
        count of a finished list, or a halt. The cancel clears that too. */
    std::function<bool()> isAvailable, isBusy, hasStatus;
    std::function<void()> prepare, cancel;

    static constexpr const char* cancelText = "Cancel preparing stems";
    static constexpr const char* clearText = "Clear the stems status";

    /** `label` is what to call preparing, which depends on what is selected. */
    void addTo (juce::PopupMenu& menu, const juce::String& label, bool anythingToPrepare) const
    {
        if (prepare == nullptr)
            return;

        menu.addSeparator();

        const auto available = isAvailable != nullptr && isAvailable();
        menu.addItem (available ? label : label + " (no separator found)",
                      available && anythingToPrepare, false, prepare);

        if (cancel == nullptr)
            return;

        // A halt empties the queue, so it is not busy any more, and its message
        // would otherwise stay up with nothing to take it down.
        if (isBusy != nullptr && isBusy())
            menu.addItem (cancelText, cancel);
        else if (hasStatus != nullptr && hasStatus())
            menu.addItem (clearText, cancel);
    }
};

/** Which tracks a press of stems.prepare_selected means. The library offers its
    selection; the playlists offer the whole of the playlist on show, which is
    what its own menu prepares too. */
inline const std::vector<juce::File>& stemListOnShow (bool playlistsOnShow,
                                                     const std::vector<juce::File>& playlist,
                                                     const std::vector<juce::File>& librarySelection)
{
    return playlistsOnShow ? playlist : librarySelection;
}

} // namespace opendj
