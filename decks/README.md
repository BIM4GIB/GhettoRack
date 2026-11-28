# Decks: the engine for the Numark DJ module

Two decks and a mixer, as a program on the companion box (`deckd`). It plays
through the Numark Mixtrack Pro FX's own sound card: master on outputs 1/2,
headphones on 3/4, 44.1 kHz. Written for this module instead of running Mixxx,
so that its screens can be our pages on every layout and its tempo can later
be tied to ApcBox.

## Status as of 2026-11-28

**Steps 1 to 3 of 4 (the engine, the bridge part, the page: `t95/ui/dj.html`, its row
in `t95/ui/VIEWS.md`) and step 4 but for the sampler: hot cues, loops, tempo, AUTO LOOP, SYNC, effects.** Built and
tested on the desk and on the Pi 4. Rene has played it on the Pi rig from the Numark
through the bridge, on headphones: "it sounds and many things actually work". Nothing
was judged in detail: scratch feel, the jog's ticks a turn, the level curves and the
lights are unconfirmed. He has since used the CUE pads and the loop buttons there ("pads
work") and found a loop fault, since fixed (LOOP later in a track took the record back to
the old loop; a loop halved all the way and doubled again came back 5 ms off). AUTO LOOP,
SYNC and the effects are newer than that: on the rig they were only seen from the
bridge's side. The T95 is untried.

| Piece | State |
|---|---|
| Decks | A track whole in memory. Play, cue as on a CD deck, search, pitch slider (speed and pitch together, ranges 8 / 16 / 50%), pitch-bend buttons, platter: scratch or nudge |
| Mixer | Trim, 3-band EQ (a band at zero is gone, +6 dB at full), one-knob filter, fader, crossfader (both decks full at the centre), master and headphone level, cue mix, a clipper that never passes full scale |
| Loading | Any file `ffmpeg` reads, up to half an hour, decoded in a thread of its own. A playing deck refuses a load |
| Numark | `numark.h`: transport, strips, master section, platters, browse and LOAD, the pads in CUE and AUTO LOOP mode and the pad mode buttons, the loop buttons (LOOP, shift + LOOP, 1/2, x2, shift + 1/2 = in, shift + x2 = out), SYNC, the FX section (six buttons, two paddles, wet / dry, beats), lights for play / cue / sync / scratch / headphones, the strip meters, the pad modes, set hot cues, the running auto loop, LOOP and the chosen effect. FADER CUTS is handed to the controller, which does it itself (as the published mapping says; not confirmed by ear). Rene has pressed the CUE pads and the loop buttons on the rig (they work); AUTO LOOP, SYNC and the FX section are from the mapping and the desk tests only, and which of the six FX buttons is which is taken from the panel's order. The controller's own report puts wet / dry on channel 10 where the mapping says 9: both are taken |
| Bridge | `t95/bridge/rackbridge/decks.py`: runs `deckd` while the Numark DJ module is the active one and the card is there, starts it again if it dies, passes the Numark's MIDI in and the lights out, serves what the decks do (`GET /decks`, `decks` events) and takes a page's asks (`POST /decks`). Also the music folder for the page: the cursor onto a file, delete, upload from a browser (into folders too), and the box's SD card or a USB stick in it as sources: listed, copied from, or a track loaded straight off them |
| Hot cues | 8 a deck: a pad sets an empty one where the record is, goes to a set one (playing: at once, blended over 2 ms, no gap; stopped: plays from it while held), shift + pad clears. Kept per file between loads (`--store`) |
| Loops | Loop in, loop out; LOOP ends the running loop or makes a new one where the record is, as many beats as the last (4 at first); shift + LOOP goes back into the last one; half and twice as long, exactly (down to 1/32 beat or 512 frames). AUTO LOOP pads: 1/16 to 8 beats, the lit pad ends it; shift + pad: a loop while held, and let go the record is where it would have been. The wrap is blended over 2 ms. Not kept between loads |
| Tempo | `tempo.cpp`: found when a track is loaded (about 0.6 s for 5 minutes on a Pi 4), kept per file. One steady tempo between 80 and 160 (174 comes out as 87; `bpmx` corrects), 0 when no beat stands out. Right on eight made-up kick tracks to 0.02 bpm; **on real music only tried on two tracks, whose real tempo nobody has confirmed** (131.63 and 133.65 found). Which of two half-beat phases is the beat is not settled: it does not matter for loops, it will for SYNC |
| SYNC | The deck takes the other deck's tempo as it plays, halved or doubled when that is nearer, and follows its pitch slider; its own slider waits. **Tempo only:** the beats are lined up by hand, because which of two half-beat places is the beat is not settled. Refused without a tempo on both decks |
| Effects | `fx.cpp`: HPF, LPF, flanger, echo, reverb, phaser. One at a time for both decks, each deck switched in by its paddle, between its filter and its fader; wet and beats (a sweep's or an echo's length by the deck's tempo, 1/16 to 8). Filters, flanger and phaser fade out in 10 ms, echo and reverb ring out. On a Pi 4 the slowest block with any of them on both decks was 302 of 1451 µs. **Nobody has heard them yet:** how each one sounds is untuned |
| Not there | SAMPLE pads, TAP, beats lined up by SYNC, key lock, a library beyond a folder's file list, recording |

## Build and test

```
decks/build.sh              # build/deckd and build/engine_test (OUT=folder: there instead)
decks/build/engine_test     # 199 checks, tracks made in memory: no card, no files
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
| `set name [d] value` | 0..1: `gain hi mid lo filter volume pfl scratch sync fx` (with d), `xfader master cuemix cuegain`; `set rate d x` with x from -0.5 to 0.5 |
| `browse steps`, `at path`, `pick d` | walk the `--music` list (every file under the folder, sorted by path), put the cursor on a file of it, load the file under the cursor |
| `scan` | the `--music` folder has changed: read it again |
| `hot d n 1\|0`, `hotclear d n` | pad n (1..8) pressed, let go: an empty one is set where the record is, a set one is gone to (stopped: it plays from there while held); clear it |
| `loopin d`, `loopout d` | the loop starts here; ends here and runs |
| `loop d 0\|1\|2 [beats]` | 0: leave the loop. 1: into the last one again. 2: the LOOP button: leave it, or a new one from here as long as the last. With beats: a loop that long, from here or from the running one's start. By the beat needs the tempo |
| `loopx d 0.5\|2` | the loop half, twice as long; with none running, what LOOP makes next |
| `roll d beats 1\|0` | a loop of that many beats while held (1), let go (0): the record is where it would have been |
| `fxkind n`, `fxwet x`, `fxbeats x` | the effect (0 HPF, 1 LPF, 2 flanger, 3 echo, 4 reverb, 5 phaser), how much (0..1), how long (1/16..8 beats) |
| `bpmx d 0.5\|2` | the tempo found is twice, half the real one: correct it (remembered) |
| `R` | say everything again |

| Out | |
|---|---|
| `V n` | version of these lines (1) |
| `X hex` | MIDI for the controller |
| `D d playing cue_held pos_ms len_ms speed cue_ms peak track` | speed and peak x1000; on change, at most 30 a second |
| `T d rate range gain hi mid lo filter volume pfl scratch` | rate x10000, range in percent, the rest 0..1000 |
| `G xfader master cue_mix cue_gain peak_l peak_r` | 0..1000 |
| `Q d h1..h8 loop_in loop_out loop_on bpm beat0 pads beats roll sync synced rate fx` | the 8 hot cues, the loop (ms; -1: none), bpm x100 (0: no steady beat found), a beat's place (ms), the pad mode (0 CUE, 1 AUTO LOOP, 2 FADER CUTS, 3 SAMPLE), the loop's beats x10000, a roll held, SYNC asked for, SYNC working, the speed offset in use x10000, through the effect |
| `E kind wet beats` | the effect for both decks: which (0..5), wet 0..1000, beats x10000 |
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

4. The beat's phase, so SYNC can line beats up; the sampler; TAP; key lock; tempo shared with ApcBox.
