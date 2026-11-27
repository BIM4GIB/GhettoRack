# GhettoRack backlog

Everything agreed, mentioned or left open that isn't built yet, so it doesn't
get lost. Newest state: 2026-11-27. Move a line out when it's done (the
status tables are `t95/README.md` and `t95/ui/VIEWS.md`).

## v13.36: watchdogs for a main loop that stopped (on the Pi rig's Seed, 2026-11-27)

`daisy/release/ApcBox.bin` is v13.36. `daisy/main.cpp`, `daisy/diag.h` only: no protocol or
report change, no new buffer. A Seed whose main loop stops restarts by itself; the write-up
is the comment "the watchdogs" in `daisy/main.cpp`.

- **Two watchdogs.** The audio callback counts the milliseconds since the main loop's last
  pass and resets the board after 8 s. The chip's own watchdog (33 s, the longest it goes)
  is fed by every pass and around every card call, and is what is left when a card call never
  returns or the interrupts are dead too. 33 s because libDaisy's card driver may wait 30 s in
  one call: a card that stalls gets to fail by itself instead of taking the box down. While a
  card call runs, only the 33 s one counts.
- **Afterwards** ApcBox starts as after any restart (the last saved set; what was not saved is
  gone) and says where the loop had stopped, every 10 s for a minute: `watchdog: the main loop
  had stopped in '...' (8 s), restarted, 1 in a row`. Three in a row with no run lasting a
  minute: report mode (LED three blinks, `STOP REPORT`), so a box that hangs on the same thing
  every time does not restart for ever. Report mode now takes `ESC BOOT`: another firmware goes
  on with `tools/flash-seed.sh`, no hand on the buttons.
- **A card call in the firmware's platform code goes inside a `CardCall`** (`daisy/main.cpp`).
  One that does not is not covered by the 33 s and can be cut off at 8.
- **Measured before** (v13.35, same Seed): the slowest main-loop pass was 0.52 s (undo of a
  sample), 0.30 s in a set save.
- **Tried on the Seed** with a build that hangs on `restart` (`make -C daisy WD_TEST=1`, never
  a release): main loop stopped with the interrupts running, gone from the USB 8.1 s later;
  with the interrupts off, 33.7 s; the log line after each; the third in a row into report
  mode; out of report mode by flashing over USB. Then the release build: `restart` (no watchdog
  line), a 14 s sample sent, a set with it saved (1280 KB), loaded, deleted, six folders
  stepped and a file loaded from each: no reset, slowest pass 0.54 s (the default set coming
  in), 0 bad lines.
- **Not tried**: a hang that is real rather than made; a card pulled in the middle of a read
  (how long the driver then waits is read from its source, not seen); a long run; the T95
  rig's Seed (v13.31, untouched). The sound keeps playing while only the main loop is stopped
  and stops at the reset: not listened to. If the bootloader ever clears the chip's reset
  flags, a 33 s reset would look like a plain restart (on this Seed, bootloader version 3, the
  flag was there).
- **Still open: a start-up that stops** is not restarted by a watchdog (they start with the main
  loop); the next start goes to report mode as before.

## v13.35: a folder's files by name, undo for a sent sample, the Daisy restarted from a page (2026-11-27)

`daisy/release/ApcBox.bin` is v13.35: control protocol 14, report version 20. No new buffer
(SDRAM as before, 95.27%).

- **A list of a folder's files on DAISY CARD** (small page, LIB): the folder stepped with ‹ ›,
  its files five to a page by name, a tap loads that one into the selected track, the row of the
  file the browser is on is lit and the list follows the encoder and the APC40. New: `files start
  count` answered in `BL folder index name` lines, and `file at n`. The big pages have no DAISY
  CARD view; nothing changed there.
- **Undo for a sample sent from the box**: the transfer keeps what the track held, like a file
  browsed on the card. `lrec undo` brings it back, `lrec redo` the sent sample (it exists only
  in the track's memory, so the undo memory keeps the sound itself; one longer than a slot has
  no redo). UNDO on the small page's LIBRARY and BOX CARD tabs; the big page's looper UNDO
  does the same.
- **RESTART DAISY**: `restart` in `ESC U` and the firmware resets itself (`NVIC_SystemReset`
  in `daisy/main.cpp`), as the RESET button does. On ApcBox's SYS view (small page) and in
  Settings > System (every layout); the first tap asks. It needs a firmware that still answers:
  a Seed that hangs is not reached by it. The watchdogs of v13.36 (above) are for that case. **Not built: a wire** from a Pi header pin to the Seed's reset, for a Seed that
  no software reaches; whether the Seed has its reset on a pin is not checked.
- **On the Pi rig's Seed (2026-11-27, through the bridge's `/do`, not by touch):** `restart`:
  the Seed left the USB and was back 5.0 s later on v13.35, on the default set (nothing was
  saved), 0 bad lines. `folder 1`, `files 0 5`: five `BL` lines with the names in `samples/bass`;
  `file at 2` loaded the third. A 2.3 s cut of a jam take sent from the box to track 12, then
  `lrec undo` (the 909 open hat back) and `lrec redo` (the cut back). For the watchdog: the
  Seed's own log gave a slowest main-loop pass of 522 ms in that stretch. Not seen: the buttons
  on the 3.5" screen under a finger, the sound of any of it.
- Tested: `sets_test.cpp` (undo, redo, undo, redo of a sent sample with the sound compared;
  `files` and `file at` with bad values), the bridge suite against the firmware's control code
  (`test_p58` the list, `test_e2d_sample_to_the_real_firmware` undo and redo).

## v13.32: the Daisy's card from a page (on the Pi rig's Seed, 2026-11-26)

`daisy/release/ApcBox.bin` is v13.32. `daisy/box.cpp`, `ports.h` (control
protocol 11). No report line changed.

- The samples on the Daisy's own card could only be reached with the encoder
  or the APC40: a page had no action for it, and on the 3.5" screen the card
  was a black box (Rene). Two page actions now: `file d` (d files on, into the
  selected track, on into the next folder past an end) and `folder 1 / -1` (the
  next folder that has files). `B` / `BF` / `BN` already said where the browser
  is.
- The small page: LIB > DAISY CARD, with the card's state, the folder and the
  file by name and number, ‹ › (and ‹‹ ›› for ten files), HEAR TRACK, UNDO.
  LIB > CARD is now BOX CARD: it was always the card in the box's own slot.
- `sets_test.cpp` (folder steps never land in an empty one, a file loads after
  the 120 ms wait, back, undo, bad values ignored) and `test_p58` in the bridge
  tests (the tab against the firmware's control code, with a card of samples).
- Seen on the Pi rig: 20 folders; two folder steps to `clap`, two file steps to
  "Clap 707" in track 1 on the Daisy, UNDO back to the 808 kick; the saved set
  loaded again afterwards so nothing stays marked unsaved.

## Small things from the list (2026-11-27, browser-tested, nothing tried on a panel or a phone)

- **A remote or a game controller on every small page**: `Small.remote` in `t95/ui/small.js`, one
  piece for the start screen, jam recorder, clock, hub and Settings. Arrows move a white outline to
  the nearest button, OK taps, BACK cancels the keyboard or goes back, HOME goes home; PLAY and
  STOP belong to the clock and the jam recorder there. Tested with arrow keys in a browser
  (`test_p57`). **Not tried: the pad or a remote on the 3.5" screen.**
- **A hub preset can be renamed**: `POST /presets/rename`, RENAME on the small SAVED tab, "rename"
  on the big page.
- **The 303's cutoff reads in Hz** on the Sound view (150 Hz to 4.8 kHz: where the filter rests,
  from `daisy/acid.h`; the envelope opens it from there). The page's arithmetic, not a report.
- **The jam recorder in ApcBox's top bar** (TV, Mac, iPad): JAM REC starts a take, the button is
  red while one runs, a tap asks STOP TAKE? and the second ends it (the question goes after 4 s).
  A phone has no room: there only the red lamp of a running take. The 3.5" screen: "jam rec"
  with the CPU figure. Tested against a made-up `arecord` (`test_j55`).
- **Add to home screen**: every page names a manifest and an icon (drawn by the bridge, no image
  file). **Not tried on a phone**: whether Safari and Chrome then open it full screen, and
  whether a page change stays inside it.
- Was done already: the start screen and Settings at 480 px (`pi4/README.md` said otherwise).

## Names on the 3.5" screen (2026-11-27, browser-tested)

- ApcBox LIB > SETS: SAVE AS NEW (a name typed, or none for one made by itself), RENAME, DELETE
  beside LOAD, SAVE HERE and DEFAULT SET. LIB > KITS: SAVE 1-8 / 9-16 (the selected track's page)
  under a typed name, asking before it replaces a kit; RENAME and DELETE for kits on the card.
  MIDI hub > SAVED: SAVE AS NEW asks for the name. Jam recorder: RENAME (2026-11-26).
- The ApcBox page has a keyboard of its own (`smKeyboard`, the keys a name on the Daisy's card
  may have); the other pages use the one in `small.js`.

## A game controller on the box (2026-11-26, on the Pi, real presses NOT watched yet)

- A PlayStation 3 pad by USB cable: the Pi's kernel knows it (17 buttons, both sticks and the
  two triggers 0-255, tilt as a second device; the face buttons' pressure is not given).
  `t95/bridge/rackbridge/gamepad.py` turns the buttons into the remote's actions for the pages
  and the sticks into sweeps that spring back (table in `t95/bridge/README.md`).
- Tested with a stand-in pad against the simulated firmware (`test_p59`). On the Pi the bridge
  found the real pad; nobody has pressed a button on it with this running.
- 2026-11-27: a map of it, like the APC40's: System > Controller map draws the pad, lights what is
  pressed, and gives a control another job (`GET` / `POST /pad`, kept in `state.json`); SYS > PAD
  MAP on the 3.5" screen reads it and resets. Browser-tested with a stand-in pad. Rene at the
  rig with the real pad: the drawing matches the buttons.
- The small layouts of the start screen, jam recorder, clock, hub and Settings take the D-pad
  from 2026-11-27 (`Small.remote`, above).
- Next: the pad by Bluetooth (pairs over the cable once; delay to be measured), triggers as
  notes with velocity (needs a keyboard port to the Daisy), tilt.

## v13.34: the default set, any time (on the Pi rig's Seed, flashed 2026-11-26)

- The set the box starts with is now a set like any other, built in memory (`DemoFill` in
  `daisy/box.cpp`) and switched in on the bar by `set demo`: no card, no slot, nothing can save
  over it. DEFAULT SET on both ApcBox layouts (a second tap when there are unsaved changes), the
  seventh action in the Daisy's SETS menu (encoder and OLED; not on the colour screen's row of
  six), pad 6 of row 4 on the APC40's sets screen. Control protocol 13.
- It is longer: bank A as in v13.31 (Rene: sounds great), bank B the second half: groove,
  arpeggio, second break, a longer roll, peak, broken peak, stripped, outro. Every track has a
  part; the 808 is on the crossfader's A side, the 909 on B, the 303s on both.
- Tested: `daisy/sets_test.cpp` plays all 16 stages (none silent, none clipping: peak 0.74 of
  full scale) and `APCBOX_DEMO_WAV=<file>` keeps the play-through as a WAV; the page tests load it
  from both layouts. **Nobody has heard bank B yet.** No performance FX are preset: they are
  momentary, a set does not store them.
- After loading it the box remembers "no set open", so the next power-up starts with it too.
- On the Pi rig's Seed: `set demo` with the transport stopped gave 128 BPM, no set open, the
  kits on their crossfader sides, slot 1 untouched; `set load 1` brought the saved set back.
  Not tried there: the switch while playing, the buttons on the pages, the APC40 pad.

## v13.33: a save doesn't replace a set (on the Pi rig's Seed, flashed 2026-11-26)

- `set save n` from a page or the bridge is refused ("n is used") when it would replace a set:
  a used slot that isn't the open set, or the open set under another name. `set over n` does
  it, and both ApcBox layouts send that only after their second tap. Control protocol 12.
  Rene's set "128bpm" was lost on 2026-11-26 to one `set save 1 <new name>` sent by hand.
- The Daisy's own SETS menu already asked for a slot that isn't the open one; unchanged.
- Tested: `daisy/sets_test.cpp` and the page test `test_p7`. On the Pi rig's Seed, with throwaway
  sets in free slots: `set save` under another name refused, `set save` onto a slot that isn't the
  open set refused (its tempo stayed), `set over` replaced it; the saved set came back after the
  flash and was left as it was. The T95 rig's Seed is still on v13.31. Not tried: the pages' second
  tap on a real Seed.

## The Pi's settings: NetworkManager (2026-11-26, read-only on the Pi)

- The helper knows NetworkManager (`NmHelper` in `t95/helper/ghettorack_helper.py`): scan, a
  join as a trial that goes back, confirm, forget, hotspot (by hand and by itself), Bluetooth
  without the attach tool. 4 tests against a stand-in `nmcli`; the 12 older ones unchanged.
- On the Pi: installed, status and scan right, Settings' tabs right on the 3.5" screen.
- **To try with Rene at the rig, in this order:** set a PIN; Bluetooth on, pair a device; join a
  second network with a wrong password (must come back in seconds), then the right one; hotspot
  on with a phone, then without (must go back after 2 minutes); BY ITSELF only after those.
- Not known: whether the Pi's radio scans while it is an access point (the step-aside covers
  it either way), and what NetworkManager does with the trial profile's file name on rename
  (the helper never deletes a file whose `id=` is not the trial's).

## Open from the rig, 2026-11-26

- **A card put back while the Seed runs is not found** (Pi rig's Seed, v13.31): `SdPoll` in
  `daisy/main.cpp` retries the mount every 2 s and never got it; a restart did. Not looked
  into: whether the card needs its power-up sequence again, or the SD driver is left in a
  failed state by the removal. `sd_last_code` is only logged for "not FAT32": log it.
- **A Seed's main loop stopped once** (v13.30, Pi rig): audio and USB audio went on, no
  report lines, no answer to a state dump. Cleared by a power cycle, which also cleared the
  record of where it was. Not reproduced in 260 sample loads. Press RESET, not the plug, if
  it happens again: the firmware then says the stage.
- **The T95 left the Wi-Fi during `t95/bridge/install.sh`** (2026-11-26, after flashing its
  Seed to v13.31). The installers finished and the TV kept showing the pages; no ping, no
  name on the network, nothing announced. Rene rebooted it and it was back. Its journal is in
  memory, so the reason went with the reboot. Suspect, without evidence: the installer's
  restart of `ghettorack-helper`; or that Wi-Fi chip. Next time, before a reboot: on the TV,
  Settings > Network and Hotspot, and `journalctl -u ghettorack-helper` over the TV or a
  cable. Worth a persistent journal on that box.

## v13.31: the box starts with a demo set (on the Pi rig's Seed, 2026-11-25)

`daisy/release/ApcBox.bin` is v13.31. `daisy/box.cpp` only (`DemoSet()`, where
`DemoPattern()` was); no report line changed.

- With nothing saved on the card, and after a full reset, the box used to hold
  one four-on-the-floor beat on A1. Now: one track in eight stages on A1 to A8
  at 128 BPM. A1 kick, hats, bass; A2 clap; A3 open hats and the lead; A4
  everything; A5 a broken kick; A6 the breakdown; A7 a snare roll; A8 the drop.
  808 drums on tracks 1-7, a 303 bass on 8, the 909 on 9-15, a 303 lead on 16
  (square, into the delay). All from the firmware: no card needed.
- The 303s are tracks 8 and 16, the last of each page: the box's desktop test
  records a loop on track 7.
- A set or project saved on the card still loads instead, as before.
- `daisy/sets_test.cpp`: 128 BPM, a kick in every stage but A6, the bass in
  all, the lead from A3, bank B empty, the two 303s and their notes, accents
  and slides, and that a full reset brings it back.
- Seen on the Pi rig's Seed through the bridge: 128.0 BPM, tracks 8 and 16
  as 303s, A1's steps, playing with meters moving, CPU 15 %, no overload.
  Rene, through the Pi's AV socket: sounds good. Written without hearing it;
  not tuned further. The T95 rig's Seed is still on v13.30.

## v13.30: the encoder with no colour screen, named on the OLED (on the Pi rig's Seed, 2026-11-25)

`daisy/release/ApcBox.bin` is v13.30. `daisy/box.cpp` (shared with the Mac
build), `box.h`, `main.cpp`. No report line changed.

- With no colour screen found (v13.29), a push of the encoder stepped through
  five views nobody could see. Now it steps through the three jobs a turn has:
  **tracks** (select), **patterns** (queue the next / previous), **samples**
  (step through files into the selected track). Back, or the push held 0.7 s:
  tracks. Each push says it on the OLED's message line ("encoder: patterns").
- The OLED's footer always names the job in a box: TRK, PAT or SMP, between
  the CPU figure and the card / APC40 state. Also with a colour screen.
- Samples: a turn past either end of a folder, or in a folder without files
  (the top one, where the box starts), goes on to the next folder that has
  some, and says "folder: file". Before, a turn in the top folder did nothing,
  and the folders were only reachable with the APC40's BANK buttons. This
  holds with a colour screen too (its SYSTEM view): files no longer wrap
  around inside one folder.
- With a colour screen the push still steps through its five views.
- `daisy/sets_test.cpp`: the three jobs, back, the popup texts, the footer
  box differing per job, the five views with a screen. Not in a desktop test:
  the walk across folders.
- Seen on the Pi rig's Seed, driven through the bridge (`/ui?a=push` and
  `turn`) with the OLED read back from its `D` line: tracks, patterns (A1 to
  A2 and back), samples (into `bass`, two files on, three back into `vocals`,
  the last folder). **Not seen: the real encoder and back button turned by
  hand, the OLED itself.** The T95 rig's Seed is still on v13.29.

## v13.29: nothing is drawn when there is no screen (on the Pi rig's Seed, 2026-11-25)

`daisy/release/ApcBox.bin` is v13.29. `daisy/tft.h` and `daisy/main.cpp` only;
no report line changed. No desktop test reaches this code.

- At power-up the firmware selects the screen's touch chip and reads its data
  line (D9) with a pull-up, then a pull-down. A chip drives the line; a pin
  with nothing on it follows the pull (high, then low). Only that pair means
  "no screen": then nothing is composed or sent, touch is not read, and the
  3 s encoder hold does not open touch calibration. Encoder and back button
  still work.
- The state is logged again 8 s after power-up (`screen: drawing` or
  `screen: none found, not drawing`, with the two readings): lines from
  power-up are gone before a bridge or a monitor listens.
- A screen wired without its touch lines can't be found: the encoder push
  held at power-up then means "there is a screen" (second byte of
  `apcbox/screen.bin`), and does not switch the bus mode on that hold.
- Seen on the Pi rig's Seed (no screen): `none found, not drawing (touch data
  line: 1 pulled up, 0 pulled down)`; the line `slowest pass 119.5 ms ... tft
  114.9` that came after every start is gone. Idle ping the same as before
  (avg 0.51 against 0.56 ms): the 115 ms was once at power-up, not every pass.
- **Not seen: a Seed with the screen on it.** The only screen is on the Pi.
  That a selected XPT2046 drives its data line is from its data sheet, not
  from this rig. If the screen ever goes back on a Daisy and stays dark: hold
  the encoder push at power-up. That override is not tried either.
- A first try read the chip over SPI at start-up; the transfer came back
  failed there (first byte 0x80, the rest 0), so that was dropped.

## v13.28: two faults in the overload guard (on the rig, flashed 2026-11-25)

`daisy/release/ApcBox.bin` is v13.28, the binary on the Seed. `daisy/main.cpp`
only. No desktop test reaches this code; seen on the rig.

- 303 tracks that were left out could stop being 303s (a type change, a kit,
  a set) while the guard still counted them: the count ran over onto the
  reverb, which went out for some seconds at 14 % CPU ("all 0 303 voices and
  the reverb left out"). The count now goes down with the tracks at once,
  and the reverb only goes when the guard itself took that last step.
- The guard took "the main loop has not come round for 0.2 s" as an overload
  whatever the CPU. The main loop stays away that long doing its own work (a
  sound rendered for a track whose type changes, a kit loading), so it
  dropped voices at idle. That now counts only above 80 % CPU.
- Seen on the rig after both: eight type changes and two kit loads at idle,
  nothing dropped; overload with eight 303s, then all of them turned back
  into sample tracks: the line follows down to `OV 0 0`, the reverb flag
  never set.
- Still so: a type change during an overload (90 %) drops one more voice.
- Found because Rene pressed play on a stress pattern left in the Seed's
  memory after a test. After a test the Seed is left empty, or it is said
  plainly what is in it.

## v13.27: a stack depth meter (on the rig, flashed 2026-11-25)

`daisy/release/ApcBox.bin` is v13.27, the binary on the Seed. No change to the
protocol.

- The stack has the whole DTCM (128 KB) and nothing else is in it. `main()`
  paints it with a pattern, the main loop looks every 10 s for the lowest
  word that changed and logs "stack: deepest so far N bytes of 131072" when
  it has gone deeper (`StackPaint`, `StackPoll` in `daisy/main.cpp`).
  Interrupts use the same stack, so the audio callback is in the figure.
- Seen on the rig: 4552 bytes after start-up, 4732 playing 8 sample tracks
  and four 303s, 5296 after a kit load from the card, and no deeper through
  performance FX, a roll, pattern copy/clear/undo, takes armed and
  cancelled, the encoder through the screen views, a set saved, loaded and
  saved again, a kit saved, and the overload guard cutting in. 4 % of the
  DTCM.
- Not exercised: a sample sent from the T95, the touch screen, a take kept
  (only armed and cancelled), a set switch between two sets with long loops,
  the SD card pulled, hours of use. The line stays in the firmware, so the
  log will say if real use goes deeper.
- So about 120 KB of the fastest RAM is free. The reverb's delay lines (99
  KB) would fit with over 20 KB left for a stack that has shown 5.3.

## v13.26: the reverb's memory out of SDRAM (on the rig, flashed 2026-11-25)

`daisy/release/ApcBox.bin` is v13.26, the binary on the Seed. No change to the
protocol. SDRAM 95.3 % (was 95.9), the main internal RAM 70 % as before, the
D2 RAM 38 % (was empty).

- DaisySP's ReverbSc keeps its eight delay lines four times further apart
  than they are long (it adds a byte count to a float pointer), so the object
  is 396 KB and only fits the SDRAM. `daisy/reverb.h` is a copy with the
  lines packed: 99 KB, in the D2 RAM (a new `.d2_bss` section in
  `daisy/ApcBox_qspi.lds`; that RAM was empty). LGPL like its source, generated
  from the library files, header kept. `daisy/reverb_test.cpp` renders both:
  20 s of noise bursts and tails, 0 samples differ.
- Seen on the rig, 8 sample tracks on every step, the clap's send on and off,
  four readings each, twice: 50 % and 40 %, so the reverb costs about 10
  points where it cost about 13 (v13.25: 52 and 39). A small gain, not the
  cure. Stopped 14 %. Measured first in the main RAM (which it took to 89 %),
  then in the D2 RAM: the same, 48 % and 37 %.
- The Seed's internal RAM, v13.26: main (AXI) 512 KB, 366 KB used: sets and
  their names about 105, file browser names 48, engine 43, MIDI buffers 37,
  serial link 29, kits 22, SD scratch 16, the rest small. D2 256 KB: the
  reverb, 99. D2's DMA part 32 KB: 26. DTCM 128 KB: only the stack. D3 64 KB:
  empty. ITCM 64 KB: 27 of audio code.
- Not tried: the DTCM (128 KB, no cache in the way). The stack lives there
  and 28 KB would be left for it; how deep the
  stack goes is not measured, so not without that.

## v13.25: the reverb sleeps when nothing feeds it (on the rig, flashed 2026-11-25)

`daisy/release/ApcBox.bin` is v13.25, the binary on the Seed. No change to the
protocol (control 10, report 19) or SDRAM.

- Where the idle CPU went, timed on the Seed part by part (v13.24, stopped,
  nothing loaded; a block has 1000 us): reverb 138, the 16 tracks 49, delay
  25, master and limiter 25, per-block settings 21, performance FX 14, USB
  audio push 11; 300 in all. With 8 sample tracks on every step: tracks 277,
  reverb 146, 546 in all. The reverb ran at full cost on silence.
- Now it is not run while nothing is sent to it and its output has been under
  -100 dB for half a second, and it wakes on the first sample that is sent.
- Seen on the rig: stopped 14 % (was 30). 8 sample tracks with the clap's
  default send: 51 to 52 % as before; with that send at 0, 39 % after the
  tail; send back up, 52 % again. Desktop: `acid_engine_test` (asleep after
  silence, output exactly 0, awake on a hit).
- Not listened to: whether waking is clean on the first hit after a long
  silence. The reverb's state stays as it was while it sleeps.
- Open, from the same measurement (the reverb's memory: v13.26). A playing
  sample track costs about 28 us, more than its arithmetic (samples are in
  SDRAM too). 16 idle tracks cost 49 us for their gain ramps.

## v13.24: the overload shows (on the rig, flashed 2026-11-25)

`daisy/release/ApcBox.bin` is v13.24, the binary on the Seed. Report 19 (`OV`),
control 10, SDRAM as before. Firmware and page go together.

- `OV mask reverb`: the 303 tracks left out and whether the reverb is. The
  ApcBox page has a red "OVERLOAD: 4 of 8 303s out" in the top bar of every
  view, dims the mixer strips of those tracks with a red outline, and names
  them under System > Links. The Daisy's screen has the same line in a red
  bar under the header, and a popup when it sets in.
- Seen on the rig with the eight-303 pattern: `OV 61440 0` (tracks 13 to 16),
  the chip and the four dimmed strips on the page, `OV 0 0` after stop.
  Desktop: `sets_test` (the line), the page test (the chip, from an injected
  line; the Mac cannot build the firmware simulator, so not end to end).
- Not seen: the bar on the Daisy's own screen (nobody looked), the OLED, the
  page on the TV and on a phone. Nothing on the APC40's lights.
- Rene has listened to v13.22 and v13.23 (said 2026-11-25); how the guard
  cutting in sounds was not part of that.

## v13.23: overload guard (on the rig, flashed 2026-11-25)

`daisy/release/ApcBox.bin` is v13.23, the binary on the Seed. Control 10,
report 18, SDRAM as before. No report line for it: it says so in the log.

- When the audio needs more than its block the main loop gets no time: no
  screen, controls or reports, and nothing to stop it with (v13.21, eight 303s
  on every step: reset by hand). Now the audio callback watches its own time
  (`Guard` in `daisy/main.cpp`) and leaves 303 voices out in the engine
  (`Engine::shed`), highest track first: one more every 0.1 s while a block
  takes over 93 % or the main loop has not come round for 0.2 s, and after
  the last 303 the reverb. They come back one every 2 s under 65 %. The
  sequencer runs on, so a voice comes back in time.
- Seen on the rig: 8 sample tracks plus eight 303s on every step at 140 BPM:
  4 of the 8 left out, 86 % average, the Seed went on reporting and took
  stop; all back within 10 s of stopping. One main loop pass of 248 ms as it
  set in. 8 sample tracks plus one 303 (57 %): the guard did nothing.
  Desktop: `acid_engine_test` checks that shed 303s are silent, their notes
  still fire and they come back.
- Open: a voice is cut
  without a fade. Between 65 % and 93 % nothing comes back by itself, so a
  heavy pattern keeps its dropped voices until it gets lighter. Sample
  tracks are never dropped. Single blocks still peak over 100 % in that
  state (107 to 120 %).

## v13.22: the audio code in fast memory (on the rig, flashed 2026-11-25)

`daisy/release/ApcBox.bin` is v13.22, the binary on the Seed. No change to the
sound, the protocol (control 10, report 18) or SDRAM.

- All code ran from the QSPI flash. What the audio callback runs (27 KB: the
  engine's per-sample code, the 303, reverb, filters, performance FX, the USB
  audio push, libDaisy's audio and DMA handlers, the maths it calls) is now
  linked into the ITCM, 64 KB of zero-wait code memory at address 0, and
  copied there at start-up (`daisy/ApcBox_qspi.lds`, `ItcmLoad` in
  `daisy/main.cpp`). Functions are picked in the linker script by name;
  nothing in the sources says where they go.
- Seen on the rig, same pattern as before (8 sample tracks on every step, then
  one 303 with 4 notes): stopped 28 % (was 38 to 44), 8 tracks 53 % (was 67
  to 71), with the 303 60 % average and 80 % peak (was 71 to 76 and up to
  124). The main loop's slowest pass 16 ms (was 40 to 55). USB audio whole.
  A 20 s take of it: no gaps.
- `python3 daisy/check_itcm.py` after a build: checks on the desk that the
  copy puts the right bytes at the right addresses and that nothing the reset
  code needs sits in the ITCM. A build that fails it does not boot. Three
  builds died on the Seed before this check existed: `memset` had moved (the
  reset code calls it before the copy), then the copy read the flash 32 bytes
  off.
- Seen on the way: the ITCM is on after reset here (ITCMCR 0x3b), a
  `.preinit_array` hook runs before the constructors, and nothing in the
  firmware wrote to addresses 0 to 64 KB during start-up and 45 s of use (a
  null pointer write would land there).
- A trial build can carry a watchdog (`make -C daisy ITCM_TRIAL=1`): a build
  that hangs then resets into the bootloader every 20 s, and a loop on the
  T95 that flashes the release binary when the bootloader shows up puts the
  good firmware back with nobody at the rig. The good firmware then shows its
  stop report and still wants one press of reset.
- 28 % with nothing playing: measured and mostly cured in v13.25.

## USB audio under load (v13.21 rebuilt 2026-11-24, on the rig)

`daisy/release/ApcBox.bin` is v13.21 with this in it (the version number did
not move: the Seed runs exactly this binary). Found with a 33 minute jam take.

- At about 72 % CPU the Daisy sent 3 % fewer USB audio packets: gaps in the
  browser, the TV sound and the jam take (11 s short of the clock). libDaisy
  has every interrupt at priority 0, so USB waited behind the audio callback.
  The audio interrupt is now one level down (`daisy/main.cpp`).
- Seen on the rig: 90 s at 76 % average CPU, 48012 frames/s, no skipped or
  missed packets. Rene listened in the browser afterwards: no gaps heard (no
  take recorded under load on this build).
- Open: with 8 sample tracks and 8 303s on every step the Seed stopped
  reporting and taking commands (audio and USB audio went on) until reset. Not
  known whether any build does that. Nothing resets it by itself.
- Open: one 303 with 8 busy sample tracks takes single blocks to 124 % CPU.
- The half-hour seam of a take is sample-continuous (seen in that take).
- CPU, measured on this build: 38 to 44 % stopped, 71 to 76 % with 8 sample
  tracks on every step plus one 303 (peaks to 124 %). Cause and cure: v13.22.

## v13.21: long plain samples in spare memory (built 2026-11-24, on the rig with the USB audio change, long samples not tried there)

`daisy/release/ApcBox.bin` is v13.21. Control protocol 10, report 18, SDRAM
95.9%, all as v13.20. Desktop tests and the bridge against the firmware
simulator only.

- A plain sample longer than a slot (16 s) claims the length of its file in
  spare memory and plays whole: loaded from the card, with a kit, with a set
  (after the switch if there is no room before), or sent from the T95's
  library (the staging pool limits that to 98 s). No piece that long free: cut
  to the slot, as before.
- `FO ... capacity` tells the bridge the room there really is, so "(cut to
  fit)" only shows when it was cut.
- A take brought back by undo after a long sample was browsed over it goes back
  into the slot; the sample's memory is free again.
- Not looked at: how long a 60 s sample takes to load from the real card, and
  what one long sample does to the room for loops in practice (one of 98 s
  takes over a third of the 262 s).
- Still not built: a fixed claim for a guitar effects module; moving claims
  together.

## v13.20: spare memory as one region with claims by size (on the rig, flashed 2026-11-24, no take tried there)

`daisy/release/ApcBox.bin` is v13.20. Control protocol 10 as before, report 18
(`SM`). SDRAM 95.9% as before. Desktop tests and the page in a browser against
the firmware simulator. On the rig after the flash: `/health` says firmware
13.20, control 10, 0 bad lines, both controllers back; the event stream has
`SM 12582912 12582912 12582912 0 786432 0` (all 262 s free) and the looper
settings as before. No take recorded there, the page not looked at on a screen.

- The 8 fixed blocks of 32 s are a table of up to 24 claims over the same 262 s,
  each in one piece. A take of chosen bars claims what it needs plus a
  sixteenth; a kept take gives back what it didn't use; a take until stopped is
  capped at 32 s (Rene's call) and trimmed once kept; a set's long loop claims
  the length of its file.
- Two sets with long loops and no room for both: the new set's track loads
  after the switch, late but whole (Rene's call), silent until then. How long
  that is on the real card is not known.
- Fixed on the way: a staged long loop whose file could not be read kept its
  memory for good; a built-in sound standing in for a missing loop was flagged
  as a loop.
- The page: looper view says "no room" with the numbers and dims REC LOOP, and
  "room for N" bars during a take until stopped; System shows loop memory.
- Not built: long plain samples in spare memory (still cut to a slot), a fixed
  claim for a guitar effects module, moving claims together (no compaction: a
  fixed user in the middle can leave much free and no piece long enough).
- To try on the rig: 8 bar takes at 100 BPM until "no room" (about 13 should
  fit where 8 did), a take until stopped, switching between two sets that both
  have a long loop on the same track, the page's numbers against what happens.

## v13.19: looper and quantise settings survive a power cycle (on the rig, flashed 2026-11-24)

`daisy/release/ApcBox.bin`. Control protocol 10 and report 17 as before; SDRAM
95.9% as before (`arm-none-eabi-size -A`).

- Take length, count-in, the four quantise settings (take start, pattern,
  mute/solo, FX) and the input level are written to `apcbox/looper.txt` on the
  Daisy's card 1.5 s after the last change, whoever changed them (a page or the
  APC40), and read at power-up and when a card goes in. No card: as before.
- The input monitor is left out on purpose: it starts off, so the box never
  comes up feeding its input to the speakers. Say if it should be kept too.
- Desktop test (`daisy/sets_test.cpp`), and on the rig once: take length 4 and
  pattern quantise "step" set through the bridge, the Seed restarted by
  flashing it again, both were back (`LP` line); then put back to what they
  were. Not tried: whether the write is heard while playing (it waits for a
  take, a set operation or a load to finish, but it is a card write, like the
  button map's), a change made from the APC40, a real power cycle.

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
- BUG (Rene, 2026-11-24, on the rig): after an overdub on a long loop, pressing undo
  seems to say "take undone" and changes nothing. No undo there is as documented; the
  message is wrong. Cause: `box.cpp` offered undo after every overdub and said "take
  undone" although nothing was kept for a long loop. Fixed 2026-11-24 in v13.18
  (desktop test; flashed, the box says `"firmware": "13.18"`; the overdub itself NOT yet tried on the rig): the overdub ends with
  "overdub N done (no undo)", undo is not offered, and pressing it says
  "overdub on long loop: no undo".
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

Known limits (looper and quantise settings not being kept: fixed in v13.19). A loop that started on a beat goes back to the bar line when its set is
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
- Mac build of the shared code (`daisy/mac/build.sh`): builds, 2026-11-24, with the
  v13.18 sources. Not started or played since v13.9.
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
  to Settings (Remote, Devices); the TV remote walks the Settings page. Built 2026-11-24, untried on the rig: Bluetooth pairing and
  Bluetooth MIDI, the PIN with the TV remote (a number pad). Sound cards and the box's SD card under Devices: built 2026-11-24 (read-only; no eject, and not the Daisy's own card).
  Rene, 2026-11-23: the join works on the rig (wrong password comes back),
  but the error and info banners are ugly and stay too long. UI pass later.
- **Jam recorder** (`/jam`, `t95/ui/jam.html`, `t95/bridge/rackbridge/jam.py`; Rene, 2026-11-24): records
  the Daisy's master on the T95. Slice 1 built 2026-11-24: `arecord` as its own process writes
  WAV onto the eMMC (the bridge's folder, `jams/<take>/`, a new file every half hour), next to
  the TV sound and listening on the shared capture device; REC / STOP with time and level on the
  page and the remote, takes played and downloaded as one WAV, rename, delete, "to library"
  (links into the sample library, folder `jams`); stops by itself under 1 GB free; a lamp on the
  start screen while a take runs. Seen on the T95 the same day: a 25 s take with the Daisy
  playing (peak -0.4 dB, 0 dropouts reported), the page in a browser on the Mac. NOT tried:
  a long take (over the half-hour seam), the remote, the TV, a phone, a power cut, the Daisy
  pulled mid-take, the box nearly full; those only against a made-up `arecord` in the tests.
  A restart of the bridge ends a running take (it is kept).
  Slice 2 built 2026-11-24: TO CARD per take and EJECT CARD on the jam page. The settings helper
  (root) copies the take as one WAV into `jams/` on the card: it makes the card writable for the
  copy only, read-only again after it, only a FAT / exFAT card in the SD slot, only from the takes
  folder (no links followed), no PIN. Eject unmounts through systemd. Seen on the T95: exFAT
  writes in the slot (256 MB at 16.6 MB/s, read back the same), the 25 s take copied and the same
  byte for byte, read-only after it, eject and the card mounted again. On the rig an `umount` by
  the helper turned out to unmount the card for the helper alone (its own mount namespace); it
  goes through systemd now. NOT tried: a long take (minutes of copying), pulling the card
  mid-copy, a full card, FAT32, the card physically taken out and put back after EJECT.
  Open: after EJECT, Settings > Devices says "in the slot, not readable" (true, but it reads as a
  fault); no EJECT on the Settings page itself; nothing lists or deletes what is on the card.
  Later: a job for
  an APC40 button, takes over 3.5 minutes get no waveform in the Samples view (40 MB limit),
  set and kit backups and the library on the card.
- **Cubase sync**: a VST3 plugin in Cubase that reads its transport and
  shares it on Link (Cubase can't follow clock or speak Link). Pinned.
- **Numark DJ**: a Mixtrack Pro FX, on an engine of our own (`decks/README.md`: what is built,
  measured and next). Not Mixxx: its window cannot be one of our pages.
- Chaining: two modules at once (hub + ApcBox, clock + anything) with
  routes between them. The data model allows it; one is active today.
- More Seeds: three exist; other bodies doing other things later.

## ApcBox views and firmware features

- Song / chain mode (patterns in a row). Slice (chop a sample over steps).
- Keys view: on-screen keyboard and pads, scale lock.
- Per-step velocity and probability (a step is off / on / accent today).
- The Daisy's own SD card browsed from the T95; sets copied to and from the
  T95 (backup); a whole kit sent from the T95 library in one go.
- A kit loaded over a sample sent from the box: undoing the kit can't bring that sample back
  (it reloads by path, and a sent sample has none). Read in `daisy/kits.h`, not tried.
- Kit names from a page are 16 characters, lower case (the command line to
  the Daisy is 31 characters).
- Looper: more than one undo; one-shot on
  an APC40 button; learn for keyboard knobs.

## MIDI and clock

- DIN MIDI when the parts arrive (UART4 on D1/D2; D13/D14 are the screen's).
  USB-to-DIN cables as ports meanwhile.
- Bluetooth MIDI (2026-11-24): works on the rig with one device, an iPad running a
  synth app that advertises Bluetooth MIDI. Pair in Settings > Bluetooth; the bridge
  makes a controller port of every paired device that offers the MIDI service
  (`rackbridge/ble.py`, its own ATT channel, `--ble`) and reaches it again by itself
  (seen: 8 s after a restart). Notes sent from the box played on the iPad, and the iPad's MIDI
  reached ApcBox (Rene, same day). The T95's
  radio needed three ways round (top of `rackbridge/ble.py`, `t95/README.md`).
  Not tried: PAIR from the Settings page with the
  new sequence (it was done by hand in that order), a Bluetooth MIDI keyboard or
  adapter, the delay, what the Wi-Fi does meanwhile, a wait shorter than 3 s before
  encrypting. A device that wants a code typed is not handled. While a paired device
  is away the bridge scans 6 s in every 20 for it.
- The box as a Bluetooth MIDI device that an iPad or Mac finds (the other way round):
  built 2026-11-24, NOT tried with a device. Settings > Bluetooth > VISIBLE (PIN): for
  3 minutes the box advertises the MIDI service under its name; whoever connects and
  turns notifications on is one more controller port ("Bluetooth guest"). No pairing
  that way, hence PIN and time limit. BlueZ serves the characteristic for the bridge
  (`Guest` in `rackbridge/ble.py`). Seen on the T95: BlueZ accepts the service
  (`ble_midi.box.state` is `ready` in `/health`), and VISIBLE without a PIN is turned
  away. Not seen: the radio advertising at all, an iPad listing or connecting, MIDI
  either way, what a second device does (one port for all of them today). Only made-up
  BlueZ in the tests.
- RTP-MIDI recovery journal (none today: a lost packet is a lost message).
- Link: start/stop sync and bar alignment when the T95 leads.
- MIDI clock following: tempo ramps lag by the fit window (4 beats).
- Clock follower, tempo steps: fixed 2026-11-24 in the bridge. A step in the
  source's tempo was often read wrong for up to 4 beats (the new ticks bent the
  fitted line instead of replacing it: 125 to 110 BPM read as 113). Now the
  last half beat's own slope is compared with the line's. In a simulation with
  made-up ticks (`FollowerSimTest`): wrong 17 to 73% of the time before, 0 to
  4% after. Not tried with a real sequencer.
- Clock follower, a run of late ticks: fixed 2026-11-24 in the bridge, in the
  simulation only. 8 ticks running 15 ms late and then back on time threw the
  reading off by up to 12 BPM (30 ms late: 25) until the window had refilled:
  the late ticks were taken for a new tempo and then fitted together with the
  ones after them. Now the window from before is kept for 2 beats and taken
  back when the ticks are on its line again: 0.1 BPM off at worst. A run of 16
  or more still reads up to 1.8 BPM off for a few ticks (was 5.5): it gets a
  fit of its own from half a beat. Tempo steps read as before (14 of 392 wrong
  in the 120 to 126 case, before and after). Not tried with a real sequencer.

## T95 and pages

- Samples from the box's SD card (2026-11-24): the 64 GB exFAT card Rene put in
  is read (19800 audio files). 11076 of them are AIFF in Ableton's own
  compression (AIFC, type `able`: the samples of Live's packs), which only Live
  decodes; tapping one gave a three-line ffmpeg error on the page. Now those
  are not listed (the page says how many it left out), not imported, and any
  other file ffmpeg can't read gets one plain line. Measured on that card with
  the new code, read-only: all 11076 found, a pass over the whole card takes
  23 s cold, the fullest folder has 347 such files. Still listed and
  unreadable there: 102 AIFC files whose header ffmpeg rejects, 4 with no
  compression name, 2 that are not AIFF inside. Settings > Devices still counts
  all 19800 as samples. Not built: turning Live's files into something
  readable (it would take Live itself to export them).

- LISTEN: measure the real delay; a lighter format than raw PCM if Wi-Fi
  needs it.
- T95-to-Daisy audio over USB (the Daisy as the T95's sound card).
- Countdown on the start screen: 5 s today, his call to keep it.
