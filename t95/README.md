# T95 companion box

An old T95 Max Android TV box, rooted and running Armbian. It takes the
screen, controllers, files and networking off the Daisy, so the Daisy only
does sound. Nothing in this folder runs yet: this is the plan and what's
known about the hardware.

## Hardware (found so far)

| | |
|---|---|
| Board | `H616-T95MAX-V4.0` (2019-12-17) |
| SoC | Allwinner H616, 4 x Cortex-A53, ~3.9 GB RAM |
| OS | Armbian, kernel `6.18.63-current-sunxi64`, PREEMPT |
| USB | 2 sockets (USB0, USB1). The H616's other two controllers aren't routed anywhere reachable |
| Debug UART | pads GND / TX / RX on the top side (used for rooting) |
| Audio | card 0 H616 analogue codec, card 1 SPDIF, card 2 HDMI |
| IR receiver | works: `ir-keytable`, remote uses extended NEC, address `0x2fe` |
| MIDI | raw MIDI works (`CONFIG_SND_RAWMIDI=y`, e.g. `/dev/snd/midiC3D0` for an LPK25). **No ALSA sequencer** (`CONFIG_SND_SEQUENCER` not set): no `aconnect`/`aseqdump` until the kernel is rebuilt with `SND_SEQUENCER`, `SND_SEQ_DUMMY`, `SND_SEQ_MIDI`, `SND_SEQ_VIRMIDI` as modules |
| Power | DC5V jack. Use a 5 V 3 A supply once the Daisy and a hub hang off it |

## USB plan

- **USB1:** the Daisy's USB-C (USB-C to USB-A cable, or soldered inside later).
  The Daisy gets a controller to itself.
- **USB0:** a powered USB 2.0 hub for the APC40, MPK249, LPK25, keyboard receiver.
- Never connect the Daisy's own USB-A host socket (pins 36/37) to the box with an
  A-to-A cable: two hosts, two 5 V supplies.

## Software plan (agreed order)

1. **Bridge daemon** (real-time priority, own CPU core):
   - opens each controller's raw MIDI port and forwards the bytes to the Daisy over
     its CDC serial (needs the planned MIDI-in command, see `protocol/serial.md`);
     LED messages come back the same way
   - reads the Daisy's report lines and serves the UI (`daisy/ui/`) over WebSocket:
     Chromium kiosk on HDMI, plus any browser on Wi-Fi (iPad)
   - can flash the Daisy: `tools/flash-seed.sh` works on Linux (`dfu-util`)
2. **IR remote**: profiles per view (STEPS, MIXER, PATTERNS, TRACK, SYSTEM, sets,
   box), tap / hold / repeat, learn mode, saved as a file on the box.
3. **Sample transfer** box to Daisy SDRAM (64 MB limit, ~0.8-1 MB/s on one lane).
4. Later: USB audio OUT (box synths into the Daisy), a second USB lane on the
   Daisy's pins 36/37 in device mode, DIN MIDI clock out from a Daisy UART,
   jam mode (device ownership, clock in/out), latency loopback test.

## Rules

- Timing stays on the Daisy (sequencer, voices, FX, mixer run on its audio clock).
  The box sends controls, never the beat.
- The Daisy must keep working standalone (APC40 on its own USB host) without the box.
- Listen through the Daisy's output, not the box's HDMI (adds tens of ms).
