# OpenDJ roadmap

Where the project is and what to pick up next. Anything ticked has tests or a verified manual
check behind it, not just code that compiles. The suite is **273 tests** on Linux and 272 on
Windows, the one difference being a stderr test whose stand-in for ffmpeg is a shell script;
anything touching audio is tested by measuring the output, not by checking that the code ran.

| Symbol | Meaning |
| :---: | --- |
| ✅ | Done and verified |
| 🚧 | Partly done, details below |
| ⬜ | Not started |

## Where it stands

Milestone 1 was a two deck core you could genuinely mix on. It is done, and so is everything
that was listed after it except video.

| # | Item | Status | Where it lives |
| :---: | --- | :---: | --- |
| 1 | Build system, GPLv3, CI | ✅ | `CMakeLists.txt`, `scripts/`, `.gitea/workflows/` |
| 2 | Audio device setup, master and cue routing | ✅ | `src/core/AudioEngine.*`, `src/core/OutputRouter.*` |
| 3 | Decks: load, play, pause, cue | ✅ | `src/core/Deck.*` |
| 4 | Tempo fader | ✅ | `src/core/Deck.*`, `src/ui/DeckComponent.*` |
| 5 | Key lock, so tempo does not shift pitch | ✅ | `Deck::renderStretched`, via Rubber Band |
| 6 | Mixer: fader, three-band EQ, crossfader, cue | ✅ | `src/core/Mixer.*` |
| 7 | Scrolling and overview waveforms, coloured by frequency | ✅ | `src/ui/WaveformComponent.*` |
| 8 | BPM detection and beat grid | ✅ | `src/analysis/TrackAnalyser.*` |
| 9 | Sync: tempo and beat phase | ✅ | `AudioEngine::syncDeck` |
| 10 | Turntable platters, mouse drivable | ✅ | `src/ui/PlatterComponent.*`, `src/ui/AngleMath.h` |
| 11 | Hot cues | ✅ | `Deck::hotCuePressed` and friends |
| 12 | MIDI mapping engine, monitor, device picker | ✅ | `src/control/`, `src/ui/MidiSetupComponent.*` |
| 13 | Roland DJ-202 mapping | ✅ | `mappings/roland-dj-202.json`, verified on hardware |
| 14 | Track browser and library database | ✅ | `src/library/`, `src/ui/BrowserComponent.*` |

| Beyond milestone 1 | Status | Where it lives |
| --- | :---: | --- |
| Four decks, A to D | ✅ | Four mixer strips, a crossfader assignment per channel, a swap button per side |
| Loops and loop rolls | ✅ | `Deck::setLoopBeats` and friends |
| Slip mode | ✅ | `Deck::setSlipEnabled`, sharing the roll's shadow playhead |
| Key detection | ✅ | `src/analysis/KeyDetector.*`, in the browser's Key column |
| Effects: filter, echo, reverb | ✅ | `src/core/Mixer.*`, one knob each per channel |
| Sampler | ✅ | `src/core/Sampler.*`, eight pads |
| Mic input | 🚧 | `src/core/MicInput.*`, `src/ui/MicComponent.*`, with talkover and a stream-only switch. Tested by measuring the output; not yet tried with a real mic |
| Stem separation | ✅ | `src/analysis/StemSeparator.*`, `StemDsp.*`, a knob per stem |
| Record the master output | ✅ | `src/core/SetRecorder.*`, with a tracklist |
| Split output cue | ✅ | `src/core/OutputRouter.*`, headphones on a stereo interface |
| Settings that survive a restart | ✅ | `src/app/Settings.*` |
| Broadcasting to Icecast | ✅ | `src/stream/`, verified against a real Icecast 2.4.4 |
| Broadcasting to YouTube and Twitch | 🚧 | `src/stream/RtmpBroadcaster.*`, `RtmpConnection.*`. The loopback test against ffmpeg's own listener passes 20 runs in 20 on Linux and 5 in 5 on Windows; never tried against a real platform |
| MilkDrop visuals | ✅ | `src/visual/Visualizer.*`, `src/ui/VisualizerComponent.*`. projectM v4 on an offscreen context, in a window of its own and in the RTMP broadcast. EGL on Linux, WGL on Windows |
| Video | ⬜ | Playing video files, as opposed to drawing visuals. Very large. Probably a separate project |

## What to pick up next

| Item | Notes |
| --- | --- |
| DJ-202 pad modes on the hardware, then the rest | Loop, roll and sampler modes are mapped from Mixxx's DJ-202 script and need pressing on the controller. Cue loop, pitch play, slicer, the parameter buttons and the TR-S sequencer are not mapped |
| Arch and macOS | `scripts/build.sh` knows the Arch packages but has never been run there. macOS has never been tried |
| Video | Playing video files is unstarted, and probably its own project. The visualiser is separate and built |
| The visualiser on macOS | Linux and Windows have an offscreen context, from EGL and WGL. macOS needs the CGL equivalent, and nothing there has been tried at all |
| RTMP against a real YouTube or Twitch account | Verified so far only against ffmpeg's own loopback RTMP listener; the handshake has never reached an actual platform |

## Compared with VirtualDJ

The goal is that a VirtualDJ user can sit down at OpenDJ and already know how to use it. This is
what they would still reach for and not find, checked against the code rather than assumed. The
VirtualDJ column is a description of that product, not a measurement of it.

Some of it is partly there already: pitch bend through the jog wheel's nudge, Camelot notation in
`MusicalKey::toCamelot` that the browser does not yet show, and hot cues that a controller can
set but nothing on screen shows or triggers.

### Reached for in every set

| Missing | What VirtualDJ has | Where OpenDJ is |
| --- | --- | --- |
| Playlists and history | Playlists, virtual and filter folders, an automatic history of played tracks, a side list for lining up the next few | Folders only. The only tracklist is the one a recording writes |
| Hot cues on screen | Pads per deck with colours and names, and markers on the waveform | Controller only, never drawn |
| Beat jump and quantize | Jumps of 1 to 32 beats; cues and loops snap to the grid | None. Automatic loops already start on the beat |
| Beat grid editing | Adjust the BPM, move the downbeat, tap tempo | Detection only, so a wrong grid cannot be corrected |
| More effects | Dozens, including flanger, phaser, gate, bitcrusher, brake, backspin, echo out and stutter, plus reverse and censor buttons | Filter, echo and reverb per channel |
| Key tools | Key shift in semitones, key match between decks, Camelot display, harmonic hints in the browser | Detection only |
| Auto gain | Tracks levelled by measured loudness | A trim, with no analysis behind it |
| Automix | Unattended mixing through a playlist, beat matched | None |
| MIDI learn and more controllers | Hundreds of controllers mapped out of the box, and a mapper that learns a control from the hardware | One mapping, written by hand. Issue #4 |
| Recording formats | MP3, FLAC and others | WAV only |

### Expected by regular users

| Missing | Notes |
| --- | --- |
| A deeper sampler | More pads, banks, recording a sample from a deck, samples locked to tempo |
| More from stems | Separating the library ahead of time, stem pads such as vocal off or drums only, effects on one stem |
| Library import and metadata | iTunes and Music, rekordbox and Serato libraries; a tag editor; cover art; colours, ratings and comments |
| Saved cue points | Hot cues kept with the track in the library |
| Waveforms | Zoom, and a strip showing both decks' beats lined up. Colour by frequency is done |
| Sandbox | Previewing a later moment in the headphones while the master plays on |
| Keyboard mapper | Shortcuts of your own; today there are only Q, W, O, P and the number row |
| Master effects | Effects on the whole mix, and effect slots with parameters |
| Ableton Link | Tempo shared with other applications and devices |
| Timecode vinyl | Decks driven by real turntables or CDJs |

### Large, or not only up to us

| Missing | Notes |
| --- | --- |
| Video and karaoke | Already listed above as probably its own project |
| A visualizer for streams | Issue #3. A smaller step towards video: the RTMP output already carries a still frame that could become a live picture |
| Streaming services | Tidal, Beatport, SoundCloud and the rest need commercial agreements, which may not be open to a GPL project |
| Lighting | DMX and OS2L |
| Everything around it | A remote control app, scripting, VST effect plugins, skins, cloud sync |

## Platforms

| Platform | Builds | Runs | Notes |
| --- | :---: | :---: | --- |
| Windows | ✅ | ✅ | MSVC 19.44. Windows Audio low latency mode by default, ASIO opt-in. Visuals on a WGL context, and in a broadcast through a named pipe. No CI job, see below |
| Fedora | ✅ | ✅ | Fedora 44, GCC 16, on a DJ-202's own four channel interface through PipeWire's JACK |
| Debian and Ubuntu | ✅ | ⬜ | Builds and tests in CI; never run on a desktop |
| Arch | ⬜ | ⬜ | Packages known, untested |
| macOS | ⬜ | ⬜ | JUCE supports it; nothing has been tried |

## Rules worth knowing before changing the engine

| Rule | Why |
| --- | --- |
| Nothing in `src/core` includes a UI header | Keeps the engine testable without a display, and stops interface concerns leaking into audio code |
| The audio thread never allocates, locks or touches a file | Any of the three can stall a block and produce an audible dropout |
| Handing new data to the audio thread uses an atomic pointer swap | See `Deck::publish` and `Deck::cleanUp`. Copy that pattern rather than adding a mutex |
| Every input goes through `ActionDispatcher` | Mouse, keyboard and MIDI produce the same actions, so the interface and the controller cannot drift apart |
| Analysis results are immutable and shared by pointer | The interface can hold one as long as it likes without affecting the audio thread's lifetime rules |
| Anything touching audio gets a test that measures the output | That is what has caught the real bugs, including every one below |

---

# Design notes

Why things are the way they are, for whoever changes them next.

## Tempo detection

Checked against 60 tracks taken at random from a 13,000 track techno collection, with the BPM
Mixxx had already worked out as the comparison.

| Estimator | Agreed with Mixxx |
| --- | :---: |
| Autocorrelation peak alone | 44 / 60 |
| Pulse-train scoring and a tempo prior | **57 / 60** |

Nearly every failure of the first version was a ratio of exactly 4/3, such as 170.7 BPM reported
for a 128 BPM track, and it reported them at full confidence. Autocorrelation cannot tell a tempo
from three quarters of it, because the beat itself contaminates that lag. What fixed it:

- **A pulse train, not a correlation peak**, scored on its weakest quarter of pulses rather than
  the average. A train at 4/3 of the tempo hits a beat once every four pulses, and scoring the
  average lets hitting hard three times in twelve rescue it.
- **Aligned in windows of eight beats.** A rigid grid drifts off the beat over five minutes from
  the fraction of a frame the period is out by, and then scores the right tempo as a miss.
- **The bass band counts double**, because the kick is the beat, and **a tempo prior centred on
  130 BPM**, since nothing in the signal separates a tempo from half of it. Better one written
  assumption than the same guess spread through the search.
- **Confidence is the margin over the best rival that is not a rounding of the winner.** Median
  1.00 where it agrees with Mixxx, 0.73 where it does not, so the number is worth reading.

One of the three remaining disagreements is Mixxx reading 63 BPM for a track that is plainly 126.
To repeat the check, point the library at a folder, let it scan, and compare the `bpm` column.

## Key detection

Twelve pitch classes folded out of the spectrum, matched against the twenty-four
Krumhansl-Kessler profiles, and reported in the sharps-throughout spelling DJ software uses,
convertible to Camelot.

| Decision | Why |
| --- | --- |
| Only the middle two minutes are read | Intros and outros are usually just drums, which say nothing about key and pull the histogram towards noise. Reading a whole track costs five times as much and changes almost nothing |
| Bins more than 35 cents off a semitone do not vote | A bin between two semitones belongs to neither, and letting it vote smears the chromagram |
| Nothing below about C3 counts | Below that, semitones are closer together than the transform can resolve |
| Each frame is normalised before it is added | So a loud drop does not outvote the four quiet minutes that share its key |
| A key already in the tags wins | Detection fills a gap rather than overruling a person |

The honest limitation is the relative major: it shares all seven notes with its minor, and a
progression touching neither leading tone is ambiguous to any method. Camelot notation is
forgiving here, since a key and its relative share a number and mix anyway.

## Key lock

Rubber Band, fetched by CMake and built from its single compilation unit so its Meson build is
never invoked. GPLv2 or later, so licence compatible.

`Deck::feedStretcher` reads the track at its recorded speed and lets the time ratio carry the
tempo. The stretcher is built in `prepare`, so the audio thread never allocates one. It is
bypassed while scratching, and at exactly the recorded speed, where it would be a delay line
that appears and disappears with the fader.

Five tests in `tests/Deck_test.cpp` count zero crossings to measure the pitch that actually came
out, rather than checking that the code ran.

## Loops and rolls

Beat-locked loops per deck: lengths from half a beat to sixteen, halve and double, loop in and
out by hand, and a toggle. Clicking a length sets a loop; holding the same button rolls instead.

| Decision | Why |
| --- | --- |
| An automatic loop starts on the beat *behind* the playhead | Snapping to the nearest beat lets a loop start a fraction late, and then it is late for every bar it plays. That is the mistake the button exists to prevent |
| Loop bounds are computed on the message thread | They need the beat grid, which lives behind a shared pointer, and taking a reference count is not a realtime operation. The audio thread sees two plain numbers |
| The wrap uses a modulo, not subtract-until-inside | A very short loop could otherwise want hundreds of iterations in one block |
| Loops are honoured on the stretcher's feed head too | With key lock on, that head chooses the audio. Wrapping only the audible head would show a loop while playing straight through |
| A hand on the platter suspends the loop | Direct manipulation should never be fenced in by something set earlier |
| A roll clears its own loop when it ends | It was the roll's doing; leaving it on would silently trap the deck |

A roll differs from a loop in the one way that matters: underneath it the track keeps running, so
letting go drops you where the music got to. That is the shadow playhead in `Deck::processBlock`,
and slip mode shares it.

## Four decks

Decks A to D with a strip each, two on screen at a time. The button beside a deck's clock swaps
it for the one behind it: A for C on the left, B for D on the right.

| Decision | Why |
| --- | --- |
| Two decks on screen, not four | Four side by side leaves each too narrow to read a waveform on, and reading the waveform is what a deck is for |
| The crossfader is assignable per channel | With four channels, A and B are no longer the only answers. C and D default to running past it, which is how a third deck is nearly always used |
| Sync follows the nearest playing deck with a grid | With two decks "the other one" needed no definition. With four it is the whole feature |
| Q, W, O and P follow the side, not the deck | So the keys keep meaning left and right after a swap. A cue key held across a swap is released against the deck it was pressed on |

It shipped with decks C and D silent. See **What running it turned up**.

## Mixer and effects

The three band EQ is a Linkwitz-Riley crossover rather than stacked shelves, so a band at zero
is a true kill and the bands sum back flat at unity. Every gain is smoothed, so nothing clicks
even when a controller sends coarse steps. The master soft clips above 0.7 rather than applying
`tanh` everywhere, which used to cost about 8% of unity gain.

**Filter**: one knob per channel, centred and doing nothing there, a low pass sweeping down and
a high pass sweeping up. Both filters run at all times, transparent at their ends, so the knob
never switches type mid-signal and clicks.

**Echo**: one knob plus a length in beats. Turning it up raises wet level and feedback together,
which is how a DJ echo is used.

| Decision | Why |
| --- | --- |
| The delay is fed even at zero wet | So turning the knob up brings in repeats of what just played, not silence then a burst once the line fills |
| The length is smoothed, not stepped | Changing beat division sweeps the repeats the way a tape delay does, which is the effect people reach for |
| Feedback is capped below one | A mixer that can be left self-oscillating will be |
| The engine sets the time, not the mixer | Tempo lives on the deck, and the mixer has no idea decks exist. No beat grid falls back to half a second, a musical guess rather than a silent failure |

**Reverb**: one knob, under the echo.

| Decision | Why |
| --- | --- |
| It runs at full wet into its own buffer | The knob is then a gain on that buffer: it cannot click, and turning it down leaves a tail ringing instead of cutting it mid-decay |
| A block at a time, not a sample | `juce::Reverb` is written that way, so the strip is built per sample into a shaped buffer, reverberated, then mixed in the final pass |
| The room is fixed and fairly large | A DJ reverb is one gesture, not a plugin |

The chain is EQ, then filter, then echo, then reverb, which is the order a send chain has on
hardware: repeats fall into the room.

The tests measure output. For the echo, a click goes in and the level is sampled where each
repeat is due and halfway between: loud on the beat and quiet between is what the right delay
length means, and neither half alone would show it.

**Binding echo and reverb to the DJ-202's FX section.** The mixer has two independent effects,
each with its own permanent knob. The hardware has one shared DEPTH knob behind three FX-select
buttons, because it was built around a single onboard effects unit rather than two always-on
sends. What a DJ actually gets: FX1 arms echo and FX2 arms reverb, both lighting up to show which
one is currently armed; the DEPTH knob then always turns whichever of the two was armed last,
echo by default. FX3 and FX ON/TAP are recognised by the mapping but sent nowhere, because there
is no third effect for FX3 to mean and no separate bypass state for ON/TAP to flip: the knob
already turns an effect off at zero.

| Decision | Why |
| --- | --- |
| `mixer.fx_select` only changes which effect the knob reaches | It never touches the mixer itself, so pressing FX1 or FX2 cannot itself change a level, only where the next knob turn lands |
| The selection lives in `ActionDispatcher`, one small integer per channel, alongside `shiftHeld` and `tempoRangePercent` | It is exactly that kind of state: not audio, not persisted, read by the next action rather than stored on a deck or the mixer |
| Binding only CC 0x00 of the DEPTH knob | Measured on real hardware: the knob broadcasts the same value on 0x00, 0x01 and 0x02 at once, so binding all three would write the mapping three times for one hand movement |
| FX3 and FX ON/TAP left unmapped | OpenDJ has two effects, not three, and a continuous knob already has an off position; inventing a use for either button would be design bolted onto a limitation the hardware does not actually have on this engine |

## Waveforms

Each deck draws the same analysis twice: a detail view that scrolls past a stationary playhead
with the beat grid over it, and an overview of the whole track underneath. Both are coloured by
frequency, so the shape of a record can be read before it is heard.

The colour comes from three band energies stored on every waveform bucket alongside the minimum,
maximum and RMS that were already there. They are measured once, on a single pass over the
audio that serves both waveforms at once, by a Linkwitz-Riley crossover split at the same two
frequencies the mixer's EQ uses.

| Decision | Why |
| --- | --- |
| The bands are the mixer's own crossovers, 300 Hz and 3 kHz, shared from `Mixer` rather than chosen again | A waveform whose red does not mean what the Low knob reaches would be worse than no colour at all. Turning a band down and watching exactly that colour leave the waveform is the point |
| A crossover filter, not the tempo pass's FFT | The FFT runs at a 1024 point frame every 256 samples and would have to be interpolated onto 2 ms buckets. A filter is sample accurate by construction, and the bucket boundaries stay where the minimum and maximum already put them |
| One pass over the audio fills both waveforms | The filtering is the expensive part and the bucketing is nearly free, so running it once per resolution would have doubled the cost for nothing |
| A running total per bucket rather than an array of them | The pass walks the track in order, so only the bucket it is inside needs a total. A sixty minute track has about 1.6 million detail buckets, and three sums each would have cost 40 MB to avoid arithmetic that is already free |
| Nothing is cached, and `analysisVersion` is not bumped | Waveform peaks were never written to the library; only the tempo and key are. The extra pass is the one cost a track that has been analysed before now pays, and it is small beside the decode it already does |
| Each column's three bands are read against the largest of the three | Bass alone is red, bass and midrange yellow, a broad mix orange, a hi-hat pattern blue. How loud the passage is already sits in the height of the bar and does not need saying twice |
| Not against each band's own loudest moment in the track | Tried, and worse. A record's midrange sits near its own peak almost all the time while its bass and treble only touch theirs on a hit, so every track came out the same shade of green |
| The played part keeps its colour, desaturated and dimmed | It is the same music. A flat second colour behind the playhead threw away everything the colouring had just said |

## Sampler

Eight slots of short sounds triggered over whatever the decks are doing. Load by clicking an
empty pad, dropping a file on it, or the pad menu. The number row fires the eight pads.

| Decision | Why |
| --- | --- |
| It joins the master after the mixer | A sample is laid over the mix, not mixed into it. Through a channel it would sit behind the crossfader, where a stab is useless |
| A slot is a decoded buffer, swapped in atomically | The decks' handover, so loading a pad mid-set cannot stall the audio thread |
| Triggering restarts rather than queues, and the envelope carries across | Retriggering is the point. Tapping fast must not chop the tail of each tap |
| Four millisecond fades on every start and stop | Long enough to swallow the step of a sound cut mid waveform, short enough that a stab still sounds like one |
| The decks' interpolator, not a cheaper one | A 44.1 kHz sample out of a 48 kHz device is being resampled whether anyone thinks of it that way, and aliasing inaudible on its own is audible over a mix |
| Sixty seconds is the limit | Anything longer is a track, and the answer is a deck |

Sixteen tests, including a 44.1 kHz sound playing for its own length out of a 48 kHz device
rather than eight per cent fast.

## Mic input

A mic from the device's inputs, laid over the mix after the sampler. Up to two inputs are enabled
in Audio setup; the strip at the end of the sampler row turns the mic on, sets its level, meters
it, and holds the talkover and routing switches. A controller reaches it through `mic.toggle`,
`mic.gain` and `mic.talkover`.

| Decision | Why |
| --- | --- |
| Never on at launch, and whether it was on is not saved | An open mic in front of speakers howls, and nobody should get that from opening the application |
| It joins after the mixer and the sampler | No fader or crossfader should be able to take a voice away |
| Stream only builds a second mix, rather than taking the voice back out of the speakers | The recorder and both broadcasters read that mix and the speakers get the ducked music alone. With the mic off and talkover let go, the second mix is not built at all |
| Talkover is 10 dB, in 50 ms, back over half a second | Enough for a voice to sit on the music without stopping it, and slow enough coming back that a pause between sentences does not pump it |
| Two inputs are summed, not averaged | A mic is mono and inputs are offered in pairs, so a mic in either half of a pair arrives at full level |
| Its own soft clip above 0.9, only where a voice is added | The mixer rounds off above 0.7 before the mic arrives. Shaping the music a second time would cost it level, and a shout still has to be rounded off rather than torn |
| Inputs are copied before the outputs are cleared | In case a backend hands over input and output channels that share memory. A test points the mic at the engine's own output to prove it is still heard |
| The device search stays output-only | Opening inputs on every device it tries adds failure points and, on Windows, microphone permission prompts. The input is chosen once in Audio setup and kept with the device |
| Not sent to the headphones | A voice heard back a few milliseconds late is distracting, and the room and the stream are where it matters |

## Output routing and the cue bus

`src/core/OutputRouter.*` holds the rules, out of the device callback and testable on its own.

| Mode | Master | Cue |
| --- | --- | --- |
| Separate outputs | 1 and 2, stereo | 3 and 4, stereo, when the device has them |
| Split output | 1, mono | 2, mono |

A split is the trick that predates DJ interfaces: one stereo output, a splitter cable, one half
to the speakers and the other to the headphones. It costs stereo in both, and it is the only way
to pre-listen on a plain sound card, where the cue bus previously had nowhere to go at all.

| Decision | Why |
| --- | --- |
| Split is never the default | It puts a mono master into one speaker. It is a choice in Audio setup, and it is remembered |
| A split uses only outputs 1 and 2, whatever else the device has | A split is a statement about a cable, not about the hardware |
| Mono is the average of both sides, not their sum | A loud stereo mix would clip on the way down otherwise |
| Fewer than four outputs on separate pairs drops the cue | Putting it anywhere else would send the headphone feed to the room |
| The mode is applied before the device opens | So the first block is routed correctly and the startup line tells the truth |

Eleven tests: both modes against one, two and four outputs, inactive channels skipped, an
oversized block trimmed, and canaries proving nothing is written past the block.

## Broadcasting

The master output to an Icecast server, encoded as Ogg Vorbis, while you play. The same tap the
recorder uses, so listeners hear exactly what the room hears.

It needed **no new dependencies**. JUCE already bundles a Vorbis encoder in
`juce_audio_formats/codecs/oggvorbis` and a socket in `juce_core/network`, which between them
are the whole job.

| Decision | Why |
| --- | --- |
| The encoder writes to a socket through an `OutputStream` | That is the seam JUCE's Ogg writer already has. Encoding, buffering and the background thread come from `ThreadedWriter` unchanged, exactly as the recorder gets them |
| A full FIFO drops samples rather than blocking | The same bargain the recorder makes, and the reason it is the right one: the people in the room paid to be there, and the stream is what gives way |
| A reconnect rebuilds the stream rather than resuming it | Ogg carries its headers at the front, so a server joining halfway through one has nothing to decode. The backoff runs 1, 2, 4 seconds up to 30 while the audio carries on untouched |
| `PUT` first, then `SOURCE` | Icecast 2.4 and later want the HTTP verb; older servers and most Icecast-alikes only know the original one. Trying both costs a round trip on an old server and nothing on a new one |
| The request is built by a function that touches no socket | The handshake is the part most likely to be subtly wrong and the easiest to test if it is kept away from the network |
| Settings that cannot work are refused before a socket opens | A typo becomes a sentence rather than a timeout |

**Track titles do not appear on an Ogg mount.** Measured against Icecast 2.4.4, which answers
"Mountpoint will not accept URL updates". That is correct behaviour, not a fault: Ogg carries
metadata in band in its Vorbis comment header, and the admin URL exists for MP3 and Shoutcast
sources that have nowhere else to put it. The code is kept, and works the day an MP3 mount does.

**A broadcast lags the room**, by the encoder and the network. Vorbis fills an Ogg page before it
emits one, and a quiet passage fills it slowly: a pure tone at quality 5 took five seconds to
produce a couple of pages. That is normal for every internet radio stream.

Ten tests cover it, including one that stands a fake server on a loopback socket, broadcasts at
it, and checks the request arrived and Ogg pages followed. There is also a hidden `[.live]` test
for a real server; `tests/Broadcast_test.cpp` says how to run it.

Verified end to end against Icecast 2.4.4 in Docker: the source registered at 160 kbps, 48 kHz,
stereo, with its genre intact, and a listener pulled 40 KB of `audio/ogg` beginning with the
`OggS` marker while it was live.

## Broadcasting to YouTube and Twitch

The same master tap again, but to an RTMP target this time, which is what YouTube, Twitch and
most other streaming platforms actually take. Unlike Icecast this **does** need a dependency:
neither JUCE nor OpenDJ can produce an H.264 video track or speak the RTMP handshake, and both
are needed, since an RTMP ingest without a video track is not a stream it will accept. The
dependency is `ffmpeg`, run as a subprocess: OpenDJ pipes it raw 16-bit PCM over stdin, and it
does the encoding and the handshake.

| Decision | Why |
| --- | --- |
| `RtmpBroadcaster` mirrors `Broadcaster` almost exactly | Audio-thread tap into a `ThreadedWriter`, a FIFO, a background thread; a full FIFO drops samples rather than blocking, the same bargain the Icecast broadcaster and the recorder both make |
| The video track is a static frame with the stream title drawn on it, built entirely by ffmpeg's own `lavfi` and `drawtext` | The simplest thing that satisfies "needs an H.264 video track". Nothing in OpenDJ renders or ships an image for it. A live waveform or the album art is a real idea but a separate piece of work; see the note in `RtmpConnection.h` before reaching for one |
| A reconnect relaunches ffmpeg from scratch, never resumes it | FLV carries its header at the front the same way Ogg does, so a server joining halfway through one has nothing to decode |
| ffmpeg is found by `RtmpConnection::findFfmpeg`, not just called by the bare name on PATH | On the machine this was built on, `/usr/local/bin/ffmpeg` (no libx264) shadows `/usr/bin/ffmpeg` (has it) on PATH. Checks `OPENDJ_RTMP_FFMPEG` first, then a short list of usual install locations, then PATH, actually running `-encoders` against each candidate rather than trusting its presence. Refuses the broadcast outright if nothing on the machine can encode H.264, rather than launching a doomed one and failing opaquely later |
| The pipe is primed with a quarter second of silence before waiting for ffmpeg to confirm the connection | ffmpeg will not open its RTMP output, and so never prints the line that confirms it, until it has read a first packet from every mapped input. Nothing guarantees the audio device has called back even once by the time a broadcast is started; the silence is what makes confirmation possible at all rather than a race against the device |
| ffmpeg's stderr is drained on every write, not only while connecting | ffmpeg prints warnings for as long as it runs. Once nobody read them its stderr pipe filled, it blocked on that write and stopped reading stdin, while `Process::write` waited for it to: a deadlock between two idle processes. A test with a stand-in ffmpeg that floods stderr mid-broadcast fails after the ten second send timeout without this, and passes in a tenth of a second with it |
| The audio input is not probed: `-analyzeduration 0 -probesize 32` | By default ffmpeg reads up to five seconds of an input before opening its outputs, and prints `Output #0` only after that. The format, rate and channels are all given, so there is nothing to find out. Without it, a quarter second of priming and a wait for confirmation never connected |
| No `-shortest` | In ffmpeg 8 it holds every stream back until the others catch up. Audio arriving ahead of a real-time video track then means nothing is encoded and stdin stops being read, until the send timeout kills ffmpeg. It never stopped ffmpeg anyway; `Process::stop` sends a signal |
| On Windows, `drawtext` is given a font file | The Windows builds of ffmpeg carry fontconfig with no config file, and `drawtext` without a font crashes there rather than failing (exit 139 with ffmpeg 8.1.2). Segoe UI, then Arial; with neither, the title is left off |

**A real, previously undiagnosed bug found while finishing this feature**: `RtmpBroadcaster::start()`
had two failure branches that called `stop()` to unwind a half-open connection, and `stop()`
unconditionally resets the broadcaster's state to `offline`. Both branches set `state = failed`
*before* calling `stop()`, so the failure was silently clobbered back to `offline` every time —
the broadcaster would report itself idle rather than failed after an ffmpeg launch or handshake
failure. Fixed by reordering both branches to call `stop()` first and set the failure state
afterward. Verified with five consecutive isolated runs of the affected test, previously a
reliable reproduction.

**The loopback test's stall was three faults, not one.** The test starts a broadcast, points it
at ffmpeg's own RTMP listener on localhost, and checks a real video and audio track both arrive.
It failed most runs, and was hidden for a while because no cause had been found. Watched from
ffmpeg's side, with a wrapper logging each ffmpeg's stderr and exit, it came apart into the
stderr deadlock, the five second probe and `-shortest`, all three in the table above. The probe
alone failed every run on Ubuntu. Once it was gone, `-shortest` left ffmpeg connected but encoding
nothing, OpenDJ's send timeout killed it, and the listener, which takes one connection, had
already exited when the reconnect came. That looked like minutes of silence from OpenDJ's side.
With all three fixed it passes 20 runs in 20 on Ubuntu 26.04 with ffmpeg 8, about sixteen seconds
each, and 5 in 5 on Windows 11 with ffmpeg 8.1.2, and runs by default again; without a capable
ffmpeg it skips with a warning:

```
opendj-tests "[rtmp-loopback]"
```

Two more faults turned up running it on Windows, where it had never been built: the Windows half
of `RtmpConnection::Process` had no `lastDiagnostic`, so nothing compiled, and the `drawtext` crash
in the table above meant every broadcast relaunched ffmpeg into the same crash while reporting
itself live.

Also unverified: no real YouTube or Twitch account was available while writing this, so the
handshake has only been checked against ffmpeg's own RTMP listener on loopback, never against an
actual platform. The protocol is the same either way, but that is a claim, not a measurement.

## MilkDrop visuals

projectM v4 draws the MilkDrop presets from what the master output is doing, into the window and
into the broadcast's video track. **Not Butterchurn**, which is the obvious name to reach for and
the wrong one here: Butterchurn is JavaScript and WebGL, so putting it in a C++ application means
embedding a browser engine, offscreen rendering and a frame-capture path, to run the same `.milk`
presets that projectM already runs natively. LGPL 2.1 or later, so licence compatible.

| Decision | Why |
| --- | --- |
| The OpenGL context belongs to no window anyone sees | The obvious alternative, a `juce::OpenGLContext` on the panel, ties the visuals to the panel being open: close it and the broadcast's video track stops mid-set. A context of its own also lets the tests render real frames and measure real pixels on a machine with no display. Surfaceless EGL on Linux; on Windows, where there is no such thing, a window that is created and never shown |
| The panel is a picture of the newest frame, not a second GL context | The frame is read back off the GPU for the broadcast anyway, so drawing it as an image costs nothing extra and there is one renderer rather than two |
| The audio thread only copies into a lock free FIFO | Same rule as everything else that taps the master. `projectm_pcm_add_float` is a library call with no realtime promises, so the render thread makes it, not the audio thread |
| Frames are turned right way up in the visualiser | OpenGL hands rows back bottom first and both consumers want the opposite. Doing it once here beats doing it in the panel and again in an ffmpeg `vflip` nobody would think to look at |
| Presets are found the way mappings are, plus `/usr/share/projectM/presets` | A distribution's own preset package is then used without copying anything. None found anywhere is not an error: projectM has an idle preset built in |
| Presets are fetched by a script, not shipped in this repository | They were released over two decades by many authors, almost none under any stated licence, and they run to a hundred megabytes. `scripts/get-presets.sh` installs them on request, or `scripts/get-presets.ps1` on Windows |

**Frames read back sheared at 854 wide.** OpenGL pads every row of a `glReadPixels` to four bytes
unless told otherwise, and three bytes a pixel means that only matches a tightly packed frame when
the width divides by four. 1280, 1920 and the 320 the tests used all do, so this was invisible
until a broadcast was set to 480p: every row landed two bytes further along than the one above,
shearing the picture and sliding the colour channels out of order down it. One `glPixelStorei`
call, and a test at 854 wide that measures how much each row differs from the row above it, which
fails without the fix and passes with it.

Six tests, one of which is the shear check above and one of which measures that a frame is not
simply black, plus a loopback test in `RtmpBroadcast_test.cpp` that pushes moving frames at the
broadcast and checks what ffmpeg's receiver wrote: H.264 at the right size, the expected frame
count, and two frames from different points in the file that actually differ.

Verified end to end through the application itself at 854x480 and 25 fps: the receiver recorded
500 frames in 20 seconds, exactly 25 a second, and the frames carry real moving content rather
than the black the feeder starts with.

**Butterchurn's presets cannot be used, and do not need to be.** Butterchurn is the WebGL
MilkDrop implementation, and asking for its presets is the obvious request; its packs ship 1,737
of them. They are not `.milk` files. They are conversions, with the equations turned into
JavaScript (`a.fps_=`, `Math.min`, `div()`) and the shaders into GLSL carrying the `xlat_mutable`
markers of an HLSL translator. projectM reads the original MilkDrop expression language and HLSL,
so using them would mean reverse-translating two languages, one of them a shading language.

The originals are a better answer anyway. `scripts/get-presets.sh`, or `get-presets.ps1` on
Windows, installs the four packs those
conversions came from, 14,575 presets, against Butterchurn's 1,737. Matching the two sets by name:
about 80 per cent of Butterchurn's list is in those packs, and the roughly 350 that are not are
mash-ups that appear to exist only in its own collection. So it is a far larger library rather
than a strict superset, and that is worth saying plainly rather than claiming everything.

The same script installs the texture pack, and `projectm_set_texture_search_paths` is given the
folders it lands in. This matters more than it looks: 8,027 of Cream of the Crop's 9,795 presets
reference a sampler, and a preset that cannot find its image does not fail, it draws wrongly,
usually as a flat colour where the picture should be.

**Linux and Windows.** The offscreen context is EGL on Linux and WGL on Windows, behind one small
class with two implementations; everything above it, from the framebuffer to the readback to
projectM itself, is shared. Windows also needs GLEW, because projectM reaches OpenGL through it
there and never initialises it, so OpenDJ does, once, before projectM is created.

Live video reaches a broadcast on both. POSIX hands ffmpeg a second pipe on descriptor three;
Windows has no way to give a child a descriptor of its choosing, so it makes a named pipe and tells
ffmpeg the name. Bringing that up also turned up the reason Windows had been reporting a broadcast
to an unreachable server as live: it had no stderr pipe, so "still running" was all it could check.
It has one now, read by a thread of its own, and waits for the same `Output #0` the POSIX side does.

macOS is what is left: it needs the CGL equivalent of that context class.

## Picking a device worth playing on

The default device used to open as DirectSound at 2560 samples, 87 ms out. That is a beat and a
half between a hand and the sound, and no amount of care above it makes up for that.

Device types are now ranked and tried best first: ASIO, JACK and CoreAudio, then a low latency
mode, then shared WASAPI or ALSA, with DirectSound last, since it is the one backend on a
Windows machine that cannot do low latency. JUCE's first open happens before the types can be
listed at all, so the manager is moved onto the best type immediately afterwards. The last resort
asks the best type for its default *by name* rather than taking whatever JUCE started on, and
`tightenBufferSize` asks a device sitting on something enormous for about 256 samples.

| | Before | After |
| --- | --- | --- |
| Device type | DirectSound | Windows Audio (Low Latency Mode) |
| Buffer | 2560 samples | 336 samples |
| Output latency | 87 ms | 7.0 ms |
| Second start | 8 to 12 s | 0.25 s |

Asking by name also settled the device being remembered: JUCE writes state for a device that was
asked for, so a second launch reopens it and skips the scan, which is where the quarter of a
second comes from. `AudioEngine::preferenceForDeviceType` is public and has three tests on it,
because a build that quietly went back to DirectSound would sound broken everywhere and nothing
would fail.

## Library and browser

| Piece | Notes |
| --- | --- |
| SQLite schema | `src/library/Library.cpp`: folders, tracks, tags, tempo, and an analysis version, so a better analyser quietly invalidates old results |
| Analyse once | `AnalysisCache`, asked before anything is decoded. `Deck::loadFile` takes a `KnownTrack` and skips the tempo pass |
| Scanning | A quick tag pass so the browser fills in seconds, then analysis at low priority, written as it goes so it can be stopped and resumed |
| Tag reading | `TagReader.cpp` parses ID3v2.2 to 2.4 and ID3v1 directly, plus MP3 length from the Xing header. Opening a decoder for 13,000 files just to ask their length is far too slow |
| Browser | Search, sortable columns, folder filter, double-click or drag onto a deck, and `browse.scroll` so the controller's encoder moves the selection |

## Recording a set

The master output to a 24-bit WAV in the user's music folder, with a tracklist beside it.

| Decision | Why |
| --- | --- |
| Tapped after the mixer, before the device | So the file holds exactly what the room heard: crossfader, master gain, soft clip and all |
| Written through JUCE's `ThreadedWriter` | The audio thread copies into a FIFO and returns; encoding and disk writes happen elsewhere |
| A full FIFO drops samples rather than blocking | The recording loses them, the room does not. The count is kept and reported: status bar, closing dialog, and the tracklist file, which is the one still there tomorrow. A set with a hole must not look complete |
| The tracklist is timed against the recording, and seeded with whatever is already playing | The wall clock would drift if samples were dropped, and recording usually starts a minute into the first track, so the list would otherwise begin with the second record |

## What is remembered

`src/app/Settings.*` keeps a session in `settings.json` beside the library database, with the
audio device in JUCE's own `audio-device.xml` next to it.

| Kept | Not kept |
| --- | --- |
| The audio device, and the cue routing mode | What was on the decks, and where in it |
| Master, phones and cue mix levels | The channel faders and the crossfader |
| Crossfader curve and per-channel assignment | EQ, filter, echo and reverb settings |
| Tempo fader ranges, which decks are on screen | |
| The sampler pads, their files, loops and gains | |
| The mic's level, talkover and routing | Whether the mic was on |
| The window's size and position | |

| Decision | Why |
| --- | --- |
| Decks and faders are deliberately not restored | An application that reopens playing where it crashed, or with a fader somewhere the user cannot see, is worse than one that starts quiet |
| A device chosen by hand beats the device search | The search makes a good first guess; it does not overrule somebody who already answered |
| Every field falls back on its own | An older or hand-edited file loses only the settings it got wrong. Out of range values are clamped rather than refused |
| Pads reload from their files | A file that moved leaves its pad empty, which is the truth, rather than a pad that looks loaded and plays nothing |
| Written to a temporary file and moved into place | An interrupted write leaves the previous settings, not half of the new ones |
| Plain JSON | A setting can be corrected by hand before an interface for it exists |

## What a mapping can reach

Everything a person can do is in the action registry, named once and reachable from a mapping
file by that name. Two tests walk the registry: every action has a name, and every name finds its
action. An action added without its entry would be unreachable from hardware and nothing else
would say so.

| Action | Takes | Notes |
| --- | --- | --- |
| `deck.select` | `deck` | Puts that deck on screen in place of the one it shares a side with |
| `deck.swap` | nothing | Swaps both sides at once. What a single deck-toggle button means |
| `mixer.crossfader_assign` | `deck`, `slot` | 0 the A side, 1 neither, 2 the B side |
| `sampler.trigger` | `slot` | Starts that pad; with shift, stops it |
| `sampler.stop` | `slot` | |
| `sampler.gain` | value | The level of the whole sampler |
| `pad.mode` | value | The pad mode a controller reports, as the code in its velocity, stored per deck |
| `pad.loop` | `deck`, `slot` | Pads 1 to 4: a loop of 1, 2, 4 or 8 beats, or in roll mode a roll of 1 to 1/8 of a beat while held |
| `mic.toggle` | nothing | Turns the mic on or off |
| `mic.gain` | value | The mic's level; 0.5 is unity |
| `mic.talkover` | nothing | Turns talkover on or off |

Plus the loop family: `loop.in`, `loop.out`, `loop.toggle`, `loop.beats`, `loop.roll`,
`loop.halve`, `loop.double`, `loop.reloop`. `loop.beats` and `loop.roll` take a slot, which
`loopBeatsForSlot` turns into a length.

---

# What running it turned up

Three real faults that no test caught, all found by building the thing and using it. They are
kept together because they share a lesson: the audio path had no seam a test could reach without
a sound card.

## Decks C and D were silent

From the day four decks landed. The device callback built its deck views from a two element list
written when there were two decks, so views 2 and 3 were default constructed with no channels and
no samples. Both extra decks rendered into nothing, and the mixer was handed pointers to empty
buffers.

| Change | Why |
| --- | --- |
| `AudioEngine::prepareToPlay` and `renderNextBlock` are public | The device callback is one caller, not a privileged one. An offline render or a plugin wrapper wants the same seam, and a test needs it |
| Views are built in a loop over `numDecks` | A list written by hand goes stale when a constant changes |
| Pointed at storage with `setDataToReferTo` | Assigning an `AudioBuffer` copies, and copying allocates. On the audio thread that is its own bug |
| Six tests in `tests/AudioEngine_test.cpp` | Every deck is loaded with a tone and required to reach the master. The old two element list was put back to confirm they fail, so they are known to catch this rather than merely to pass |

## The crash on exit

Closing the window ended the process with an access violation, `0xC0000005`, and it was the same
bug wearing a different hat: `Mixer::processBlock` took its block length from the master buffer
and trusted it for every other buffer, including the empty ones above. It now takes the length
every buffer involved can actually take, and a deck handing over a short buffer is dropped.

The method is worth keeping. Windows records the faulting offset in the Application event log,
and `llvm-symbolizer --obj=OpenDJ.exe` turns image base plus that offset into a file and a line.
It named `Mixer::processBlock` every time, which said the fault was on the audio thread rather
than anywhere near the close button.

Two other real faults were fixed on the way, neither the cause:

- The window's bounds were read in the destructor, from a `DocumentWindow` already half torn
  down. They are noted on the timer instead.
- Shutdown is now explicit and ordered in `AudioEngine::stop`: callback and device first, then
  the loader and separator pools, then any open recording.

## MP3 tracks were refused

JUCE over-reports the length of an MP3, so the last 93 to 189 ms failed to read and the whole
track was discarded as undecodable. `TrackDecoder` now reads in chunks and halves the chunk on
failure, keeping everything that does decode. A track whose reported length overruns what exists
plays, which is the honest answer.

---

# The Roland DJ-202

Measured on the hardware rather than taken from the documentation.

| Check | Status | What was found |
| --- | :---: | --- |
| Note off behaviour | ✅ | Both forms occur, depending on the sequencer's MIDI version: as a UMP client releases arrive as note off, as a legacy client as note on with velocity zero. A release is taken from the message rather than the mapping, and a note off falls back to the note on control of the same number |
| Jog tick rate | ✅ | 800, not the 512 the Mixxx mapping states: two turns produced 1590 ticks |
| Platter encoding | ✅ | Controller 6, relative, centred on 64. The wheel also streams 14-bit absolute position as pitch bend, but that flows whenever a hand merely rests on it, so it is deliberately unmapped |
| Pad modes | 🚧 | The mode arrives as the velocity of note 0x00 on each pad channel, and pads 1 to 4 send the same notes in loop and roll mode, so `pad.mode` stores it and `pad.loop` reads it. Codes and notes taken from Mixxx's DJ-202 script (`github.com/mrtnGLSR/DJ-202`), not yet pressed on the hardware |
| Tempo fader polarity | ✅ | Controller 9 coarse, 59 fine. The top of the travel reports 0x3FFF and the bottom 0, and Roland prints the + at the bottom, so `"inverted": true` is right: the fast end has to come out as 1. Pinned by a test on the shipped mapping |

## The one that cost the most: controller 6 and MIDI 2.0

JUCE registers its ALSA sequencer client as MIDI 2.0. On a kernel that knows about UMP, the
sequencer translates every legacy message before handing it over, and MIDI 2.0 reserves
controller 6 as Data Entry, the middle of an RPN sequence, rather than a controller in its own
right. A bare controller 6 is swallowed in translation.

The DJ-202's platters report on controller 6. The touch was seen and the turning was not, so the
deck stopped dead under the hand. Reproducible with nothing but `aseqdump`:

```
aseqdump -u 0   ->  controller 6 arrives, controller 7 arrives
aseqdump -u 2   ->  controller 6 is gone, controller 7 arrives
```

`src/control/AlsaMidiCompat.cpp` defines the weak symbol JUCE uses to ask for MIDI 2.0, so the
client stays at legacy MIDI 1.0. Delete it once JUCE lets an application choose its own client
MIDI version.

---

# Working on this

| Task | Windows | Linux |
| --- | --- | --- |
| Install dependencies | comes with VS 2022 Build Tools | `scripts/build.sh --deps` |
| Build | `pwsh scripts/build.ps1` | `scripts/build.sh` |
| Build and run | `pwsh scripts/build.ps1 -Run` | `scripts/build.sh --run` |
| Run the tests | `pwsh scripts/build.ps1 -Test` | `ctest --test-dir build` |
| Start from scratch | `pwsh scripts/build.ps1 -Clean` | `scripts/build.sh --clean` |
| Build with ASIO | `pwsh scripts/build.ps1 -Asio` | not applicable |

Pass audio files on the command line to start with tracks already on the decks, which is far
faster than clicking through a file dialog while testing:

```
scripts/build.sh --run track-a.wav track-b.wav
```

ASIO needs Steinberg's SDK, which is headers and cannot be shipped with anything. An ASIO
*driver* such as ASIO4ALL is not the SDK. See the README.

## Continuous integration

`.gitea/workflows/build.yml` runs on pushes to `main` and `testing`, on pull requests, and on
demand from the Actions tab. The README carries a status badge per branch; Gitea's badges are per
workflow rather than per job, so one badge covers both platforms.

| Job | Runs on | Status |
| --- | --- | :---: |
| fedora | `fedora:latest` container on the Linux runner | ✅ |
| debian | the Linux runner's own image | ✅ |
| windows | removed, see below | ⬜ |

| Detail | Why |
| --- | --- |
| The Fedora job installs `nodejs` and `git` first | `actions/checkout` is a JavaScript action and the stock image has neither. Without it the job fails in two seconds, long before a compiler is involved |
| There is no Windows job | There was one, it worked, and it was removed: the only Windows runner was a desktop someone works on, and a five minute MSVC build per push made it unusable to type on. Windows is checked by building and testing there directly, which is what has actually caught things, including `NOMINMAX`, the MP3 bug and all three faults above |
| A restored Windows job should call `scripts/build.ps1` | The script finds CMake, Ninja and the MSVC environment inside Build Tools, so the machine needs nothing on PATH but git and node |

The job is kept verbatim as a comment at the foot of the workflow file so it can be pasted back
in one piece.

### Restoring the Windows runner

On a machine nobody is working on, with Build Tools, git and Node installed:

```
pwsh scripts/setup-windows-runner.ps1 -Token <registration token>
pwsh scripts/install-runner-service.ps1
```

The token comes from repository Settings, then Actions, then Runners. The label is
`windows-latest:host`, where `:host` is what makes jobs run on the machine rather than in a
container. The second script installs it as the `GiteaRunner` service via NSSM, which is needed
because the runner is a console program that does not speak to the service control manager;
`-Uninstall` undoes it. Manage it with `Get-Service`, `Restart-Service`, or
`Stop-Service GiteaRunner; Set-Service GiteaRunner -StartupType Manual` to stop it eating the
machine. Logs are in `C:\gitea-runner\logs`, where `runner.err.log` is the interesting file
because progress goes to stderr; a last result of `0xC000013A` means the session ended rather
than a crash.

Two things about it are worth knowing in advance.

**Workspaces must stay in `C:\gitea-runner\work`, as set in `config.yaml`.** This is not a
tidiness preference. The service runs as `LocalSystem`, whose home is under
`C:\Windows\System32`. Visual Studio ships a 32-bit CMake, and a 32-bit process reading a path
under `System32` is silently redirected by WOW64 to `SysWOW64`, where the checkout is not. It
presents as CMake claiming the source directory does not exist, eleven seconds into a job whose
checkout plainly succeeded.

**`LocalSystem` is privileged, and so is every job it runs.** No stored password, but worth
knowing before pointing this repository's CI at code you have not read.

### Reading a failed job's log

The web endpoint serves it without a token, which is quicker than clicking through the UI:

```
curl https://git.interdo.me/interdome/opendj/actions/runs/<run>/jobs/<job>/logs
```

Get both ids from `/api/v1/repos/interdome/opendj/actions/runs/<run>/jobs`. The API's own `/logs`
route needs a token; the web one does not.
