# GhettoRack screens

What each screen shows. How a screen is made, and the rules it must keep:
[`docs/CANON.md`](../../docs/CANON.md).

Every screen the T95 shows, on the TV (1920x1080, remote) and on a phone.
Rules: one purpose per view, everything relevant visible, no scrolling,
instant switching (number keys on the remote, tabs, bottom bar on the phone).
Controls look like hardware: pots with LED rings, faders, LED meters, buttons
that are dark until active and glow in their colour. Waveforms wherever a
sample or loop is involved.

Status: **built** = works in `apcbox.html` (the page at `/apcbox`, made from
`apcbox.css` + `apcbox.js` by `build.py`; desktop-tested against the firmware
control code, not yet tried on the rig), **mock** = drawn in `mock/` only,
**later** = planned, needs new firmware features.

Open the mocks: `t95/ui/mock/views.html?v=<name>` in a browser at 1920x1080
(static, made-up values). Names in the table.

## Always on screen

| Frame | Content |
|---|---|
| Top bar | stop, play, record, BPM, tap, position, pattern (and queued), set, LISTEN (the Daisy's sound in this browser), status LEDs |
| View bar | 8 tabs; number keys 1-8 on the remote |
| Track header | on per-track views: name, type (sample / 303 / live loop), mute, solo, rec arm |

## ApcBox

| Tab | View | Mock | Content | Status |
|---|---|---|---|---|
| 1 Perform | Perform | `perform` | 16 strips: pan and send pots, fader, meters, step LEDs, on/mute/solo/arm; patterns, FX, master pots | built |
| 2 Steps | Drum grid | `grid` | 16 tracks x 16 steps; tap = on, again = accent, again = off; mute/solo per row | built |
| 2 Steps | One track | `track` | big steps, waveform, the track's main pots | built |
| 2 Steps | 303 | `acid` | note grid (piano roll), accent, slide, big steps with note names | built |
| 3 Mixer | Mixer | `mixer` | per track: pan, reverb, delay, send C, fader, meters, mute/solo, crossfader side; master strip | built |
| 4 Sound | Sample track | `sound` | waveform with start and playhead; 8 device pots, 8 track pots | built |
| 4 Sound | 303 | `sound303` | filter curve, envelope; cutoff (in Hz: where the filter rests, 150 Hz to 4.8 kHz), reso, env mod, decay, accent, tune, slide, wave | built |
| 4 Sound | Live loop | `loop` | loop waveform with bar lines, re-record, overdub, stop, undo take | built (v13.12) |
| 5 Samples | Library / SD | `samples` | pages of files with a waveform each, hear it on this device, send to the selected track, upload, import from the SD card, delete. Tap a file: trim view (two markers on a big waveform, normalise, hear the result, send) | built |
| 5 Samples | Looper | - | the Daisy's own input into the selected track: input meter, level pot, monitor, count-in, take length (free/1/2/4/8 bars), start on bar or beat; REC LOOP, OVERDUB, SAMPLE (one-shot), STOP, UNDO / REDO TAKE, CLEAR; the track's waveform | built (v13.13) |
| 5 Samples | Mic | `sampler` | record with this device's microphone (HTTPS on phones), live waveform, into the library, on to the selected track | built |
| 5 Samples | Kits | - | the 8 tracks of a page as slots with waveforms (tap: select and hear); SAVE AS KIT / SAVE OVER (asks), RENAME, DELETE (asks), UNDO KIT; every kit on the Daisy, load into tracks 1-8 or 9-16 | built (v13.14) |
| 6 Patterns | Patterns | `patterns` | 8 banks x 8 with a thumbnail each, playing and queued, copy/clear/undo | built |
| 6 Patterns | Sets | (in `patterns`) | the 8 slots on the Daisy's card: load (switches on the bar), save here, save as new, rename, delete, transition; DEFAULT SET (v13.34): the set in the firmware, any time | built (v13.10) |
| 7 FX | FX | `fx` | 8 performance FX as hold pads; reverb, delay, master filter, drive pots; master out waveform | built |
| 8 System | APC40 map | - | a drawing of the APC40: every button, knob and fader with what it does alone (white), with SHIFT (amber), on SEND C or a 303 track (blue); tap one for the full description. From v13.15 it is the editor: tap a control, CHANGE (alone or SHIFT), tap the control whose job it should do (asks which if it has two), or pick a job no button has, or nothing; "whole row" moves 8 at once; knobs and faders swap among themselves; ↺ one control back, RESET ALL (asks). Changed controls are dashed | built (v13.15; the job texts are written by hand from `daisy/box.cpp`) |
| 8 System | Controller map | - | a drawing of the game controller on the box's USB: every button, stick and trigger with its job; a press on the controller lights it; tap one, CHANGE, pick a job; ↺ and RESET ALL. The box keeps it (`/pad`) | built (2026-11-27) |
| 8 System | System | `system` | links, ping, log, quantise (pattern switch: bar/beat/step; mute and solo: at once/beat/bar; takes: bar/beat; held FX: at once/step/beat), other modules | built |
| - | Song / chain | - | patterns in a row as an arrangement | later (no song mode in the firmware) |
| - | Slice | - | chop a sample or loop across steps | later (new firmware feature) |
| - | Keys | - | on-screen keyboard and pads for the selected track, scale lock | later |

Sound and Steps show the editor that fits the selected track's type.

## Other modes

| View | Content | Status |
|---|---|---|
| Selector | pick a mode | built (`selector.html`) |
| MIDI hub | routing grid, network devices, presets | built (`midihub.html`), same look |
| Clock | tempo, source, outputs, timing | built (`clock.html`), same look |
| Jam recorder | `/jam` (`t95/ui/jam.html`), from the start screen (its lamp is red while a take runs): REC / STOP, time, level L/R, room left on the T95; takes with play (one WAV, seekable), download, rename, to the sample library, TO CARD (one WAV in `jams/` on the box's SD card, with progress), delete (asks); EJECT CARD; the card's free room in the top bar. Records the Daisy's master onto the T95. Remote: OK does the focused button (REC first), play / stop, arrows walk the buttons | built, same look; one short take tried on the rig, the remote not |
| Settings | `/settings` (`t95/ui/settings.html`): Network, Hotspot, Bluetooth (on / off, scan, pair, forget; MIDI devices are marked; VISIBLE: 3 minutes as a Bluetooth MIDI device an iPad or Mac can find, with the PIN, untried with a device), System (start-up delay, PIN, power), Devices (Daisy, controllers, Bluetooth MIDI, the box's SD card, every sound card with what the box uses it for, TV sound; the Daisy's own card is not shown: the firmware doesn't report it), Remote (the remote's buttons per profile). With the TV remote, ok on a PIN field (or on a button that needs the PIN) opens a number pad. The start screen holds only the modules and the way here | built; hotspot not working on the rig yet |
| Cubase sync | plugin status, Cubase's transport | later |
| Numark DJ | `/dj` (`t95/ui/dj.html`), on the deck engine (`decks/README.md`). **DECKS**: per deck the track's name, time left, its whole outline with the playhead (yellow) and the cue point, pitch and range, level; CUE, PLAY, LOAD. **PADS**: a deck's 8 hot cues (a tap sets an empty one or goes to a set one; CLEAR, then the pad, clears it), its loop (IN, OUT, LOOP, 1/2, x2; LOOP ends the running loop or makes a new one from here, its length in beats on the button) and its tempo with BPM /2 and BPM x2 for a tempo found half or double. On the wide layout the pads sit in each deck, on a phone they are a tab. DECKS shows the tempo as it plays (with the pitch slider), the hot cues as marks on the outline and the loop as a bar under it. **LIBRARY** (`LIB`): the music folder as folders and tracks, paged; the yellow row is the engine's cursor, the same one the Numark's browse knob moves; TO DECK 1 / 2 (not onto a playing deck), DELETE (asks; not what is on a deck), FROM CARD (the box's SD card: OPEN a folder, COPY a track or a whole folder), and on the big layouts UPLOAD from the device. DECKS has SYNC a deck (the other deck's tempo; tempo only, the beats are lined up by hand). **MIX**: where the Numark's knobs and faders stand, the levels, the crossfader; read only, but for PHONES, SCRATCH and FX (the deck through the effect; the button names it). The big layouts also choose the effect, its beats and how wet; on the small screen those are the Numark's, shown in the message line. Wide (TV, Mac, iPad on its side): all three at once. Phone, upright or on its side: one at a time on a tab row. 480x320: rail DECKS / PADS / LIB / MIX. With another module active: one line and START THE DECKS. Remote: on the small screen arrows walk the buttons, 1 and 2 start and stop a deck; on a TV up / down move the cursor, left / right load it to deck 1 / 2, ok loads it to a deck that is not playing. **Not there:** a scrolling waveform (the outline is 800 points a track), beats lined up by SYNC, the AUTO LOOP sizes as pads on a screen (1/2 and x2 choose the length), SAMPLE pads, a held pad on a screen (the Numark's pads do that), choosing the effect on the small screen, key lock, track lengths and tags in the list, search, seeking by touch on the small screen (a click on the outline seeks on the big layouts) | built; 30 frames a second on the Pi's panel with a deck playing (DECKS 318 kB/s over SPI, MIX 858); browser tests on the desk for all four layouts; Rene has not seen it on the panel yet, the remote is untried with a real one |

## Shared parts (build once)

Pot with LED ring (fill from the left, or from the centre for pan-like
values), fader, LED meter, LED button, step cell, big step, waveform (static,
live, with markers and bar lines), pad, list row with waveform.

## What the firmware has today (checked in `daisy/engine.h`, `box.cpp`)

- A step is off / on / accent. No per-step velocity or probability
  (probability is a per-track pot).
- 303 steps: note and slide. 303 pots: cutoff, reso, env mod, decay, accent,
  tune, slide, wave (saw/square).
- Sample track pots: pitch, decay, filter, reso, start, reverse, drive, prob.
- Live loop: arm, records whole bars, stop ends on the bar, cancel. From
  v13.12: fixed take lengths, overdub (in place, at the loop's tempo), undo
  of the last take, count-in, start on the beat, input level, and one-shot
  sampling from the input (starts with the sound).
- The APC40's REC ARM button is "roll" (hold to repeat a track), not record
  arm. On these screens ARM means arming a live loop recording for that track.
- v13.9 (control protocol 4, report 10) adds what the screens need, with no
  APC40 required: page actions through `POST /do` on the bridge (mute, solo,
  pots, tempo, steps, notes, pattern queue/copy/clear/undo, FX hold, record,
  metronome, track type, live loop arm/stop/cancel, kit load, audition),
  level meters (`H`), the 64-pattern mask (`O`) and a waveform outline per
  track (`Z`). See `protocol/serial.md`. Desktop-tested; not flashed yet.
- v13.10 (control protocol 5, report 11): sets and the kit list for the pages.
- v13.12 (control protocol 7, report 13): looper and quantise settings (`LP`),
  roll from a page (ROLL on the Perform strips, hold), keyboard knobs (CC 1/74
  filter, 71 reso, 72 decay, 73 start, 7 volume, 10 pan, 91 reverb, 93 delay),
  pitch bend (2 semitones) and pads (channel 10, notes 36-51 = tracks 1-16)
  on the selected track. Desktop-tested; not flashed yet.
- v13.13 (control protocol 8, report 14): held FX snap to the step (or the
  beat, or not), redo of an undone take. On the APC40: OVERDUB overdubs the
  selected track's loop (it used to do the same as the record button). The
  two device arrows (next to it) are undo and redo: of the last take, and of
  a sample changed from the APC40 (SHIFT + those arrows = previous / next
  file, BANK left/right = folder, the encoder), so a sample changed by
  accident comes back, also when it was a recorded loop. Stepping on through
  files still undoes to the original.
- v13.14 (control protocol 9, report 15): kits made on the box (`daisy/kits.h`):
  sounds plus settings, 303 tracks as 303 state, rename, delete; a kit loaded
  by accident is undone with the same undo (APC40: the left device arrow).
- v13.15 (control protocol 10, report 16): the button map (`daisy/apcmap.h`,
  `MB` / `MC`, `map b` / `map c` / `map reset`), saved on the card.
- v13.16 (control protocol 10): 16 s slots, 8 spare blocks for loops that don't
  fit one. No page change: the looper says "no room" in the popup line.
- v13.17 (report 17): `VF`, the firmware's version; Settings > Devices shows it.
- v13.20 (report 18): `SM`, loop memory. The looper view says "no room: 8 bars
  at 100 BPM (19 s, longest free 10 s)" before a take is armed and dims REC
  LOOP, and "room for 13" during a take that runs until stopped; System >
  Links has a "loop memory" line, amber when no piece of 32 s is free.
- v13.21: a sample longer than 16 s goes into the same memory whole (from the
  card, a kit, a set or the T95's library, which sends up to 98 s). No page
  change: "(cut to fit)" after a send now only shows when there was no room.
- v13.24 (report 19): `OV`, overload. While the Daisy leaves 303 voices out
  the top bar of every view has a red "OVERLOAD: 4 of 8 303s out", the mixer
  strips of those tracks are dimmed with a red outline, and System > Links
  names the tracks. The Daisy's own screen shows the same line in a red bar.
- v13.32 (control protocol 11): `file d` and `folder d`, the Daisy's own card
  from a page. The small LIB view has a DAISY CARD tab for it.
- v13.33 (control protocol 12): `set over n [name]`; `set save` no longer replaces a set.
  SAVE HERE sends `set over` after its second tap.
- v13.35 (control protocol 14, report 20): `files start count` answered in `BL` lines and
  `file at n`: DAISY CARD is a list of the folder's files by name. A sample sent from the box
  can be undone (`lrec undo`), on the small page with UNDO beside TO TRACK.
- v13.34 (control protocol 13): `set demo`, the default set from the firmware, switched in on
  the bar. DEFAULT SET in both layouts' SETS (asks when there are unsaved changes).

## Layouts

Three, picked from the window and scaled to fit it (`fit()` in `apcbox.js`),
and the small one above, asked for with `?small`:
wide (TV, Mac, iPad on its side; drawn 1920 wide), upright (phone, iPad
upright; drawn 390 wide), and a phone on its side (drawn 844 wide: the view
buttons move to a rail on the left, the top bar is one row, strips lose
their pots in Perform and the Mixer keeps three small ones).

## Small: 480x320 (`?small`)

The 3.5" touch screen on a Pi's header (`pi4/README.md`); the kiosk opens
`/apcbox?small=1`. Its own seven views on a rail on the left, a 24 px top bar
(play, record, BPM, pattern, one status, the track page, ZOOM), drawn 1:1.
The APC40 does the playing; this screen shows names and values and picks
things by name.

| Rail | Dense (stylus, fingernail) | ZOOM (finger) |
|---|---|---|
| STEP | 16 tracks x 16 steps, every step tappable, mute and solo per row, the selected track's values in a line | the 8 tracks of the selected track's page |
| TRK | the selected track: 16 big steps (303: with notes), waveform, 4 pots at a time (SOUND 1, SOUND 2, MIX), HEAR, mute, solo | the same |
| MIX | 16 strips: pan and two sends as bars, fader with a meter, mute, solo, crossfader side; master | 8 strips with pan and reverb pots; master |
| PAT | all 64 patterns, the 8 FX as hold pads, copy / clear / undo | one bank of 8 with a bank row, big FX pads |
| LOOP | LOOPER: the Daisy's own input into the selected track. Take length (free, 1, 2, 4, 8 bars), input level, COUNT-IN, MONITOR, an input meter; REC LOOP, OVERDUB, SAMPLE (one-shot), STOP, UNDO / REDO TAKE (CANCEL while armed), CLEAR. QUANTISE: what snaps to the beat, as on the big System view (pattern switch, mute and solo, takes, held FX) | the same |
| LIB | LIBRARY and BOX CARD (files on the box and on the card in its slot: a waveform each, TO TRACK, UNDO for what the track held before), DAISY CARD (the samples on the Daisy's own card: the folder stepped with ‹ ›, its files by name five to a page, a tap loads one into the selected track, UNDO; says so when the Daisy has no card or older firmware than v13.35), KITS (load on 1-8 / 9-16, SAVE the selected track's page as a kit under a typed name, RENAME and DELETE a kit on the card, undo), SETS (LOAD, SAVE HERE, SAVE AS NEW under a typed name, RENAME, DELETE, DEFAULT SET). A name is typed on the screen's own keyboard: a-z, 0-9, space, `-`, up to 16 | the same |
| SYS | Daisy link, firmware, ping, CPU and overload, loop memory, controllers, log; MODULES, SETTINGS, PAD MAP (the game controller's controls and jobs as a list, the pressed one lit, RESET; to read, a job is changed on a bigger screen), RELOAD, RESTART DAISY (the Daisy's RESET button from here, firmware v13.35; asks, and says so when the set is unsaved) | the same |

The 1-8 / 9-16 button selects the same track on the other page. ZOOM is kept
in the browser. No shadows or glows, and plain elements instead of a canvas:
the panel is on a slow SPI link and only what changed is sent. Class names
start with `s-` so the other layouts' styles cannot reach them.

The frame and the parts are shared from 2026-11-25: `small.css` and `small.js`
(the bridge serves them at `/small.css` and `/small.js`). A page loads
`small.js` and, opened with `?small`, builds itself with `Small.frame()`: the
24 px top bar, a rail on the left with HOME last, content that never scrolls
(`Small.pager` for anything longer than the screen), a touch that acts when the
finger lands, no pointer unless a mouse moves. Class names start with `g-`.
Small sticks to the browser: opened once with `?small=1`, every page of the box
stays small there, whatever link led to it (the TV pages' HOME is a plain `/`);
`?small=0` ends it. The kiosk has a browser of its own.
Typing, with no keyboard on the box: `Small.keyboard()` (four rows of ten keys,
SHIFT, a symbols layer, SPACE, DEL; a password shows as dots until SHOW) and
`Small.numpad()` for a PIN. Both cover the frame and give the text back, or
nothing on CANCEL.
The ApcBox page has its own copy of these rules (it is one self-contained
file) and is the model for the rest.

| Page | `?small` | Content |
|---|---|---|
| Start (`/`) | built | the modules, the jam recorder and Settings as tiles, three across; "last used" in green; at boot the countdown as a bar with STAY HERE |
| Jam recorder (`/jam`) | built | REC: the button, time, levels, room left, the box's SD card and EJECT. TAKES: five to a page, TO LIBRARY, TO CARD, RENAME (the screen's keyboard), DELETE (asks). No playing (the box has no speaker) |
| Clock (`/clock`) | built | CLOCK: tempo, position, beat lamps, START / CONTINUE, -1 -0.1 +0.1 +1 TAP, clock on, send when stopped, share on Link. FROM: where the tempo comes from. OUT: who gets the clock. STATS: timing |
| MIDI hub (`/midihub`) | built | ROUTE: the grid, 5 senders by 5 receivers a page (‹ › for more of either); tap a square to connect and select it, EDIT for channel, to-channel and message types (each tap applied at once), REMOVE. A sender's name turns green while it sends. NET: what is on the network, NEVER CONNECT / ALLOW. SAVED: presets, LOAD, SAVE AS NEW (the name typed on the screen's keyboard, "preset N" offered), RENAME, DELETE (asks) |
| Settings (`/settings`) | built | NET: the Wi-Fi it is on, SCAN, networks four to a page, JOIN / RETRY (the password on the screen's keyboard, the join on trial as on the TV page), FORGET (asks), BOX NAME. HOT: the hotspot, its password, BY ITSELF. BT: on / off, SCAN, VISIBLE, PAIR / FORGET. IR: the remote's buttons five to a page, PROFILE, LEARN TAP / HOLD, CLEAR. DEV: Daisy, controllers, sound cards, the box's SD card, SOUND OUT (steps through the box's outputs: HDMI, the headphone socket; then off). SYS: start-up delay, RESTART BRIDGE / REBOOT / SHUT DOWN (ask), RESTART DAISY (asks; no PIN, it is the Daisy's own action), SET / CHANGE PIN. The PIN shows as a button in the top bar (LOCKED / UNLOCKED) and is asked for on a number pad when something needs it |

Not there: editing 303 notes, recording from a microphone (the box has none), the
APC40 map, changing the game controller's jobs, hearing a file before it is sent (the box
has no speaker: HEAR plays what is on the track, through the Daisy). The
Every page of the box has a small layout now.

A remote or a game controller works on all of them (2026-11-27, `Small.remote` in `small.js`):
the arrows move a white outline from button to button, OK taps, BACK cancels the keyboard or
goes back, HOME goes to the start screen. PLAY and STOP start and stop the clock on the clock
page and a take on the jam recorder. The ApcBox page keeps its own (arrows: track and volume).
Browser-tested with arrow keys; not tried with the pad on the panel.

## Phone

Every view has a portrait layout that fits 390x844 with no scrolling:
`mock/views.html?phone=1&v=<name>` (`mock/phone.js`). Compact top bar, eight
short tabs at the bottom. Differences from the TV: Perform and Mixer show 8
strips at a time (1-8 / 9-16, master as a third page in the mixer); the drum
grid keeps all 16 x 16; pots are 4 across; big steps are 2 rows of 8; sets
are a second page under Patterns; OLED and LED mirrors are behind a button
in System.
