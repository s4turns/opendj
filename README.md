# OpenDJ

Free and open source DJ software, built in C++ with [JUCE](https://juce.com).

The goal is an application a VirtualDJ user can sit down at and already know how to use:
the same deck layout, the same workflow, the same muscle memory. The artwork, the naming
and the code are entirely original and entirely open.

**Status: early development.** Two decks play with waveforms, beat grids, automatic BPM
detection and sync. The mixer works. There is no track browser and no controller support yet.

## Design goals

- **Real audio performance.** ASIO and JACK support, multi-device routing, and a realtime
  safe engine that never allocates or locks on the audio thread.
- **Hardware first.** A data-driven MIDI mapping layer with the Roland DJ-202 as the
  reference controller. Mappings are JSON files, so a new controller needs no recompile.
- **Free, in both senses.** GPLv3, no paid tier, no locked features.

## Roadmap to the first playable release

1. Project scaffolding and audio device setup — **done**
2. Two decks with play, pause and cue
3. Tempo fader and key lock
4. Scrolling and overview waveforms
5. Mixer: gain, three-band EQ, crossfader, headphone cue
6. BPM analysis, beatgrid and sync
7. Track browser backed by a SQLite library
8. MIDI mapping engine, monitor and learn panel
9. Roland DJ-202 mapping, including jog wheel scratch

Out of scope until that is finished: effects racks, four decks, stem separation, video,
streaming services and recording.

## Using it

Load a track with the Load button on either deck, or drag an audio file onto a deck. Decoding
and analysis run in the background, so the interface stays responsive on a long file. Click the
overview waveform to move through the track.

Each deck shows a scrolling waveform with the beat grid drawn over it, bar lines brighter than
beats, and the detected tempo next to the title. Sync matches this deck's tempo and beat phase
to the other one.

| Key | Action |
| --- | --- |
| Q / W | Deck A cue / play |
| O / P | Deck B cue / play |

Cue follows the behaviour DJs expect. While the deck is stopped, pressing cue sets the cue
point and previews from it, and releasing returns and stops. While it is playing, pressing
cue drops straight back to the cue point and stops there.

## Building

You need a C++20 compiler, CMake 3.22 or newer, and git.

On Windows, Visual Studio 2022 Build Tools with the C++ workload provides all three; its
bundled CMake and Ninja are used automatically by `scripts/build.ps1`.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

JUCE and Catch2 are fetched into `external/` at configure time, so the first configure
needs a network connection and takes a few minutes.

On Linux you also need the JUCE system packages:

```
sudo apt install build-essential cmake pkg-config libasound2-dev libjack-jackd2-dev \
    libfreetype-dev libfontconfig1-dev libx11-dev libxext-dev libxinerama-dev \
    libxrandr-dev libxcursor-dev libcurl4-openssl-dev
```

### ASIO on Windows

ASIO is off by default because Steinberg's SDK cannot be redistributed. Download it
yourself, then configure with:

```
cmake -S . -B build -DOPENDJ_ENABLE_ASIO=ON -DOPENDJ_ASIO_SDK_PATH=C:/path/to/asiosdk
```

Without it the app uses WASAPI, which is fine for development but not for a live set.

## Running the tests

```
ctest --test-dir build --output-on-failure
```

## Roland DJ-202 notes

The DJ-202 is a two-deck controller with a built-in four-channel USB audio interface, which
maps directly onto OpenDJ's master-on-1-2 and cue-on-3-4 routing.

Set the unit's USB mode to match your platform. On Windows use **Vendor** mode with Roland's
driver installed, which gives you ASIO. On Linux use **Generic** mode, which is USB class
compliant and works with ALSA and JACK with no driver at all.

The control assignments were derived from Roland's MIDI implementation chart and cross
checked against the [Mixxx community mappings](https://github.com/mixxxdj/mixxx/wiki/Roland-Dj-202)
for the DJ-202 and the closely related DJ-505. Those mappings are GPLv2 or later and so are
compatible with this project; they are credited in the header of `mappings/roland-dj-202.json`.

## Licence

GPLv3 or later. See [LICENSE](LICENSE).

OpenDJ is not affiliated with, endorsed by, or derived from Atomix Productions, Roland
Corporation, or Serato. Product names are trademarks of their respective owners and are used
only to describe hardware and file compatibility.
