# Decks: the engine for the Numark DJ module

Two decks and a mixer, as a program on the companion box (`deckd`). It plays
through the Numark Mixtrack Pro FX's own sound card: master on outputs 1/2,
headphones on 3/4, 44.1 kHz. Written for this module instead of running Mixxx,
so that its screens can be our pages on every layout and its tempo can later
be tied to ApcBox.

## Status as of 2026-11-28

**Steps 1 to 3 of 4 (the engine, the bridge part, the page: `t95/ui/dj.html`, its row
in `t95/ui/VIEWS.md`) and the first slice of step 4: hot cues, loops, tempo.** Built and
tested on the desk and on the Pi 4. Rene has played it on the Pi rig from the Numark
through the bridge, on headphones: "it sounds and many things actually work". Nothing
was judged in detail: scratch feel, the jog's ticks a turn, the level curves and the
lights are unconfirmed. The pads, the loop buttons and the tempo are newer than that
session: on the rig they were only seen from the bridge's side (two tracks loaded, a
tempo found for each). The T95 is untried.

| Piece | State |
|---|---|
| Decks | A track whole in memory. Play, cue as on a CD deck, search, pitch slider (speed and pitch together, ranges 8 / 16 / 50%), pitch-bend buttons, platter: scratch or nudge |
| Mixer | Trim, 3-band EQ (a band at zero is gone, +6 dB at full), one-knob filter, fader, crossfader (both decks full at the centre), master and headphone level, cue mix, a clipper that never passes full scale |
| Loading | Any file `ffmpeg` reads, up to half an hour, decoded in a thread of its own. A playing deck refuses a load |
| Numark | `numark.h`: transport, strips, master section, platters, browse and LOAD, the pads in CUE mode and the pad mode buttons, the loop buttons (LOOP, shift + LOOP, 1/2, x2, shift + 1/2 = in, shift + x2 = out), lights for play / cue / scratch / headphones, the strip meters, the pad modes, set hot cues and LOOP. FADER CUTS is handed to the controller, which does it itself (as the published mapping says; not confirmed by ear). The pad and loop numbers are from that mapping and the desk tests: no press on the real pads has been checked yet |
| Bridge | `t95/bridge/rackbridge/decks.py`: runs `deckd` while the Numark DJ module is the active one and the card is there, starts it again if it dies, passes the Numark's MIDI in and the lights out, serves what the decks do (`GET /decks`, `decks` events) and takes a page's asks (`POST /decks`). Also the music folder for the page: the cursor onto a file, delete, copy from the box's SD card, upload from a browser |
| Hot cues | 8 a deck: a pad sets an empty one where the record is, goes to a set one (playing: at once, blended over 2 ms, no gap; stopped: plays from it while held), shift + pad clears. Kept per file between loads (`--store`) |
| Loops | Loop in, loop out, on / off, into it again, half and twice as long (down to 256 frames); with no loop set, LOOP makes one of 4 beats from here when the tempo is known. The wrap is blended over 2 ms. Not kept between loads |
| Tempo | `tempo.cpp`: found when a track is loaded (about 0.6 s for 5 minutes on a Pi 4), kept per file. One steady tempo between 80 and 160 (174 comes out as 87; `bpmx` corrects), 0 when no beat stands out. Right on eight made-up kick tracks to 0.02 bpm; **on real music only tried on two tracks, whose real tempo nobody has confirmed** (131.63 and 133.65 found). Which of two half-beat phases is the beat is not settled: it does not matter for loops, it will for SYNC |
| Not there | AUTO LOOP and SAMPLE pads, SYNC, FX paddles, key lock, a library beyond a folder's file list, recording |

## Build and test

```
decks/build.sh              # build/deckd and build/engine_test (OUT=folder: there instead)
decks/build/engine_test     # 156 checks, tracks made in memory: no card, no files
```

On Linux with `libasound2-dev` deckd plays through a card. Anywhere else it
builds with `--null` only (paced by the clock, no sound), which is what the
desk and the bridge's tests (`DecksTest` in `t95/bridge/test_bridge.py`) use. Loading needs `ffmpeg` on the path.

```
deckd [--device hw:CARD=FX,DEV=0 | --null] [--socket /tmp/deckd.sock]
      [--music folder] [--store file] [--period 128] [--buffer 384] [--midi /dev/snd/midiCxD0]
```

`--midi` opens the controller's port itself. That is for trying the engine
alone, with the bridge stopped: in the module the bridge owns the port and
passes the messages on (`M` lines), as the canon asks.

## The socket

A Unix socket, text lines, up to 4 clients. A client that stops reading is
dropped at 256 kB queued; it never holds the engine up. `d` is deck 1 or 2.

| In | |
|---|---|
| `M hex` | MIDI from the controller, whole messages |
| `C 1` | the controller is here: the engine answers with what to send it (`X`) and sends every light again |
| `C 0` | the module is being left: the engine answers with what puts the controller back (`X`) |
| `load d path`, `eject d` | refused while the deck plays |
| `play d 0\|1\|2` | stop, play, toggle |
| `cue d 1\|0` | the cue button pressed, let go |
| `seek d seconds` | |
| `set name [d] value` | 0..1: `gain hi mid lo filter volume pfl scratch` (with d), `xfader master cuemix cuegain`; `set rate d x` with x from -0.5 to 0.5 |
| `browse steps`, `at path`, `pick d` | walk the `--music` list (every file under the folder, sorted by path), put the cursor on a file of it, load the file under the cursor |
| `scan` | the `--music` folder has changed: read it again |
| `hot d n 1\|0`, `hotclear d n` | pad n (1..8) pressed, let go: an empty one is set where the record is, a set one is gone to (stopped: it plays from there while held); clear it |
| `loopin d`, `loopout d` | the loop starts here; ends here and runs |
| `loop d 0\|1\|2 [beats]` | leave the loop, into it again, the other of the two. With beats, or with no loop yet: a new one of that many beats (4) from here; needs the tempo |
| `loopx d 0.5\|2` | the loop half, twice as long |
| `bpmx d 0.5\|2` | the tempo found is twice, half the real one: correct it (remembered) |
| `R` | say everything again |

| Out | |
|---|---|
| `V n` | version of these lines (1) |
| `X hex` | MIDI for the controller |
| `D d playing cue_held pos_ms len_ms speed cue_ms peak track` | speed and peak x1000; on change, at most 30 a second |
| `T d rate range gain hi mid lo filter volume pfl scratch` | rate x10000, range in percent, the rest 0..1000 |
| `G xfader master cue_mix cue_gain peak_l peak_r` | 0..1000 |
| `Q d h1..h8 loop_in loop_out loop_on bpm beat0 pads` | the 8 hot cues, the loop (ms; -1: none), bpm x100 (0: no steady beat found), a beat's place (ms), the pad mode (0 CUE, 1 AUTO LOOP, 2 FADER CUTS, 3 SAMPLE) |
| `N d track name`, `F d track path`, `W d track hex` | a track arrived: its name, its file, and 800 peaks (two hex digits each) |
| `B cursor count path` | the browse cursor: the file it is on (`-`: the list is empty) |
| `H xruns worst_us period buffer rt channels dropped` | once a second: dropouts so far, the slowest block since the last line, whether the audio thread got real-time priority |
| `L text` | log |

These lines are not fixed yet: the bridge part is their only user, and the page (step 3) will ask for more.

## Measured on the Pi 4 (2026-11-27)

Both decks playing a 5 and a 7 minute track at +6% and -4%, EQ and filter in
use, a scripted scratch every 5 s, master at zero, the kiosk and the bridge
running beside it. The Numark's card took 4 channels at 24 bits.

| Period / buffer | Real-time priority | Time | Dropouts | Slowest block |
|---|---|---|---|---|
| 128 / 384 (8.7 ms) | no | 30 s | 0 | 217 us of 2902 |
| 64 / 192 (4.4 ms) | no | 60 s | 0 | 319 us of 1451 |
| 64 / 192 (4.4 ms) | yes | 60 s | 0 | 129 us of 1451 |
| 32 / 96 (2.2 ms) | yes | 60 s | 0 | 156 us of 725 |

About 4.5% of one core. A 5 minute MP3 was decoded in 1.6 s, a 7 minute FLAC
in 2.5 s.

What this does not show: minutes are not hours; nothing else was loading the
box (no sample transfer, no page being drawn hard); the buffer figure is the
engine's side only, not the card's own delay or the time from a pad to the
ear; and the T95 has not been tried at all.

## Choices worth knowing

- **Plain playing is not interpolated.** With the pitch at zero the deck
  steps frame by frame. Off zero, and when scratching, it reads between
  frames with a 4-point curve: about 1 dB down at 15 kHz, and no filter
  against aliasing above normal speed. Good for a first version; a proper
  resampler is the upgrade.
- **Scratch follows the hand in two 5 ms steps**, so jog messages that arrive
  in lumps still give a steady speed. The platter is taken as 1024 ticks a
  turn at 33 1/3 rpm, a figure from Mixxx's mapping, not measured. Both
  need Rene's hands on the platter to judge.
- **Headphones take the master before its knob**, so turning the room down
  does not turn the headphones down.
- **Real-time priority only on the audio thread**, and only if the limits
  allow it (`apcbox-bridge.service` sets `LimitRTPRIO` for it); it runs without.

## Next

4. AUTO LOOP pads, SYNC (needs the beat's phase settled), FX, the sampler, key lock, tempo shared with ApcBox.
