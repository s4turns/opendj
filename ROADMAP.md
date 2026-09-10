# OpenDJ roadmap

Where the project is and what to pick up next. Anything ticked has tests or a verified manual
check behind it, not just code that compiles.

| Symbol | Meaning |
| :---: | --- |
| ✅ | Done and verified |
| 🚧 | Partly done, details in the table below it |
| ⬜ | Not started |

## Milestone 1 — a deck you can actually mix on

| # | Item | Status | Where it lives |
| :---: | --- | :---: | --- |
| 1 | Build system, GPLv3, CI | ✅ | `CMakeLists.txt`, `scripts/`, `.gitea/workflows/` |
| 2 | Audio device setup, master and cue routing | ✅ | `src/core/AudioEngine.*` |
| 3 | Two decks: load, play, pause, cue | ✅ | `src/core/Deck.*` |
| 4 | Tempo fader | ✅ | `src/core/Deck.*`, `src/ui/DeckComponent.*` |
| 5 | Key lock, so tempo does not shift pitch | ⬜ | not started, needs a time stretcher |
| 6 | Mixer: fader, three-band EQ, crossfader, cue | ✅ | `src/core/Mixer.*` |
| 7 | Scrolling and overview waveforms | ✅ | `src/ui/WaveformComponent.*` |
| 8 | BPM detection and beat grid | ✅ | `src/analysis/TrackAnalyser.*` |
| 9 | Sync: tempo and beat phase | ✅ | `AudioEngine::syncDeck` |
| 10 | Turntable platters, mouse drivable | ✅ | `src/ui/PlatterComponent.*`, `src/ui/AngleMath.h` |
| 11 | Hot cues | ✅ | `Deck::hotCuePressed` and friends |
| 12 | MIDI mapping engine, monitor, device picker | ✅ | `src/control/`, `src/ui/MidiSetupComponent.*` |
| 13 | Roland DJ-202 mapping | 🚧 | `mappings/roland-dj-202.json` |
| 14 | Track browser and library database | ⬜ | nothing yet |

## Platforms

| Platform | Builds | Runs | Notes |
| --- | :---: | :---: | --- |
| Windows | ✅ | ✅ | Built and run with MSVC 19.44. WASAPI by default, ASIO opt-in |
| Fedora | ✅ | ⬜ | Builds in the Fedora CI job; nobody has run it on a desktop yet |
| Debian and Ubuntu | ✅ | ⬜ | Same, via the Debian CI job |
| Arch | ⬜ | ⬜ | `scripts/build.sh` knows the packages, untested |
| macOS | ⬜ | ⬜ | JUCE supports it; nothing has been tried |

## Item 13: DJ-202 checks that need the hardware

The mapping is complete for two-deck use, but three values were derived from documentation
rather than measured. The Controller panel's monitor shows raw values, so each takes a minute.

| Check | Status | What to do |
| --- | :---: | --- |
| Tempo fader polarity | ⬜ | Mapped `"inverted": true`, assuming the highest value is at the bottom of the throw. If the fader works backwards, set it to `false` |
| Note off behaviour | ⬜ | The mapping assumes buttons send note on with velocity 0. Both forms are handled, but the pads have not been watched on hardware |
| Jog tick rate | ⬜ | 512 ticks per revolution came from the Mixxx mapping. If one full turn does not move the track by exactly 1.8 seconds, change `jogTicksPerRevolution` |

## Item 5: key lock

| Step | Status | Notes |
| --- | :---: | --- |
| Add Rubber Band R3 as a submodule | ⬜ | GPL, so licence compatible. Its build is not CMake, so expect to write a small wrapper |
| Feed the stretcher from the deck | ⬜ | The hook is where `rate` is applied to `readPosition` in `Deck::processBlock`. With key lock on, read at the file rate and ask the stretcher for the tempo ratio |
| Bypass it while scratching | ⬜ | A stretcher cannot follow a hand. Trying to make it is where this feature usually goes wrong |
| Key lock button and DJ-202 mapping | ⬜ | The controller already sends it on notes 0x0D and 0x0E |

## Item 14: track browser and library

| Step | Status | Notes |
| --- | :---: | --- |
| SQLite schema | ⬜ | Path, title, duration, BPM, key, cached waveform peaks |
| Analyse once, not on every load | ⬜ | `TrackAnalyser::analyse` already produces everything the table needs; it has nowhere to be stored |
| Browser panel | ⬜ | Folder tree and track list, drag to a deck |
| Wire up the controller load buttons | ⬜ | `ActionDispatcher::selectedFileProvider` is the hook. It exists and returns nothing, which is why the DJ-202 load buttons currently do nothing |

## Beyond milestone 1

| Item | Status | Notes |
| --- | :---: | --- |
| Four decks | ⬜ | `AudioEngine::numDecks` is a constant the mixer sizes itself from, so the engine mostly follows. The interface and the DJ-202 deck-toggle button are the work |
| Loops and loop rolls | ⬜ | The DJ-202's second pad row is already reserved for these |
| Effects | ⬜ | Filter, echo, reverb. The DJ-202 effects section is on MIDI channels 9 and 10, unmapped |
| Sampler | ⬜ | The DJ-202 pads send sampler notes on 0x21 to 0x30, unmapped |
| Record the master output | ⬜ | Straightforward: tap the master buffer in `AudioEngine` |
| Key detection | ⬜ | The analysis pass already has the audio in memory and an FFT set up |
| Slip mode | ⬜ | The DJ-202 has a button for it on note 0x07 |
| Stem separation | ⬜ | Large. Needs a model and a licence decision about shipping weights |
| Video | ⬜ | Very large. Probably a separate project |

## Working on this

| Task | Windows | Linux |
| --- | --- | --- |
| Install dependencies | comes with VS 2022 Build Tools | `scripts/build.sh --deps` |
| Build | `pwsh scripts/build.ps1` | `scripts/build.sh` |
| Build and run | `pwsh scripts/build.ps1 -Run` | `scripts/build.sh --run` |
| Run the tests | `ctest --test-dir build -C RelWithDebInfo` | `ctest --test-dir build` |
| Start from scratch | `pwsh scripts/build.ps1 -Clean` | `scripts/build.sh --clean` |

Pass audio files on the command line to start with tracks already on the decks, which is far
faster than clicking through a file dialog while testing:

```
scripts/build.sh --run track-a.wav track-b.wav
```

## Rules worth knowing before changing the engine

| Rule | Why |
| --- | --- |
| Nothing in `src/core` includes a UI header | Keeps the engine testable without a display, and stops interface concerns leaking into audio code |
| The audio thread never allocates, locks or touches a file | Any of the three can stall a block and produce an audible dropout |
| Handing new data to the audio thread uses an atomic pointer swap | See `Deck::publish` and `Deck::cleanUp`. Copy that pattern rather than adding a mutex |
| Every input goes through `ActionDispatcher` | Mouse, keyboard and MIDI produce the same actions, so the interface and the controller cannot drift apart |
| Analysis results are immutable and shared by pointer | The interface can hold one as long as it likes without affecting the audio thread's lifetime rules |
| Anything touching audio gets a test that measures the output | The suite has already caught three real bugs this way, not by checking that code runs |
