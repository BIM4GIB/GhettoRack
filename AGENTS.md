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
touch), `hostcmd_test.cpp` (serial commands vs echo), `liverec_test.cpp`,
`acid_engine_test.cpp`, `host_test.cpp`, `t95/bridge/test_bridge.py`; build lines
are at the top of each file or in `build_host.sh`. Run them after engine/box changes.

## Rules that matter

- **Timing lives on the Daisy.** Sequencer, voices, FX and mixer run on the
  Daisy's audio clock. The T95 handles controls and networking; browser or Linux
  scheduling must not become the per-step timing source. Planned external MIDI /
  network clock leader and follower modes must explicitly translate and track
  tempo/phase on the Daisy. Do not confuse musical sync with the audio sample clock.
- **Daisy works standalone.** With nothing but the APC40 on its USB host port.
- **Keep the protocol in step.** Change a report line: bump `kReportVersion` in
  `daisy/box.cpp` and `REPORT_VERSION` in `daisy/ui/apcbox-ui.html`, and update
  `protocol/serial.md`, in the same commit.
- **Don't state unverified hardware facts as certain.** Several things are only
  tested in desktop tests and the Mac build (USB audio on iPad, live sampling, SD
  hot-swap, sync set switching on the real card). Say so.
- `box.cpp` is shared by the firmware and the Mac build: no Daisy-only calls
  there; Daisy specifics go in `main.cpp`, `tft.h`, `usbdev.cpp`.
- SDRAM is reported ~93% used (slots + staging pool). Verify against the current
  build with `arm-none-eabi-size -A` before adding buffers. 64 MB total SDRAM is
  not 64 MB available for incoming samples.
- The screen (SunFounder 3.5", ST7796U behind shift registers) needs 16-bit
  command words; `tft.h` defaults to that, encoder held at power-up switches.

## T95 integration rules

- Read [t95/README.md](t95/README.md) and [protocol/serial.md](protocol/serial.md)
  before changing the bridge or USB transport. Firmware v13.x and serial report
  version 7 are separate version numbers.
- Preserve direct Web Serial and the Mac engine UI. The existing HTTP monitor
  uses SSE at `/events`; reuse it for initial telemetry rather than introducing
  another transport without a reason. Mac-only `/ui` and `/tft.bmp` features
  must be capability-gated for a Daisy-backed bridge.
- Current Daisy CDC input supports only `ESC R` (state dump) and `ESC BOOT`
  (bootloader); v13.2 and older act on any bare `R`/`B` byte, so never open the
  port with echo on (plain `cat`/`stty`/`screen` on Linux).
  Do not send MIDI or sample payloads until a framed, versioned receive protocol
  exists. Payload bytes must not fall through to those legacy commands.
- One bridge owns the Daisy serial port. Identify the device by VID/PID and
  serial, not a hardcoded tty index. Raw MIDI card indices are also unstable.
  Separate device connection, telemetry freshness and browser connection status.
- Use bounded queues and explicit overflow handling. Slow browsers and sample
  transfers must not block MIDI forwarding or the Daisy audio callback.
  Apply real-time scheduling only to measured, bounded timing work, not the
  web server, filesystem operations or logging. Measure latency and jitter.
- Keep bootable recovery media and board-specific DTB/firmware changes intact.
  Re-identify disks before a write; device numbers in docs are observations,
  not portable install targets. Do not include destructive commands in general
  setup scripts without explicit target validation.
- Never commit Wi-Fi credentials, private keys, raw eMMC dumps or personal
  samples. Follow the existing release convention for firmware binaries;
  large OS images belong in release assets with checksums and build provenance.
- For documentation-only changes, check facts, paths and the diff. For protocol
  or bridge changes, test fragmentation, reconnects, multiple clients and queue
  pressure. Hardware claims require hardware evidence; desktop tests alone do
  not establish USB compatibility or gig readiness.

## Working with Rene

Terse and direct, no padding, no em-dashes. Agree on a plan before building
anything substantial. Give full paths. He likes visuals (diagrams, screenshots).
