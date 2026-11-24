# GhettoRack screens

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
| 4 Sound | 303 | `sound303` | filter curve, envelope; cutoff, reso, env mod, decay, accent, tune, slide, wave | built |
| 4 Sound | Live loop | `loop` | loop waveform with bar lines, re-record, overdub, stop, undo take | built (v13.12) |
| 5 Samples | Library / SD | `samples` | pages of files with a waveform each, hear it on this device, send to the selected track, upload, import from the SD card, delete. Tap a file: trim view (two markers on a big waveform, normalise, hear the result, send) | built |
| 5 Samples | Looper | - | the Daisy's own input into the selected track: input meter, level pot, monitor, count-in, take length (free/1/2/4/8 bars), start on bar or beat; REC LOOP, OVERDUB, SAMPLE (one-shot), STOP, UNDO / REDO TAKE, CLEAR; the track's waveform | built (v13.13) |
| 5 Samples | Mic | `sampler` | record with this device's microphone (HTTPS on phones), live waveform, into the library, on to the selected track | built |
| 5 Samples | Kits | - | the 8 tracks of a page as slots with waveforms (tap: select and hear); SAVE AS KIT / SAVE OVER (asks), RENAME, DELETE (asks), UNDO KIT; every kit on the Daisy, load into tracks 1-8 or 9-16 | built (v13.14) |
| 6 Patterns | Patterns | `patterns` | 8 banks x 8 with a thumbnail each, playing and queued, copy/clear/undo | built |
| 6 Patterns | Sets | (in `patterns`) | the 8 slots on the Daisy's card: load (switches on the bar), save here, save as new, rename, delete, transition | built (v13.10) |
| 7 FX | FX | `fx` | 8 performance FX as hold pads; reverb, delay, master filter, drive pots; master out waveform | built |
| 8 System | APC40 map | - | a drawing of the APC40: every button, knob and fader with what it does alone (white), with SHIFT (amber), on SEND C or a 303 track (blue); tap one for the full description. From v13.15 it is the editor: tap a control, CHANGE (alone or SHIFT), tap the control whose job it should do (asks which if it has two), or pick a job no button has, or nothing; "whole row" moves 8 at once; knobs and faders swap among themselves; ↺ one control back, RESET ALL (asks). Changed controls are dashed | built (v13.15; the job texts are written by hand from `daisy/box.cpp`) |
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
| Settings | `/settings` (`t95/ui/settings.html`): Network, Hotspot, Bluetooth, System (start-up delay, PIN, power), Devices (TV sound), Remote (the remote's buttons per profile). The start screen holds only the modules and the way here | built; hotspot not working on the rig yet |
| Cubase sync | plugin status, Cubase's transport | later |
| Numark DJ | decks with scrolling waveforms, mixer, library | later |

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
- Still missing in the firmware: sample browser on the Daisy's card from the
  T95.

## Layouts

Three, picked from the window and scaled to fit it (`fit()` in `apcbox.js`):
wide (TV, Mac, iPad on its side; drawn 1920 wide), upright (phone, iPad
upright; drawn 390 wide), and a phone on its side (drawn 844 wide: the view
buttons move to a rail on the left, the top bar is one row, strips lose
their pots in Perform and the Mixer keeps three small ones).

## Phone

Every view has a portrait layout that fits 390x844 with no scrolling:
`mock/views.html?phone=1&v=<name>` (`mock/phone.js`). Compact top bar, eight
short tabs at the bottom. Differences from the TV: Perform and Mixer show 8
strips at a time (1-8 / 9-16, master as a third page in the mixer); the drum
grid keeps all 16 x 16; pots are 4 across; big steps are 2 rows of 8; sets
are a second page under Patterns; OLED and LED mirrors are behind a button
in System.
