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
| Windows | ✅ | ✅ | Built and run with MSVC 19.44. WASAPI by default, ASIO opt-in. No CI job: checked by building there directly, see below |
| Fedora | ✅ | ✅ | Built and run on Fedora 44 with GCC 16, on a DJ-202's own four-channel interface through PipeWire's JACK. Decks, platters, browser, analysis and the library all exercised |
| Debian and Ubuntu | ✅ | ⬜ | Same, via the Debian CI job |
| Arch | ⬜ | ⬜ | `scripts/build.sh` knows the packages, untested |
| macOS | ⬜ | ⬜ | JUCE supports it; nothing has been tried |

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

## Recording a set

The Record button in the bottom bar writes the master output to a 24-bit WAV in the user's
music folder, named for the date and time, with a tracklist in a text file beside it.

| Decision | Why |
| --- | --- |
| Tapped after the mixer, before the device | So the file holds exactly what the room heard: crossfader, master gain, soft clip and all |
| Written through JUCE's `ThreadedWriter` | The audio thread copies a block into a FIFO and returns. Encoding and disk writes happen on a background thread, so a slow disk never stalls the callback |
| A full FIFO drops samples rather than blocking | That is the right way round: the recording loses them, the room does not. But the count is kept and reported |
| Dropouts are said out loud | In the status bar while it happens, in the dialog when it stops, and in the tracklist file, which is the only one still there tomorrow. A set with a hole in it must not look complete |
| The tracklist is timed against the recording | Not the wall clock, so it stays right even if samples were dropped |
| Whatever is already playing seeds the list | Recording usually starts a minute into the first track. A list beginning with the second record is missing the one people ask about |

Verified end to end in the running application, not only in tests: eight seconds of playback
recorded to a 48 kHz 24-bit stereo file that reads back at the right length with real audio in
it, and a tracklist beside it.

## The echo

One knob per channel, with a length in beats beside it. At zero it is silent and out of the way;
turning it up raises the wet level and the feedback together, which is how a DJ echo is used.

| Decision | Why |
| --- | --- |
| The delay is fed even at zero wet | So turning the knob up brings in repeats of what just played, rather than silence followed by a burst once the line fills |
| The length is smoothed, not stepped | Changing beat division sweeps the repeats the way a tape delay does. That is an effect in its own right and the one people reach for |
| Feedback is capped below one | A mixer that can be left self-oscillating will be |
| It sits after the EQ and filter | Which is where a send is on a DJ mixer: it repeats whatever you shaped, not the raw track |
| The engine sets the time, not the mixer | Tempo lives on the deck, and the mixer has no idea decks exist. A deck with no beat grid falls back to half a second, which is a musical guess rather than a silent failure |

The tests measure the output rather than checking the code ran: a click goes in, and the level
is sampled where each repeat is due and halfway between. Loud on the beat and quiet between is
what having the right delay length means, and neither half alone would show it, since a wash is
loud everywhere and silence is quiet everywhere.

## The reverb

One knob per channel, under the echo. At zero it is silent, and turning it up moves the channel
from dry into a room without changing its level.

| Decision | Why |
| --- | --- |
| The reverb runs at full wet into its own buffer | The knob is then a gain on that buffer. It cannot click, and turning it down leaves a tail ringing out instead of cutting it off mid-decay |
| It runs a block at a time, not a sample at a time | `juce::Reverb` is written that way, so the mixer does its EQ, filter and echo per sample into a shaped buffer, reverberates that buffer, and mixes the two in the final pass |
| The room is fixed and fairly large | A DJ reverb is one gesture, not a plugin. One knob that always sounds like the same room is more useful behind a mix than five that need setting up |
| It sits after the echo | So repeats fall into the room, which is the order a send chain has on hardware |

The tests measure the output. A burst goes in and the level is read after it stops: silent at
zero, ringing on afterwards, quieter later than earlier rather than sustaining, still ringing
after the knob comes down, and nothing crossing from one channel into the other.

## The sampler

Eight slots holding short sounds, triggered over whatever the decks are doing. Load one by
clicking an empty pad, dropping a file on it, or taking Load from its menu. The number row on
the keyboard fires the eight pads.

| Decision | Why |
| --- | --- |
| It joins the master after the mixer | A sample is laid over the mix, not mixed into it. Running it through a channel would put it behind the crossfader, where a stab is useless |
| A slot is a decoded buffer, swapped in atomically | The same handover the decks use, so loading a pad mid-set cannot stall the audio thread. The displaced sound is freed a few blocks later |
| Triggering restarts rather than queues | Retriggering is what a sampler is for. The envelope carries across a retrigger, so tapping a pad fast does not chop the tail of each tap |
| Every start and stop is faded over four milliseconds | Long enough to swallow the step a sound cut mid waveform would make, short enough that a stab still sounds like a stab |
| Slots are resampled with the same interpolator as the decks | A 44.1 kHz sample out of a 48 kHz device is being resampled whether anyone thinks of it that way. Aliasing that is inaudible on its own is audible over a mix |
| Sixty seconds is the limit | A sampler holds stabs and loops. Anything longer is a track, and the answer is a deck |

Sixteen tests cover it, measuring output rather than checking that code ran: silent until
triggered, a one shot that ends itself, a loop that does not, a retrigger that goes back to the
start, a stop that fades, both gains scaling, the headphone send, two slots at once, and a
44.1 kHz sound playing for its own length out of a 48 kHz device rather than eight per cent
fast.

## Four decks

Decks A to D, four strips on the mixer, and two decks on screen at a time. The button beside a
deck's clock swaps it for the one behind it: A for C on the left, B for D on the right.

| Decision | Why |
| --- | --- |
| Two decks on screen, not four | Four side by side leaves each one too narrow to read a waveform on, and reading the waveform is what a deck is for. The swap button costs one click and keeps both of them full size |
| The crossfader is assignable per channel | With four channels, A and B are no longer the only answers. C and D default to running past the crossfader, which is how a third deck is nearly always used |
| Sync follows the nearest playing deck with a grid | With two decks "the other one" needed no definition. With four it is the whole feature, and a deck that is stopped or ungridded is not what anybody means |
| Q, W, O and P follow the side, not the deck | They mean the deck on the left and the deck on the right, so the keys keep working after a swap. A cue key held across a swap is released against the deck it was pressed on |
| The engine needed almost nothing | `AudioEngine::numDecks` was already `Mixer::numChannels`, and both the mixer and the engine loop over their strips. Raising the constant did most of it, which is what that constant was for |

Five tests cover the mixer half: that there is a strip per deck, that C and D ignore the
crossfader by default, that any channel can be put on either side of it, that the defaults read
back, and that all four channels reach the master at once rather than two of them being dropped.

## What a mapping can reach

Everything a person can do is in the action registry, named once and reachable from a mapping
file by that name. Two checks run over the registry itself: that every action has a name, and
that every name finds its action again. An action added without its entry would otherwise be
unreachable from hardware and nothing would say so.

| Action | What it takes | Notes |
| --- | --- | --- |
| `deck.select` | `deck` | Puts that deck on screen in place of the one it shares a side with |
| `deck.swap` | nothing | Swaps both sides at once: A and B out, C and D in, or back. What a single deck-toggle button on a controller means |
| `mixer.crossfader_assign` | `deck`, `slot` | Which side of the crossfader a channel answers to: slot 0 the A side, 1 neither, 2 the B side |
| `sampler.trigger` | `slot` | Starts that pad. With shift held it stops it instead |
| `sampler.stop` | `slot` | |
| `sampler.gain` | value | The level of the whole sampler |

## What is remembered

Until now nothing survived a restart except the library, so every launch meant picking the
audio device again. `src/app/Settings.*` keeps a session in `settings.json` beside the library
database, with the audio device in JUCE's own `audio-device.xml` next to it.

| Kept | Not kept |
| --- | --- |
| The audio device chosen by hand | What was on the decks, and where in it |
| Master, phones and cue mix levels | The channel faders |
| Crossfader curve and the per-channel assignment | The crossfader position |
| Tempo fader ranges | EQ, filter, echo and reverb settings |
| Which decks are on screen | |
| The sampler pads, their files, loops and gains | |
| The window's size and position | |

| Decision | Why |
| --- | --- |
| Decks and faders are deliberately not restored | An application that reopens playing where it crashed, or with a fader somewhere the user cannot see, is worse than one that starts quiet |
| A device chosen by hand beats the device search | The search exists to make a good first guess, not to overrule somebody who already answered the question |
| Every field falls back on its own | A file from an older build, or one edited by hand into nonsense, loses only the settings it got wrong. Values out of range are brought back into it rather than refused |
| Pads are reloaded from their files, not stored as audio | A file that has moved leaves its pad empty, which is the truth, instead of a pad that looks loaded and plays nothing |
| Written to a temporary file and moved into place | An interrupted write leaves the previous settings rather than half of the new ones |
| Plain JSON | A setting can be corrected by hand when the interface for it does not exist yet |

Eight tests cover it: both round trips, a missing file, a half damaged file falling back field by
field, values out of range being clamped, the state reaching a mixer and a sampler and being
read back off them, and a save over an existing file leaving no temporary behind.

## The crash on exit, and what it was

Closing the window ended the process with an access violation, `0xC0000005`. Fixed. Worth
writing down because the method got there faster than reading code would have.

Windows records the faulting offset in the Application event log, and
`llvm-symbolizer --obj=OpenDJ.exe` turns image base plus that offset into a file and a line.
It named `Mixer::processBlock` every time, which said the fault was on the audio thread rather
than anywhere near the close button.

The fault itself: `processBlock` took its block length from the master buffer alone and trusted
it for every other buffer in the function. A device handing over a block longer than `prepare`
was told to expect wrote past the end of the strip buffers. It now takes the length every
buffer involved can actually take, and a deck handing over a short buffer is dropped rather
than read past the end of.

Two other things were fixed on the way, both real and neither the cause:

- The window's bounds were read in the destructor, from a `DocumentWindow` already half torn
  down. They are noted on the timer instead.
- Shutdown is now explicit and ordered, in `AudioEngine::stop`: the callback and device go
  first, then the loader and separator pools, then a recording still open is closed. The
  destructor calls it, and so does the shell before anything else is freed.

Verified by running the application and closing it: exit code 0, settings written, and the
window reopening where it was left.

## Picking a device worth playing on

Running it turned up something worse than the crash: the default device opened as DirectSound at
2560 samples, 87 ms out. That is a beat and a half of slack between a hand and the sound, and no
amount of care above it can make up for it. Startup also took eight to twelve seconds, nearly
all of it scanning devices.

The search took JUCE's own order of device types, which on Windows puts DirectSound first.
DirectSound is the one backend on the machine that cannot do low latency. Types are now ranked
and tried best first: ASIO, JACK and CoreAudio, then a low latency mode, then shared WASAPI or
ALSA, with DirectSound last. The last resort asks the best type for its own default by name
rather than taking whatever JUCE started on, and `tightenBufferSize` asks a device sitting on
something enormous for about 256 samples instead.

Measured on Windows, on the same machine, before and after:

| | Before | After |
| --- | --- | --- |
| Device type | DirectSound | Windows Audio (Low Latency Mode) |
| Buffer | 2560 samples | 336 samples |
| Output latency | 87 ms | 7.0 ms |
| First start | 8 to 12 s | 12 s |
| Later starts | 8 to 12 s | 0.25 s |

Asking by name also settled the one thing left open about the settings file: JUCE writes device
state for a device that was asked for, so the device is now remembered, and a second launch
reopens it and skips the scan entirely. That is where the quarter of a second comes from.

## Beyond milestone 1

| Item | Status | Notes |
| --- | :---: | --- |
| Slip mode | ✅ | `Deck::setSlipEnabled`, sharing the shadow playhead with loop rolls. Action `deck.slip_toggle` for the DJ-202's note 0x07 |
| Key detection | ✅ | `src/analysis/KeyDetector.*`, shown in the browser's Key column. See above |
| Four decks | ✅ | Four decks, four mixer strips, a crossfader assignment per channel and a swap button per side. See above. Actions `deck.select`, `deck.swap` and `mixer.crossfader_assign` are in the registry, so the DJ-202 deck-toggle button needs only a mapping entry |
| Loops and loop rolls | ✅ | `Deck::setLoopBeats` and friends, with a loop row on each deck. See above |
| Effects | ✅ | A filter, a beat-synced echo and a reverb on every channel strip. The DJ-202 effects section is on MIDI channels 9 and 10, still unmapped |
| Sampler | ✅ | `src/core/Sampler.*`, eight slots on a row of pads under the browser. See above. Actions `sampler.trigger`, `sampler.stop` and `sampler.gain` are in the registry; the DJ-202 pads send sampler notes on 0x21 to 0x30 and need only a mapping entry |
| Record the master output | ✅ | `src/core/SetRecorder.*`, with a tracklist written beside the audio. See above |
| Stem separation | ✅ | `src/analysis/StemSeparator.*` and `StemDsp.*`, with a knob per stem on each deck |
| Settings that survive a restart | ✅ | `src/app/Settings.*`, in `settings.json` beside the library. See above |
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
finished run and use its re-run button. Neither needs a commit.

| Job | Runs on | Status |
| --- | --- | :---: |
| fedora | `fedora:latest` container on the Linux runner | ✅ |
| debian | the Linux runner's own image | ✅ |
| windows | removed | ⬜ |

### Why there is no Windows job

There was one, it worked, and it was removed anyway. The only Windows runner available was a
desktop someone works on, and a five minute MSVC build landing on it for every push made that
machine unusable to type on. CI that costs more than it catches is not worth running.

Windows is still checked, by building and running the tests there directly, which is what has
actually been catching things: the `NOMINMAX` failure, the MP3 decode bug and the loop work were
all found that way rather than by a CI tick.

To bring it back, on a machine nobody is working on, see below. The job itself is kept as a
comment at the foot of `.gitea/workflows/build.yml` so it can be pasted back in one piece.

Two things about the remaining jobs are worth knowing before changing them.

| Detail | Why |
| --- | --- |
| The Fedora job installs `nodejs` and `git` before anything else | `actions/checkout` is a JavaScript action, and the stock Fedora image has neither. Without that step the job fails in two seconds, long before a compiler is involved |
| A Windows job, if restored, should call `scripts/build.ps1` rather than CMake directly | The script finds the CMake, Ninja and MSVC environment inside Visual Studio Build Tools, so a self-hosted machine needs nothing on PATH but git and node |

The README carries a status badge per branch. Gitea's badges are per workflow rather than per
job, so one badge covers both platforms and goes red if either fails.

### If a restored Windows job misbehaves

The runner is a console program on a desktop machine, so it goes down when the session that
started it does, and a job already in flight is then left with no runner and fails for reasons
that have nothing to do with the build. Check the machine before reading the log:

| Check | Command |
| --- | --- |
| Is the service running? | `Get-Service GiteaRunner` |
| Start or restart it | `Restart-Service GiteaRunner` |
| Stop it eating the machine | `Stop-Service GiteaRunner; Set-Service GiteaRunner -StartupType Manual` |
| What did it say? | `Get-Content C:\gitea-runner\logs\runner.err.log -Tail 40` |

The runner writes its ordinary progress to stderr, so `runner.err.log` is the interesting file
and `runner.log` stays empty. An empty `runner.log` is not a sign that anything is wrong.

A last result of `0xC000013A` means it was killed by the session ending rather than crashing.

### Registering a Windows runner

There is no Windows job to run at the moment, so this is only needed if one is restored. The
Linux runner advertises `ubuntu-latest` and has no MSVC, so it will never take a Windows job.
On a machine nobody is working on, with Visual Studio Build Tools, git and Node installed:

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
