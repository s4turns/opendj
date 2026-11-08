/*
    This file is part of OpenDJ.
    Copyright (C) 2026 The OpenDJ contributors.

    OpenDJ is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. See the LICENSE file at the root of this repository.
*/

#include <JuceHeader.h>

#include "app/MainComponent.h"
#include "app/Settings.h"

namespace opendj
{

class OpenDJApplication final : public juce::JUCEApplication
{
public:
    OpenDJApplication() = default;

    const juce::String getApplicationName() override    { return ProjectInfo::projectName; }
    const juce::String getApplicationVersion() override { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        mainWindow = std::make_unique<MainWindow> (getApplicationName());

        // Files named on the command line go straight onto the decks. It saves
        // a lot of clicking while testing, and it makes the app work as a
        // handler for audio files.
        if (const auto files = juce::StringArray::fromTokens (commandLine, true); ! files.isEmpty())
            if (auto* content = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                content->loadInitialTracks (files);
    }

    void shutdown() override
    {
        mainWindow.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name,
                              juce::Colour (0xff141418),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, false);
            setResizeLimits (900, 600, 10000, 10000);

            // Back where it was left, if that is still somewhere a window can
            // be. A monitor that has since been unplugged would otherwise put
            // the window off the edge of everything, so the saved bounds are
            // only honoured when they land on a screen that exists.
            const auto saved = SessionState::readFrom (SessionState::defaultFile()).windowBounds;
            const auto bounds = juce::Rectangle<int>::fromString (saved);

            if (! bounds.isEmpty()
                && juce::Desktop::getInstance().getDisplays()
                       .getTotalBounds (true).intersects (bounds.reduced (40)))
                setBounds (bounds);
            else
                centreWithSize (getWidth(), getHeight());

            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        // A freshly shown window does not always hand keyboard focus to its
        // content: on X11 (Linux) it does not, so the key shortcuts — F11 for
        // full screen, the deck cue and play keys, the sampler pads — did nothing
        // until the window was clicked. Giving the content the keys whenever the
        // window becomes active fixes that at launch and on every refocus, and the
        // check that nothing is focused yet keeps it from stealing the caret from
        // the library search box when the window is clicked back into.
        void activeWindowStatusChanged() override
        {
            DocumentWindow::activeWindowStatusChanged();

            if (isActiveWindow()
                && juce::Component::getCurrentlyFocusedComponent() == nullptr)
                if (auto* content = getContentComponent())
                    content->grabKeyboardFocus();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace opendj

START_JUCE_APPLICATION (opendj::OpenDJApplication)
