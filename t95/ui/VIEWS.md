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
| 4 Sound | Live loop | `loop` | loop waveform with bar lines, arm, stop at bar, cancel, back to a sample | built (no input level yet) |
| 5 Samples | Library / SD | `samples` | browser with a waveform per file, audition, send to track, upload, import | mock (the tab links to the classic page for now) |
| 5 Samples | Record | `sampler` | record from the Daisy input or the phone mic, live waveform, trim, normalise, save, send | mock (trim, normalise: later) |
| 5 Samples | Kits | - | kit list, load, what's in each | to draw |
| 6 Patterns | Patterns | `patterns` | 8 banks x 8 with a thumbnail each, playing and queued, copy/clear/undo | built |
| 6 Patterns | Sets | (in `patterns`) | sets on the card, save, load, switch on the bar | mock (needs set commands in the firmware) |
| 7 FX | FX | `fx` | 8 performance FX as hold pads; reverb, delay, master filter, drive pots; master out waveform | built |
| 8 System | System | `system` | links, ping, OLED mirror, APC40 LED mirror, log, settings | built |
| - | Song / chain | - | patterns in a row as an arrangement | later (no song mode in the firmware) |
| - | Slice | - | chop a sample or loop across steps | later (new firmware feature) |
| - | Keys | - | on-screen keyboard and pads for the selected track, scale lock | later |

Sound and Steps show the editor that fits the selected track's type.

## Other modes

| View | Content | Status |
|---|---|---|
| Selector | pick a mode | built (`selector.html`) |
| MIDI hub | routing grid, network devices, presets | built (`midihub.html`); restyle to match |
| Clock | tempo, source, outputs, timing | built (`clock.html`); restyle to match |
| Settings | remote buttons, start-up, TV sound, network name | in the selector today; own view planned |
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
- Live loop: arm, records whole bars, stop ends on the bar, cancel. No overdub.
- The APC40's REC ARM button is "roll" (hold to repeat a track), not record
  arm. On these screens ARM means arming a live loop recording for that track.
- v13.9 (control protocol 4, report 10) adds what the screens need, with no
  APC40 required: page actions through `POST /do` on the bridge (mute, solo,
  pots, tempo, steps, notes, pattern queue/copy/clear/undo, FX hold, record,
  metronome, track type, live loop arm/stop/cancel, kit load, audition),
  level meters (`H`), the 64-pattern mask (`O`) and a waveform outline per
  track (`Z`). See `protocol/serial.md`. Desktop-tested; not flashed yet.
- Still missing in the firmware: set save/load from the T95, sample browser
  on the Daisy's card from the T95, roll from a page.

## Phone

Every view has a portrait layout that fits 390x844 with no scrolling:
`mock/views.html?phone=1&v=<name>` (`mock/phone.js`). Compact top bar, eight
short tabs at the bottom. Differences from the TV: Perform and Mixer show 8
strips at a time (1-8 / 9-16, master as a third page in the mixer); the drum
grid keeps all 16 x 16; pots are 4 across; big steps are 2 rows of 8; sets
are a second page under Patterns; OLED and LED mirrors are behind a button
in System.
