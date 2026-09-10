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
| 6b | Per-channel filter, low pass down and high pass up | ✅ | `Mixer::setChannelFilter` |
| 7 | Scrolling and overview waveforms | ✅ | `src/ui/WaveformComponent.*` |
| 8 | BPM detection and beat grid | ✅ | `src/analysis/TrackAnalyser.*`, checked against a real collection below |
| 9 | Sync: tempo and beat phase | ✅ | `AudioEngine::syncDeck` |
| 10 | Turntable platters, mouse drivable | ✅ | `src/ui/PlatterComponent.*`, `src/ui/AngleMath.h` |
| 11 | Hot cues | ✅ | `Deck::hotCuePressed` and friends |
| 12 | MIDI mapping engine, monitor, device picker | ✅ | `src/control/`, `src/ui/MidiSetupComponent.*` |
| 13 | Roland DJ-202 mapping | ✅ | `mappings/roland-dj-202.json`, verified on hardware |
| 14 | Track browser and library database | ✅ | `src/library/`, `src/ui/BrowserComponent.*` |

## Platforms

| Platform | Builds | Runs | Notes |
| --- | :---: | :---: | --- |
| Windows | ✅ | ✅ | Built and run with MSVC 19.44. WASAPI by default, ASIO opt-in. CI needs a self-hosted runner, see below |
| Fedora | ✅ | ✅ | Built and run on Fedora 44 with GCC 16, on a DJ-202's own four-channel interface through PipeWire's JACK. Decks, platters, browser, analysis and the library all exercised |
| Debian and Ubuntu | ✅ | ⬜ | Same, via the Debian CI job |
| Arch | ⬜ | ⬜ | `scripts/build.sh` knows the packages, untested |
| macOS | ⬜ | ⬜ | JUCE supports it; nothing has been tried |

## FX

Real-time stem separation using the same algorithm as Virtual DJ.

## Item 13: DJ-202, as measured on the hardware

| Check | Status | What was found |
| --- | :---: | --- |
| Note off behaviour | ✅ | Both forms occur, and which one depends on the sequencer's MIDI version: as a UMP client the DJ-202's releases arrive as note off, as a legacy client as note on with velocity zero. A release is now taken from the message rather than the mapping, and a note off falls back to the note on control of the same number, so either form releases the button it belongs to |
| Jog tick rate | ✅ | 800, not the 512 the Mixxx mapping states: two turns of the platter produced 1590 ticks on controller 6 |
| Platter encoding | ✅ | Controller 6, relative, centred on 64. The wheel also streams 14-bit absolute position as pitch bend, but that flows whenever a hand merely rests on it and its net movement over two real turns was 0.04 of a revolution, so it is deliberately unmapped |
| Tempo fader polarity | ⬜ | Still mapped `"inverted": true` on the assumption that the highest value is at the bottom of the throw. If the fader works backwards, set it to `false` |

### The one that cost the most: controller 6 and MIDI 2.0

JUCE registers its ALSA sequencer client as MIDI 2.0. On a kernel that knows about
UMP, the sequencer then translates every legacy message before handing it over, and
MIDI 2.0 reserves controller 6 as Data Entry, the middle of an RPN sequence, rather
than a controller in its own right. A bare controller 6 is swallowed in translation.

The DJ-202's platters report on controller 6. The touch was seen and the turning was
not, so the deck stopped dead under the hand. It is reproducible with nothing but
`aseqdump`:

```
aseqdump -u 0   ->  controller 6 arrives, controller 7 arrives
aseqdump -u 2   ->  controller 6 is gone, controller 7 arrives
```

`src/control/AlsaMidiCompat.cpp` defines the weak symbol JUCE uses to ask for MIDI
2.0, so the client stays at the legacy MIDI 1.0 a new one gets by default. Delete it
once JUCE lets an application choose its own client MIDI version.

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

## Key detection

Twelve pitch classes folded out of the spectrum, then matched against the twenty-four
Krumhansl-Kessler major and minor profiles. Reported in the spelling DJ software uses, sharps
throughout, and convertible to Camelot notation, which is what harmonic mixing actually runs on.

| Decision | Why |
| --- | --- |
| Only the middle two minutes are read | The intro and outro of a club record are usually just drums, which say nothing about key and pull the histogram towards noise. Reading a whole ten minute track costs five times as much and changes almost nothing |
| Bins more than 35 cents off a semitone do not vote | A bin sitting between two semitones belongs to neither, and letting it vote is what turns a chromagram into a flat smear |
| Nothing below about C3 counts | Below that, semitones are closer together than the transform can resolve, so they land in the wrong pitch class |
| Each frame is normalised before it is added | So a loud drop does not outvote the four quiet minutes that share its key |
| A key already in the tags wins | Detection fills the gap where there is nothing rather than overruling what a person put there |

Confidence is the margin over the runner-up. It is worth reading: a track whose key is
unambiguous finishes well clear, while a chromatic wash produces a near-tie between keys that
share no notes.

The honest limitation is the relative major. A minor key and its relative major contain the
same seven notes, and a progression that touches neither leading tone is ambiguous to any
method, including this one. Camelot notation is forgiving here, since a key and its relative
share a number and mix anyway.

## Loops and rolls

Beat-locked loops on each deck: a row of lengths from half a beat to sixteen, halve and double,
loop in and out by hand, and a loop toggle. Clicking a length sets a loop; holding the same
button rolls instead.

| Decision | Why |
| --- | --- |
| An automatic loop starts on the beat *behind* the playhead | Snapping to the nearest beat lets a loop start a fraction late, and a loop that starts late is late for every bar it plays. That is the mistake the button exists to prevent |
| Loop bounds are computed on the message thread | Working them out needs the beat grid, and the grid lives behind a shared pointer. Taking a reference count is not a realtime operation, so the audio thread only ever sees two plain numbers |
| The wrap uses a modulo, not a subtract-until-inside loop | A very short loop could otherwise want hundreds of iterations inside one block, and the audio thread should not do an unbounded amount of anything |
| Loops are honoured on the stretcher's feed head too | With key lock on, that head chooses the audio. A loop that wrapped only the audible head would show the deck looping while it played straight through |
| A hand on the platter suspends the loop | Direct manipulation should never be fenced in by something set earlier |
| A roll clears its own loop when it ends | It was the roll's doing. Leaving it enabled would silently trap the deck |

A roll differs from a loop in one way that matters: underneath it the track keeps running, so
letting go drops you where the music got to rather than where the loop left off. That is what
makes a roll usable mid-phrase without losing the mix, and it is what the shadow playhead in
`Deck::processBlock` is for.

The loop actions are in the registry as `loop.in`, `loop.out`, `loop.toggle`, `loop.beats`,
`loop.roll`, `loop.halve`, `loop.double` and `loop.reloop`, so the DJ-202's second pad row can
be mapped to them without touching any engine code. `loop.beats` and `loop.roll` take a slot,
which `loopBeatsForSlot` turns into a length.

## Beyond milestone 1

| Item | Status | Notes |
| --- | :---: | --- |
| Slip mode | ⬜ | After a scratch the track carries on from where the hand left it, rather than catching up to where it would have been. The DJ-202 has a button for it on note 0x07 |
| Key detection | ✅ | `src/analysis/KeyDetector.*`, shown in the browser's Key column. See above |
| Four decks | ⬜ | `AudioEngine::numDecks` is a constant the mixer sizes itself from, so the engine mostly follows. The interface and the DJ-202 deck-toggle button are the work |
| Loops and loop rolls | ✅ | `Deck::setLoopBeats` and friends, with a loop row on each deck. See above |
| Effects | ⬜ | Filter, echo, reverb. The DJ-202 effects section is on MIDI channels 9 and 10, unmapped |
| Sampler | ⬜ | The DJ-202 pads send sampler notes on 0x21 to 0x30, unmapped |
| Record the master output | ⬜ | Straightforward: tap the master buffer in `AudioEngine` |
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
| Is the service running? | `Get-Service GiteaRunner` |
| Start or restart it | `Restart-Service GiteaRunner` |
| What did it say? | `Get-Content C:\gitea-runner\logs\runner.err.log -Tail 40` |

The runner writes its ordinary progress to stderr, so `runner.err.log` is the interesting file
and `runner.log` stays empty. An empty `runner.log` is not a sign that anything is wrong.

A last result of `0xC000013A` means it was killed by the session ending rather than crashing.

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

One is already registered as `INTERHOME-windows`, running as a Windows service, so Windows CI
works whether or not anyone is signed in. To add another, or to replace it: the
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

Then make it unattended:

```
pwsh scripts/install-runner-service.ps1
```

That asks for Administrator, installs NSSM, and runs the runner as the `GiteaRunner` service on
automatic startup, disabling the logon task so only one runner is ever live. `-Uninstall` undoes
it and puts the task back.

| Detail | Why |
| --- | --- |
| NSSM rather than `sc.exe` | The runner is a console program and does not speak to the service control manager. Installed directly, Windows would keep reporting it as failing to start while it worked perfectly. NSSM runs it as a child and does the service protocol for it |
| It runs as `LocalSystem` | So it needs no stored password. That is a privileged account, and jobs it runs are equally privileged, which is worth knowing before pointing this repository's CI at code you have not read |
| Logs in `C:\gitea-runner\logs` | Rotated at 10 MB. The service restarts itself five seconds after any exit |
| Job workspaces in `C:\gitea-runner\work`, set in `config.yaml` | Not a tidiness preference. See below |

The workspace location is the one setting here that must not be reverted to its default. A
service running as `LocalSystem` has its home under `C:\Windows\System32`, which is where the
runner would otherwise put each job. Visual Studio ships a **32-bit** CMake, and a 32-bit process
reading a path under `System32` is silently redirected by WOW64 to `SysWOW64`, where the checkout
is not. What you see is CMake reporting that the source directory does not exist, eleven seconds
into a job whose checkout plainly succeeded into that exact directory. Nothing in the message
hints at the cause.

### Reading a failed job's log

The web endpoint serves it without a token, which is quicker than clicking through the UI:

```
curl https://git.interdo.me/interdome/opendj/actions/runs/<run>/jobs/<job>/logs
```

Get both ids from `/api/v1/repos/interdome/opendj/actions/runs/<run>/jobs`. The API's own
`/logs` route needs a token; the web one does not.
