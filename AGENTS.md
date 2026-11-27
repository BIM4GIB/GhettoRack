# GhettoRack: brief for agents

GhettoRack is Rene's modular standalone electronic instrument for home jams and
portable live use. Read this first, then the folder you're working in.

The Daisy Seed 3 running ApcBox is a core component, not an optional peripheral.
The planned startup selector chooses one module or compatible combinations,
with focused views. ApcBox, the Numark DJ mode, and network clock mode are parts
of the wider project; do not narrow the architecture to any one controller.

## What it is

- **Daisy Seed** (STM32H750, libDaisy) running **ApcBox**: a 16-track groovebox
  played from an **Akai APC40 mk1**. Sample tracks (SD card, built-in kits, live
  recorded loops) or 303 synth voices (Pocket 303, from the `uhntiss` repo),
  16-step sequencer, 64 patterns, mixer with per-track A/B crossfader, reverb and
  delay sends, master filter, 8 performance FX, sets saved to the card with their
  samples and switched in time on a bar line, 3.5" touch screen + encoder, OLED.
- **T95 box** (Allwinner H616, Armbian): the companion that will take the UI,
  controllers, files and networking off the Daisy. See `t95/README.md`.
- **uhntiss** (separate repo, `BIM4GIB/uhntiss`): Pocket 303 synth source
  (Max for Live, plug-ins, Daisy). `daisy/acid.h` is a float port of its voice.

## Layout

```
daisy/      ApcBox firmware (Makefile; kits.h: kits saved on the box; apcmap.h: the APC40 button map), desktop tests, mac/ (same engine on macOS
            with the APC40 on the Mac), ui/ (monitor page, Chrome Web Serial),
            release/ApcBox.bin (latest built firmware)
decks/      the deck engine for the Numark DJ module (deckd: two decks, mixer, the
            Numark Mixtrack Pro FX's map in numark.h; build.sh, engine_test.cpp;
            its README says what is built and measured). The bridge runs it
            (`t95/bridge/rackbridge/decks.py`). No page yet
t95/        companion box: findings, plan, bridge/ (Daisy link, controllers, a game controller,
            remote, router, network MIDI), ui/ (selector, MIDI hub, clock, the
            ApcBox page: edit apcbox.css/.js, run build.py; VIEWS.md lists every screen;
            small.css/.js: the frame every page shares at 480x320),
            kiosk/ (Chromium on HDMI), helper/ (the root helper behind the Settings
            page: Wi-Fi, name, Bluetooth, reboot; its README says what it may do)
pi4/        the same companion on a Raspberry Pi 4 with the 3.5" touch screen on its
            header: findings, screen/ (the screen's driver, calibrate.py, framerate.py,
            touchlag.py, shot.py: a screenshot of the kiosk's page)
protocol/   serial.md: what the Daisy's USB presents and the report lines
tools/      sdprep.py (fill an SD card), seed2-setup.sh (fresh Seed), check-public.sh,
            flash-seed.sh (flash one chosen Seed; macOS and Linux),
            boxcert.py (HTTPS without a browser warning: your own authority, a certificate per box),
            mock_oled.py, mock_tft.py, mock_atlas.py (draw the screen mockups in docs/DISPLAYS.md)
docs/       CANON.md (how a module and its screens are made: read it before touching a
            page), wiring-guide.html (every connection, diagrams), images/,
            DISPLAYS.md (a screen for the bare Daisy: options, mockups; nothing built),
            NOTEBOX.md (a found cabinet as the home-jam speaker: sizes, the plan; nothing built)
lib/        libDaisy v9.0.0 and DaisySP (+ LGPL) as pinned submodules
            (`daisy/reverb.h` is a repacked copy of DaisySP-LGPL's ReverbSc, LGPL too)
```

## Build

```
git submodule update --init --recursive
make -C lib/libDaisy && make -C lib/DaisySP && make -C lib/DaisySP/DaisySP-LGPL
make -C daisy                      # firmware -> daisy/build/ApcBox.bin
python3 daisy/check_itcm.py        # the fast-memory copy, checked on the desk; a build that fails it does not boot
daisy/mac/build.sh                 # macOS build of the same engine
tools/flash-seed.sh                # flash (pick the port)
```
Tests: `daisy/sets_test.cpp` (pattern copy/undo, sets save/load/switching,
touch, T95 controller routing, looper/quantise/keyboard controls), `hostcmd_test.cpp` (serial commands vs echo), `link_test.cpp` (resync keeps lines whole), `liverec_test.cpp`,
`acid_engine_test.cpp`, `host_test.cpp`, `decks/engine_test.cpp` (the deck engine and the Numark map; `decks/build.sh` builds and names it), `reverb_test.cpp` (`daisy/reverb.h` against the library's reverb, sample for sample), `t95/bridge/test_bridge.py` (its `DecksTest` needs `decks/build.sh` first; its end-to-end part
runs the firmware control code via `daisy/hostbuild/t95sim`, built by `build_host.sh`); build lines
are at the top of each file or in `build_host.sh`. Run them after engine/box changes.

## Rules that matter

- **A module, a page or a view follows `docs/CANON.md`.** What a module consists
  of, the four layouts (TV, phone upright, phone sideways, 480x320), the shared
  frame and parts for the small one (`t95/ui/small.css`, `small.js`), how a
  touch, a confirmation, a PIN and a message behave, what each colour means,
  what the 3.5" screen can draw, and when a page change counts as done. Change
  the canon first if a rule is wrong; do not work around it in one page.
- **Timing lives on the Daisy.** Sequencer, voices, FX and mixer run on the
  Daisy's audio clock. The T95 handles controls and networking; browser or Linux
  scheduling must not become the per-step timing source. Planned external MIDI /
  network clock leader and follower modes must explicitly translate and track
  tempo/phase on the Daisy. Do not confuse musical sync with the audio sample clock.
  The decks (`decks/`) are a second sound engine, on the box, on the Numark's card
  and its clock (44.1 kHz): the same rule inside it (one audio thread keeps its time;
  the bridge and pages only ask), and no shared sample clock with the Daisy.
- **Daisy works standalone.** With nothing but the APC40 on its USB host port.
- **APC40 input and lights go through the button map** (`daisy/apcmap.h`).
  A new button function goes into `OnButton` under the button that has it in
  the standard layout, or as a new "virtual" job there; give it a light in
  `UpdateLeds` / `VirtualLeds`, and a line in `apcMap()` / `MAP_VIRT` in
  `t95/ui/apcbox.js` so the map page shows it.
- **Keep the protocol in step.** Change a report line: bump `kReportVersion` in
  `daisy/box.cpp` and `REPORT_VERSION` in `daisy/ui/apcbox-ui.html`, and update
  `protocol/serial.md`, in the same commit.
- **Don't state unverified hardware facts as certain.** Several things are only
  tested in desktop tests and the Mac build (USB audio on iPad, live sampling, SD
  hot-swap, sync set switching on the real card). Say so.
- `box.cpp` is shared by the firmware and the Mac build: no Daisy-only calls
  there; Daisy specifics go in `main.cpp`, `tft.h`, `usbdev.cpp`.
- From v13.36 two watchdogs reset a Seed whose main loop stops (`daisy/main.cpp`, "the
  watchdogs"): 8 s without a pass, 33 s inside a card call. A new card call in `main.cpp`
  goes inside a `CardCall`; main-loop work that can take seconds without touching the card
  needs a look at those limits first.
- SDRAM is ~96% used from v13.16 (16 slots of 16 s, 262 s of spare memory for
  long loops, staging pool, one slot of looper undo). From v13.20 the spare
  memory is one region handed out as claims by size (`Spare...` in
  `daisy/box.cpp`), not 8 fixed blocks. Verify against the current
  build with `arm-none-eabi-size -A` before adding buffers. 64 MB total SDRAM is
  not 64 MB available for incoming samples. From v13.21 a sound longer than a
  slot (a loop or a plain sample) loads into spare memory whole when a piece
  that long is free, and is cut to the slot when not.
- The screen (SunFounder 3.5", ST7796U behind shift registers) needs 16-bit
  command words; `tft.h` defaults to that, encoder held at power-up switches.

## T95 integration rules

- Read [t95/README.md](t95/README.md) and [protocol/serial.md](protocol/serial.md)
  before changing the bridge or USB transport. Firmware v13.x and serial report
  version 20 are separate version numbers.
- Preserve direct Web Serial and the Mac engine UI. The existing HTTP monitor
  uses SSE at `/events`; reuse it for initial telemetry rather than introducing
  another transport without a reason. Mac-only `/ui` and `/tft.bmp` features
  must be capability-gated for a Daisy-backed bridge.
- Daisy CDC input: `ESC R` (state dump), `ESC BOOT` (bootloader) and, from
  v13.5, the T95 control frames `ESC K/M/N/P`, from v13.7 `ESC U` (UI
  actions), from v13.8 `ESC F` (sample transfer; see `protocol/serial.md`
  and `daisy/xfer.h`), from v13.9 the page actions in `ESC U` (control
  protocol 4: mute, solo, knobs, steps...), from v13.10 set actions (protocol 5), from v13.11 `ESC T` (protocol 6: the T95's clock; the Daisy steers
  its sequencer onto it, the audio clock stays the timing source), from v13.12 the looper, quantise and roll actions in `ESC U` (protocol 7). Send control frames only after the Daisy reports a
  `P` line, `ESC U` only at version 2+, `ESC F` only at 3+, page actions at 4+, sets at 5+, `ESC T` at 6+, looper/quantise/roll actions at 7+, `lrec redo` and `quant fx` at 8+, `kit save/over/rename/delete` at 9+, `map` at 10+, `file` and `folder` (the card's samples from a page) at 11+, `set demo` at 13+, `files` and `file at` (a folder's file names, answered in `BL` lines) and `restart` (the Daisy resets itself) at 14+, `set over` at 12+ (from v13.33 `set save` never replaces another set; whoever sends `set over` has asked first). v13.2 and older act
  on any bare `R`/`B` byte, so never open the port with echo on (plain
  `cat`/`stty`/`screen` on Linux). Sample data goes only as `ESC F` frames
  (never ESC M), sent at low priority behind MIDI by the bridge.
- One bridge owns the Daisy serial port. Identify the device by VID/PID and
  serial, not a hardcoded tty index. Raw MIDI card indices are also unstable.
  Separate device connection, telemetry freshness and browser connection status.
- Use bounded queues and explicit overflow handling. Slow browsers and sample
  transfers must not block MIDI forwarding or the Daisy audio callback.
  Apply real-time scheduling only to measured, bounded timing work, not the
  web server, filesystem operations or logging. Measure latency and jitter.
- Anything needing root on the T95 goes through `t95/helper/ghettorack_helper.py`
  (a fixed list of commands, PIN for whatever changes the box), never by giving
  the bridge rights. A network change must be a trial that goes back by itself.
- Keep bootable recovery media and board-specific DTB/firmware changes intact.
  Re-identify disks before a write; device numbers in docs are observations,
  not portable install targets. Do not include destructive commands in general
  setup scripts without explicit target validation.
- **The repo is public.** Run `tools/check-public.sh` before every push (keys,
  credentials, IP/MAC addresses, device serials, samples, images). Never commit
  Wi-Fi credentials, private keys, raw eMMC dumps or personal
  samples. Follow the existing release convention for firmware binaries;
  large OS images belong in release assets with checksums and build provenance.
- For documentation-only changes, check facts, paths and the diff. For protocol
  or bridge changes, test fragmentation, reconnects, multiple clients and queue
  pressure. Hardware claims require hardware evidence; desktop tests alone do
  not establish USB compatibility or gig readiness.

## The rig

When the session can reach Rene's network (Claude Code on his Mac): the T95 is
`ssh ghettobox`, its clone is `~/GhettoRack`. Look, measure and install there
yourself instead of handing him commands to paste. The box's address, Wi-Fi
names and device serials stay out of the repo. `docs/HANDOVER.md` has the
state of the rig as of the last session, if it is still there.

## Working with Rene

Terse and direct, no padding, no em-dashes. Agree on a plan before building
anything substantial. Give full paths. He likes visuals (diagrams, screenshots).
