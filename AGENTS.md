# GhettoRack: brief for agents

GhettoRack is Rene's DIY groovebox rig. Read this first, then the folder you're
working in.

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
daisy/      ApcBox firmware (Makefile), desktop tests, mac/ (same engine on macOS
            with the APC40 on the Mac), ui/ (monitor page, Chrome Web Serial),
            release/ApcBox.bin (latest built firmware)
t95/        companion box: hardware findings, plan, (later) bridge code
protocol/   serial.md: what the Daisy's USB presents and the report lines
tools/      sdprep.py (fill an SD card), seed2-setup.sh (fresh Seed),
            flash-seed.sh (flash one chosen Seed; macOS and Linux)
docs/       wiring-guide.html (every connection, diagrams), images/
lib/        libDaisy v9.0.0 and DaisySP (+ LGPL) as pinned submodules
```

## Build

```
git submodule update --init --recursive
make -C lib/libDaisy && make -C lib/DaisySP && make -C lib/DaisySP/DaisySP-LGPL
make -C daisy                      # firmware -> daisy/build/ApcBox.bin
daisy/mac/build.sh                 # macOS build of the same engine
tools/flash-seed.sh                # flash (pick the port)
```
Tests: `daisy/sets_test.cpp` (pattern copy/undo, sets save/load/switching,
touch), `liverec_test.cpp`, `acid_engine_test.cpp`, `host_test.cpp`; build lines
are at the top of each file or in `build_host.sh`. Run them after engine/box changes.

## Rules that matter

- **Timing lives on the Daisy.** Sequencer, voices, FX and mixer run on the
  Daisy's audio clock. Other devices send controls, never the beat.
- **Daisy works standalone.** With nothing but the APC40 on its USB host port.
- **Keep the protocol in step.** Change a report line: bump `kReportVersion` in
  `daisy/box.cpp` and `REPORT_VERSION` in `daisy/ui/apcbox-ui.html`, and update
  `protocol/serial.md`, in the same commit.
- **Don't state unverified hardware facts as certain.** Several things are only
  tested in desktop tests and the Mac build (USB audio on iPad, live sampling, SD
  hot-swap, sync set switching on the real card). Say so.
- `box.cpp` is shared by the firmware and the Mac build: no Daisy-only calls
  there; Daisy specifics go in `main.cpp`, `tft.h`, `usbdev.cpp`.
- SDRAM is ~93% used (slots + staging pool). Check `arm-none-eabi-size -A` before
  adding big buffers.
- The screen (SunFounder 3.5", ST7796U behind shift registers) needs 16-bit
  command words; `tft.h` defaults to that, encoder held at power-up switches.

## Working with Rene

Terse and direct, no padding, no em-dashes. Agree on a plan before building
anything substantial. Give full paths. He likes visuals (diagrams, screenshots).
