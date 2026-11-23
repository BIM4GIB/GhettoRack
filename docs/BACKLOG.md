# GhettoRack backlog

Everything agreed, mentioned or left open that isn't built yet, so it doesn't
get lost. Newest state: 2026-11-23. Move a line out when it's done (the
status tables are `t95/README.md` and `t95/ui/VIEWS.md`).

## Before the next flash (firmware v13.11 is built, not flashed)

In v13.11 already: the Daisy follows the T95's clock, the T95 can follow the
Daisy. To add before flashing (to be agreed, see the proposal in the chat):

- Sampler: trim start/end and normalise before sending (T95 side); record
  the Daisy's input straight into a one-shot sample (not a bar-long loop).
- Looper: fixed take lengths (1/2/4/8 bars, stops by itself), overdub, undo
  the last take, count-in, input level and monitor on the page.
- Quantise: when a pattern switch lands (bar now; beat or at once), when a
  loop recording starts (bar now; beat), whether muted/FX changes snap.
  Recording hits already snaps to the nearest step (there is no off-grid
  timing in a pattern).
- Roll (hold to repeat a track) from a page; on the APC40 it's the REC ARM
  button.
- Keyboard knobs, pads and pitch bend into ApcBox (notes only today).

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
