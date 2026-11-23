# GhettoRack backlog

Everything agreed, mentioned or left open that isn't built yet, so it doesn't
get lost. Newest state: 2026-11-23. Move a line out when it's done (the
status tables are `t95/README.md` and `t95/ui/VIEWS.md`).

## In the firmware to flash (v13.13, built, desktop-tested, NOT flashed)

`daisy/release/ApcBox.bin`. Control protocol 8, report 14.

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
  press goes back to what the track held before the browsing started).
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
the APC40 there is no button yet for one-shot sampling (REC still arms a
loop; it follows the take length and beat settings).

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
- v13.13 on the Seed: SDRAM is now 98% used (the looper's undo takes one more
  slot, at the top of the memory nothing used before). Watch the boot log and
  try an undo of a long take. Overdub and one-shot levels with the real
  input; the one-shot start threshold with real noise on the line.
- Clock follow on the Seed (v13.11 part): error figure on the Clock page.
- ROLL and the trim markers with a thumb; keyboard knobs through the T95
  (which CCs the MPK249 actually sends).

## Modules

- **Settings** (start screen): network manager (Wi-Fi, address, name),
  remote control setup, peripherals (MIDI devices, audio cards, SD),
  Bluetooth. The remote setup and start-up options move there from the
  selector.
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
- Undo for a kit loaded by accident, and for a sample sent from the T95.
- Looper: settings saved with the project; more than one undo; one-shot on
  an APC40 button; learn for keyboard knobs.

## MIDI and clock

- DIN MIDI when the parts arrive (UART4 on D1/D2; D13/D14 are the screen's).
  USB-to-DIN cables as ports meanwhile.
- Bluetooth MIDI (experiment).
- RTP-MIDI recovery journal (none today: a lost packet is a lost message).
- Link: start/stop sync and bar alignment when the T95 leads.
- MIDI clock following: tempo ramps lag by the fit window (4 beats).

## T95 and pages

- LISTEN: measure the real delay; a lighter format than raw PCM if Wi-Fi
  needs it.
- Add-to-home-screen for the phone (full screen, no Safari bars).
- T95-to-Daisy audio over USB (the Daisy as the T95's sound card).
- Countdown on the start screen: 5 s today, his call to keep it.
