# GhettoRack backlog

Everything agreed, mentioned or left open that isn't built yet, so it doesn't
get lost. Newest state: 2026-11-23. Move a line out when it's done (the
status tables are `t95/README.md` and `t95/ui/VIEWS.md`).

## v13.17 on the rig (flashed 2026-11-24): v13.16 plus the firmware's version in the report

Report version 17: a `VF` line with the firmware's version. `/health` on the
box says `"firmware": "13.17"`, Settings > Devices shows it. Read back on the
rig after the flash, pages at report 17, no mismatch warning.

## v13.16: 16 s slots and spare blocks (in v13.17 on the rig, long takes NOT tried there yet)

Control protocol 10 as before. SDRAM
95.9% in the build (`make -C daisy`, 2026-11-24). On the rig it boots and the
bridge talks to it; nothing of the list below has been played there.

- A track's slot holds 16 s (was 32 s); the looper's undo memory shrinks with it.
- 8 spare blocks of 32 s. The looper takes one only when the chosen take length
  doesn't fit a slot at the tempo (8 bars below about 117 BPM). None free: it
  says "no room" and arms nothing. A take with no chosen length gets a block if
  one is free and moves into the slot afterwards if it fits; else it records
  into the slot and ends at 16 s.
- A block is free again when its track gets any other sound.
- No undo for a take that stays in a block, nor for an overdub on one. A
  dropped take puts the track back on what it held, long loop included.
- Sets: a long loop is saved whole and loads back into a block; with none free
  it is cut to 16 s. Any other sound over 16 s is cut on load (sets saved
  before this included). One-shot sampling ends at 16 s.
- Fixed on the way: a set with a sound longer than a slot wrote past the slot
  when it was copied home after the switch.
- To try on the rig: an 8 bar take at 100 BPM (record, play past 16 s, cancel
  one, save and reload a set with it), and the "no room" message with all 8
  blocks in use. Possible later: undo for long takes (the old sound is still
  in the slot, only redo needs memory).

## In the firmware to flash (v13.15, built, desktop-tested, NOT flashed)

`daisy/release/ApcBox.bin`. Control protocol 10, report 16.

- Clock: the Daisy follows the T95's clock, the T95 can follow the Daisy.
- Looper (Samples > looper, or the loop page under Sound): take length
  free/1/2/4/8 bars, overdub (in place, only at the loop's tempo), undo the
  last take, count-in, start on the bar or the beat, input level, monitor.
- One-shot sampling from the Daisy's input (SAMPLE: starts with the sound).
- Quantise (System): pattern switch on bar/beat/step, mute and solo at
  once/beat/bar, held FX at once/step/beat (step by default: on and off
  both land on the grid).
- Redo of an undone take. APC40: OVERDUB = overdub the selected track's loop,
  the two device arrows = undo / redo, SHIFT + them = previous / next sample
  in the folder (BANK left/right = folder, as before).
- Undo also takes back a sample changed from the APC40 by accident (one
  press goes back to what the track held before the browsing started), and
  a kit loaded by accident (all 8 tracks).
- Kits made on the box (Samples > kits): the 8 tracks of a page saved as a
  kit on the card, sounds plus settings, 303 tracks as 303 state (knobs, no
  steps), loops left out. Save over and delete ask first. Rename. A track
  holding a recording is never replaced by a kit.
- ROLL (hold) on the Perform strips.
- Keyboard knobs, pitch bend and pads (channel 10) on the selected track.
- T95 side, no flash needed: trim and normalise before sending (tap a file
  in the library).

Left out on purpose, say if wanted: auto-stop of a one-shot on silence, a
pre-roll for one-shots (the first fraction of a millisecond below the
threshold is lost).

Known limits: looper and quantise settings are not saved over a power cycle.
A loop that started on a beat goes back to the bar line when its set is
loaded again. Undo is one take deep (undo and redo go back and forth). On
the APC40 one-shot sampling and kit save have no button until you give
them one on the map (REC still arms a loop).

## To check on the rig (built, desktop-tested only)

- TV sound and LISTEN at the same time (shared capture), and who reset the
  TV sound setting to off on 2026-11-23.
- The ApcBox page on the TV at 120 BPM (speed), pots, faders and step cells
  with a thumb on the phone, the phone on its side.
- Safari: record, hearing a sample, typing a set name, the sideways layout.
- Samples view, kits, sets from the page (v13.10 flashed?).
- Clock module: Cubase leading over network MIDI, Link with Live or an iPad,
  tick lateness on the T95 itself.
- Network MIDI: the T95 inviting the Mac by itself (so far the Mac
  connected), notes both ways through the hub, an iPad.
- Keyboards through the T95: sound check. Sample transfer speed figure.
- Boot straight into the last mode. The remote after relearning (OK button).
- Daisy: the screen's 7-9 ms per loop, the one-off 52 ms ping spike.
- Mac build of the shared code (`daisy/mac/build.sh`) since v13.9.
- v13.15 boots, plays and the APC40 map works on the rig (2026-11-23). Still
  to try there: the items below.
- v13.15 on the Seed: SDRAM is now 98% used (the looper's undo takes one more
  slot, at the top of the memory nothing used before). Watch the boot log and
  try an undo of a long take. Overdub and one-shot levels with the real
  input; the one-shot start threshold with real noise on the line.
- Clock follow on the Seed (v13.11 part): error figure on the Clock page.
- Kit save on the real card: how long 8 sounds take, a rename and a delete
  (FatFs rename of a folder is untested on the Seed).
- ROLL and the trim markers with a thumb; keyboard knobs through the T95
  (which CCs the MPK249 actually sends).

## APC40 button map

- Done (v13.15, not flashed): the whole layout can be changed on System >
  APC40 map. Buttons alone and with SHIFT, pads and strip rows (8 at once),
  knobs and faders among themselves, six jobs no button has (one-shot
  sample, save kit, take length, count-in, pattern and FX quantise). Lights
  and knob rings follow. Saved on the Daisy's card.
- Limits: a job is always "what control X does in the standard layout", so
  jobs that depend on a mode keep doing so (pad row 5 is a keyboard on a 303
  track wherever it sits). SHIFT and CUE LEVEL stay. Lights show the job a
  button does alone, not its SHIFT job. A button without a light (PLAY,
  STOP, REC, TAP, NUDGE, BANK SELECT, STOP ALL) can't show anything.
- To check on the APC40: lights of moved jobs (a pad showing a mute, a strip
  button showing an FX), rings of moved knobs.
- Later: the same for other controllers (keyboards' knobs and pads, MIDI
  learn); more jobs without a button as they come up; per-set maps.

## Modules

- **Settings** (start screen, top right): first slice built 2026-11-23,
  NOT yet run on the T95 (`t95/helper/README.md`). In it: Wi-Fi status, scan,
  join as a trial that goes back by itself (seconds on a wrong password), forget, box name; Bluetooth
  on/off (needs `sudo t95/helper/get-bt-tool.sh` once); restart bridge,
  reboot, shut down, start-up delay; a PIN for everything that changes the
  box; devices (Daisy, controllers). The box's own hotspot is built (2026-11-23)
  but parked: on the rig the phone joined it and the page at its address
  didn't load (not looked into; "by itself" is off by default until it
  works). The remote's buttons and TV sound moved from the start screen
  to Settings (Remote, Devices); the TV remote walks the Settings page. Still to build: Bluetooth pairing and
  Bluetooth MIDI, sound cards and SD cards under Devices, typing a PIN with the TV remote.
  Rene, 2026-11-23: the join works on the rig (wrong password comes back),
  but the error and info banners are ugly and stay too long. UI pass later.
- **Cubase sync**: a VST3 plugin in Cubase that reads its transport and
  shares it on Link (Cubase can't follow clock or speak Link). Pinned.
- **Numark DJ**: likely Mixxx on the T95 in the kiosk; which Numark?
- Chaining: two modules at once (hub + ApcBox, clock + anything) with
  routes between them. The data model allows it; one is active today.
- More Seeds: three exist; other bodies doing other things later.

## ApcBox views and firmware features

- Song / chain mode (patterns in a row). Slice (chop a sample over steps).
- Keys view: on-screen keyboard and pads, scale lock.
- Per-step velocity and probability (a step is off / on / accent today).
- The Daisy's own SD card browsed from the T95; sets copied to and from the
  T95 (backup); a whole kit sent from the T95 library in one go.
- 303 cutoff shown in Hz (a percentage today).
- Undo for a sample sent from the T95.
- Kit names from a page are 16 characters, lower case (the command line to
  the Daisy is 31 characters).
- Looper: settings saved with the project; more than one undo; one-shot on
  an APC40 button; learn for keyboard knobs.

## MIDI and clock

- DIN MIDI when the parts arrive (UART4 on D1/D2; D13/D14 are the screen's).
  USB-to-DIN cables as ports meanwhile.
- Bluetooth MIDI: the T95's own radio works (scan found 21 devices on the
  rig); pairing and BLE MIDI are the next experiment.
- RTP-MIDI recovery journal (none today: a lost packet is a lost message).
- Link: start/stop sync and bar alignment when the T95 leads.
- MIDI clock following: tempo ramps lag by the fit window (4 beats).

## T95 and pages

- LISTEN: measure the real delay; a lighter format than raw PCM if Wi-Fi
  needs it.
- Add-to-home-screen for the phone (full screen, no Safari bars).
- T95-to-Daisy audio over USB (the Daisy as the T95's sound card).
- Countdown on the start screen: 5 s today, his call to keep it.
