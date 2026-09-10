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
| 5 | Key lock, so tempo does not shift pitch | ✅ | `Deck::renderStretched`, via Rubber Band |
| 6 | Mixer: fader, three-band EQ, crossfader, cue | ✅ | `src/core/Mixer.*` |
| 7 | Scrolling and overview waveforms | ✅ | `src/ui/WaveformComponent.*` |
| 8 | BPM detection and beat grid | ✅ | `src/analysis/TrackAnalyser.*`, checked against a real collection below |
| 9 | Sync: tempo and beat phase | ✅ | `AudioEngine::syncDeck` |
| 10 | Turntable platters, mouse drivable | ✅ | `src/ui/PlatterComponent.*`, `src/ui/AngleMath.h` |
| 11 | Hot cues | ✅ | `Deck::hotCuePressed` and friends |
| 12 | MIDI mapping engine, monitor, device picker | ✅ | `src/control/`, `src/ui/MidiSetupComponent.*` |
| 13 | Roland DJ-202 mapping | 🚧 | `mappings/roland-dj-202.json` |
| 14 | Track browser and library database | ✅ | `src/library/`, `src/ui/BrowserComponent.*` |

## Platforms

| Platform | Builds | Runs | Notes |
| --- | :---: | :---: | --- |
| Windows | ✅ | ✅ | Built and run with MSVC 19.44. WASAPI by default, ASIO opt-in. CI needs a self-hosted runner, see below |
| Fedora | ✅ | ✅ | Built and run on Fedora 44 with GCC 16 and PipeWire's JACK. Decks, browser, analysis and the library all exercised |
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

## Item 5: key lock — done

| Step | Status | Notes |
| --- | :---: | --- |
| Rubber Band | ✅ | Fetched by CMake and built from its single compilation unit, so its Meson build is never invoked. GPLv2 or later, so licence compatible |
| Feed the stretcher from the deck | ✅ | `Deck::feedStretcher` reads the track at its recorded speed and the time ratio carries the tempo. The stretcher is built in `prepare`, so the audio thread never allocates one |
| Bypass it while scratching | ✅ | And at exactly the recorded speed, where it would be a delay line that appears and disappears with the fader |
| Key lock button and DJ-202 mapping | ✅ | `Key` on each deck, and notes 0x0D and 0x0E on the controller |

Five tests in `tests/Deck_test.cpp` count zero crossings to measure the pitch that
actually came out, so they check the tone held rather than that the code ran.

## Item 14: track browser and library — done

| Step | Status | Notes |
| --- | :---: | --- |
| SQLite schema | ✅ | `src/library/Library.cpp`: folders, tracks, tags, tempo, and an analysis version, so a better analyser quietly invalidates old results |
| Analyse once, not on every load | ✅ | `AnalysisCache` in `src/analysis/`, which the engine asks before it decodes. `Deck::loadFile` takes a `KnownTrack` and skips the tempo pass |
| Scanning | ✅ | `LibraryScanner`: a quick tag pass so the browser fills in seconds, then analysis at low priority, written as it goes so it can be stopped and picked up later |
| Tag reading | ✅ | `src/library/TagReader.cpp`. ID3v2.2 to 2.4 and ID3v1 parsed directly, plus MP3 length from the Xing header, because opening a decoder for 13,000 files just to ask their length is far too slow |
| Browser panel | ✅ | Search, sortable columns, folder filter, double-click or drag onto a deck |
| Wire up the controller load buttons | ✅ | `selectedFileProvider`, plus a new `browse.scroll` action so the DJ-202 browse encoder moves the selection |

## Item 8: how good is the tempo detection?

Checked against 60 tracks taken at random from a 13,000 track techno collection,
with the BPM Mixxx had already worked out for each as the comparison.

| Estimator | Agreed with Mixxx |
| --- | :---: |
| Autocorrelation peak alone | 44 / 60 |
| Pulse-train scoring and a tempo prior | **57 / 60** |

Nearly every failure of the first version was a ratio of exactly 4/3 — 170.7 BPM
reported for a 128 BPM track — and it reported them at full confidence.
Autocorrelation cannot tell a tempo from three quarters of it, because the beat
itself contaminates that lag. What the estimator does now:

- **Judges a candidate by a pulse train rather than by correlation.** A train at
  4/3 of the tempo lands on a beat once every four pulses. It is scored on the
  level the weakest quarter of its pulses reach instead of their average, so
  hitting hard three times in twelve does not rescue it.
- **Aligns that train in windows of eight beats.** A rigid grid over five minutes
  drifts off the beat from the fraction of a frame the period is out by, and a
  drifting grid scores the correct tempo as a miss.
- **Weights the bass band double.** The kick is the beat.
- **Carries a tempo prior**, centred on 130 BPM. Nothing in the signal can
  separate a tempo from half of it, so that assumption is written down in one
  place instead of being spread through the search.
- **Reports honest confidence**: how far the winner finished clear of the best
  rival that is not simply a rounding of it. The median is 1.00 where it agrees
  with Mixxx and 0.73 where it does not, so the number is worth reading.

Of the three that still disagree, one is a 63 BPM reading from Mixxx for a track
that is plainly 126, so the disagreement is not necessarily the wrong way round.

To repeat the check, point the library at a folder, let the scan finish, and
compare the `bpm` column with whatever you trust.

## Beyond milestone 1

| Item | Status | Notes |
| --- | :---: | --- |
| Key detection | ⬜ | The library has the column and the browser the display; only tags fill them in so far |
| Four decks | ⬜ | `AudioEngine::numDecks` is a constant the mixer sizes itself from, so the engine mostly follows. The interface and the DJ-202 deck-toggle button are the work |
| Loops and loop rolls | ⬜ | The DJ-202's second pad row is already reserved for these |
| Effects | ⬜ | Filter, echo, reverb. The DJ-202 effects section is on MIDI channels 9 and 10, unmapped |
| Sampler | ⬜ | The DJ-202 pads send sampler notes on 0x21 to 0x30, unmapped |
| Record the master output | ⬜ | Straightforward: tap the master buffer in `AudioEngine` |
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

## Continuous integration

The workflow is `.gitea/workflows/build.yml`. It runs on pushes to `main` and `testing`, on pull
requests, and on demand from the Actions tab.

### Re-running a workflow

Open the Actions tab, pick the workflow, and use **Run workflow** for a fresh run, or open a
finished run and use its re-run button. Neither needs a commit. That matters because most
Windows failures are not the code: the runner is a console program on a desktop machine, so it
goes down when the session that started it does, and a job already in flight is then left with
no runner and fails for reasons that have nothing to do with the build.

If a Windows job fails or sits unclaimed, check the machine before reading the log:

| Check | Command |
| --- | --- |
| Is the runner running? | `Get-Process act_runner` |
| Did its task stop, and why? | `Get-ScheduledTaskInfo -TaskName 'Gitea Actions runner'` |
| Start it again | `Start-ScheduledTask -TaskName 'Gitea Actions runner'` |

A last result of `0xC000013A` means it was killed by the session ending rather than crashing.
The task is set to restart itself once a minute if that happens.

| Job | Runs on | Status |
| --- | --- | :---: |
| fedora | `fedora:latest` container on the Linux runner | ✅ |
| debian | the Linux runner's own image | ✅ |
| windows | a self-hosted Windows runner | ✅ |

Two things about this setup are worth knowing before changing it.

| Detail | Why |
| --- | --- |
| The Fedora job installs `nodejs` and `git` before anything else | `actions/checkout` is a JavaScript action, and the stock Fedora image has neither. Without that step the job fails in two seconds, long before a compiler is involved |
| The Windows job calls `scripts/build.ps1` rather than CMake directly | The script finds the CMake, Ninja and MSVC environment inside Visual Studio Build Tools, so a self-hosted machine needs nothing on PATH but git and node |

The README carries a status badge per branch. Gitea's badges are per workflow rather than per
job, so one badge covers all three platforms and goes red if any of them fails.

### Registering a Windows runner

One is already registered as `INTERHOME-windows`, started from a scheduled task at logon, so
Windows CI runs only while that machine is logged in. To add another, or to replace it: the
Linux runner advertises `ubuntu-latest` and has no MSVC, so it will never take the Windows job,
and that job is simply never scheduled without a Windows machine. On a machine with Visual
Studio Build Tools, git and Node installed:

```
pwsh scripts/setup-windows-runner.ps1 -Token <registration token>
```

Get the token from Gitea under repository Settings, then Actions, then Runners, then "Create new
runner". The script downloads the runner, registers it with the label `windows-latest:host`, and
starts it from a task at logon. The `:host` suffix is what makes jobs run directly on the machine
rather than in a container.
