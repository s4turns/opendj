/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/NowPlayingOverlay.h"

#include "ui/AngleMath.h"
#include "ui/OverlayMath.h"

namespace opendj
{

namespace
{
    // The same colours as the on-screen platter, so the two read as one thing.
    const juce::Colour recordColour { 0xff17171c };
    const juce::Colour ringColour   { 0xff26262e };
    const juce::Colour grooveColour { 0x22ffffff };
    const juce::Colour markerColour { 0xffe8e8ee };
    const juce::Colour accentColour { 0xff35c2f0 };
    const juce::Colour labelColour  { 0xff2a2a33 };

    double nowSeconds() { return juce::Time::getMillisecondCounterHiRes() / 1000.0; }
}

void NowPlayingOverlay::setNowPlaying (const juce::String& title, float audibility, bool playing)
{
    const std::lock_guard<std::mutex> lock (mutex);

    if (title != nowPlaying.text)
    {
        nowPlaying.text = title;
        nowPlaying.changedAtSeconds = nowSeconds();
    }

    nowAudibility = audibility;
    nowIsPlaying = playing;
}

void NowPlayingOverlay::setComingUp (const juce::String& title)
{
    const std::lock_guard<std::mutex> lock (mutex);

    if (title != comingUp.text)
    {
        comingUp.text = title;
        comingUp.changedAtSeconds = nowSeconds();
    }
}

void NowPlayingOverlay::drawTurntable (juce::Graphics& g, juce::Point<float> centre, float radius,
                                       const AudioEngine::DeckStatus& status, int deckIndex) const
{
    // Bright when it is in the mix, dim when it is waiting or stopped.
    const auto brightness = status.playing ? 0.45f + 0.55f * juce::jlimit (0.0f, 1.0f, status.audibility)
                                           : 0.35f;

    g.beginTransparencyLayer (brightness);

    const auto ring = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);
    g.setColour (ringColour);
    g.fillEllipse (ring);

    const auto recordRadius = radius * 0.8f;
    g.setColour (recordColour);
    g.fillEllipse (juce::Rectangle<float> (recordRadius * 2.0f, recordRadius * 2.0f).withCentre (centre));

    g.setColour (grooveColour);

    for (auto r = recordRadius * 0.45f; r < recordRadius; r += juce::jmax (3.0f, recordRadius * 0.09f))
        g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (centre), 1.0f);

    // The marker: the track position at 33 1/3 rpm, the same angle the
    // on-screen platter uses, so a scratch or a nudge shows here too.
    juce::Path marker;
    marker.addRoundedRectangle (-radius * 0.022f, -recordRadius * 0.94f,
                                radius * 0.044f, recordRadius * 0.62f, radius * 0.02f);
    g.setColour (markerColour);
    g.fillPath (marker, juce::AffineTransform::rotation ((float) angles::platterAngle (status.positionSeconds))
                            .translated (centre));

    const auto labelRadius = recordRadius * 0.42f;
    const auto labelBox = juce::Rectangle<float> (labelRadius * 2.0f, labelRadius * 2.0f).withCentre (centre);
    g.setColour (labelColour);
    g.fillEllipse (labelBox);

    g.setColour (status.playing ? accentColour : juce::Colours::grey);
    g.setFont (juce::FontOptions (juce::jmax (10.0f, labelRadius * 0.9f), juce::Font::bold));
    g.drawText (juce::String (deckIndex + 1), labelBox, juce::Justification::centred);

    g.endTransparencyLayer();
}

void NowPlayingOverlay::composite (unsigned char* rgb, int width, int top, int bottom) const
{
    juce::Image::BitmapData bitmap (canvas, juce::Image::BitmapData::readOnly);

    for (int y = top; y < bottom; ++y)
    {
        const auto* source = bitmap.getLinePointer (y);
        auto* destination = rgb + (size_t) y * (size_t) width * 3;

        for (int x = 0; x < width; ++x)
        {
            // JUCE's ARGB images are premultiplied BGRA in memory, so the
            // colour is already scaled by its own alpha.
            const auto* pixel = source + x * 4;

            if (pixel[3] == 0)
                continue;

            const auto keep = 1.0f - (float) pixel[3] / 255.0f;

            destination[x * 3 + 0] = (unsigned char) juce::jmin (255.0f, destination[x * 3 + 0] * keep + pixel[2]);
            destination[x * 3 + 1] = (unsigned char) juce::jmin (255.0f, destination[x * 3 + 1] * keep + pixel[1]);
            destination[x * 3 + 2] = (unsigned char) juce::jmin (255.0f, destination[x * 3 + 2] * keep + pixel[0]);
        }
    }
}

void NowPlayingOverlay::draw (unsigned char* rgb, int width, int height)
{
    if (width <= 0 || height <= 0)
        return;

    Line now, next;
    float audibility = 0.0f;
    bool playing = false;

    {
        const std::lock_guard<std::mutex> lock (mutex);
        now = nowPlaying;
        next = comingUp;
        audibility = nowAudibility;
        playing = nowIsPlaying;
    }

    {
        now.text = "Debug Artist - Debug Title"; now.changedAtSeconds = nowSeconds() - 3.0; playing = true; audibility = 1.0f;
        next.text = "Next Artist - Next Title"; next.changedAtSeconds = nowSeconds() - 3.0;
    }

    const auto t = nowSeconds();
    const auto nowOpacity = now.text.isEmpty()
        ? 0.0f : overlay::nowPlayingOpacity (t - now.changedAtSeconds, audibility, playing);
    const auto nextOpacity = next.text.isEmpty()
        ? 0.0f : juce::jlimit (0.0f, 1.0f, (float) ((t - next.changedAtSeconds) / overlay::fadeSeconds));

    // One corner for each deck: A top left, B top right, C bottom left, D
    // bottom right. A deck with nothing on it leaves its corner empty.
    std::array<AudioEngine::DeckStatus, AudioEngine::numDecks> statuses {};

    for (int i = 0; i < AudioEngine::numDecks; ++i)
        statuses[(size_t) i] = engine.getDeckStatus (i);

    // Only the two bands anything is drawn in are cleared and blended: a full
    // frame through a software renderer thirty times a second is work the
    // visuals can do without.
    const auto margin = (float) height / 24.0f;
    const auto turntableRadius = (float) height / 9.0f;
    const auto band = juce::jmin (height / 2, (int) (turntableRadius * 2.0f + margin * 2.0f));
    const auto topBand = band;
    const auto bottomBand = band;
    const auto bottomTop = height - bottomBand;

    // A plain software image, not the platform's native one: on Windows that
    // is Direct2D backed, and text drawn into it from this thread, which is
    // not the message thread, does not come out.
    if (! canvas.isValid() || canvas.getWidth() != width || canvas.getHeight() != height)
        canvas = juce::Image (juce::Image::ARGB, width, height, true, juce::SoftwareImageType());

    canvas.clear ({ 0, 0, width, topBand });
    canvas.clear ({ 0, bottomTop, width, bottomBand });

    {
        juce::Graphics g (canvas);

        const auto left = margin + turntableRadius;
        const auto right = (float) width - margin - turntableRadius;
        const auto top = margin + turntableRadius;
        const auto bottom = (float) height - margin - turntableRadius;
        const juce::Point<float> centres[AudioEngine::numDecks] {
            { left, top }, { right, top }, { left, bottom }, { right, bottom } };

        for (int i = 0; i < AudioEngine::numDecks; ++i)
            if (statuses[(size_t) i].loaded)
                drawTurntable (g, centres[i], turntableRadius, statuses[(size_t) i], i);

        // The watermark, in the gutter under the last deck's turntable, in the
        // bottom right corner. Always on, and faint enough to ignore.
        {
            const auto box = juce::Rectangle<float> ((float) width - margin - 240.0f, (float) height - margin,
                                                     240.0f, margin);
            g.setFont (juce::FontOptions (margin * 0.78f, juce::Font::bold));
            g.setColour (juce::Colours::black.withAlpha (0.45f));
            g.drawText ("OpenDJ", box.translated (1.0f, 1.0f), juce::Justification::centredRight, false);
            g.setColour (juce::Colours::white.withAlpha (0.6f));
            g.drawText ("OpenDJ", box, juce::Justification::centredRight, false);
        }

        if (nowOpacity > 0.0f || nextOpacity > 0.0f)
        {
            // A soft dark band so white text survives a white preset.
            g.setGradientFill (juce::ColourGradient (juce::Colours::transparentBlack, 0.0f, (float) bottomTop,
                                                     juce::Colours::black.withAlpha (0.65f), 0.0f, (float) height,
                                                     false));
            g.fillRect (0, bottomTop, width, bottomBand);

            const auto fontHeight = (float) height / 18.0f;
            const auto inset = margin * 2.0f + turntableRadius * 2.0f;
            const auto area = juce::Rectangle<float> (inset, (float) bottomTop, (float) width - inset * 2.0f,
                                                      (float) bottomBand).withTrimmedBottom (margin * 0.6f);

            auto text = area;

            if (nowOpacity > 0.0f)
            {
                const auto line = text.removeFromBottom (fontHeight * 1.5f);
                g.beginTransparencyLayer (nowOpacity);
                g.setFont (juce::FontOptions (fontHeight, juce::Font::bold));
                g.setColour (juce::Colours::black.withAlpha (0.8f));
                g.drawText (now.text, line.translated (2.0f, 2.0f), juce::Justification::centredBottom, true);
                g.setColour (juce::Colours::white);
                g.drawText (now.text, line, juce::Justification::centredBottom, true);
                g.endTransparencyLayer();
            }

            if (nextOpacity > 0.0f)
            {
                const auto line = text.removeFromBottom (fontHeight * 1.1f);
                const auto message = "Coming up next  " + next.text;
                g.beginTransparencyLayer (nextOpacity);
                g.setFont (juce::FontOptions (fontHeight * 0.68f, juce::Font::plain));
                g.setColour (juce::Colours::black.withAlpha (0.8f));
                g.drawText (message, line.translated (1.5f, 1.5f), juce::Justification::centredBottom, true);
                g.setColour (accentColour);
                g.drawText (message, line, juce::Justification::centredBottom, true);
                g.endTransparencyLayer();
            }
        }
    }

    composite (rgb, width, 0, topBand);
    composite (rgb, width, bottomTop, height);
}

} // namespace opendj
