# ApcBox serial protocol (report version 7)

The Daisy's USB-C is a composite USB device, full speed (12 Mbit/s):

| Interfaces | Class | Endpoints | Host sees |
|---|---|---|---|
| 0, 1 | CDC-ACM serial | 0x82 interrupt, 0x01 out / 0x81 in bulk 64 | `/dev/ttyACM*` (Linux), `/dev/cu.usbmodem*` (macOS) |
| 2, 3 | USB Audio Class 1 | 0x83 isochronous async IN, 47/48/49 frames per 1 ms | input device "ApcBox": stereo, 48 kHz, 16-bit, the master output |

VID 0x1209, PID 0x0001 (pid.codes test PID), product "ApcBox", serial = STM32
unique ID in hex. The baud rate setting is ignored. Source: `daisy/usbdev.cpp`.

There is no USB-MIDI interface and no audio OUT (host to Daisy) yet.

## Host to Daisy

| Byte | Does |
|---|---|
| `R` | Send a full state dump (every line below, now) |
| `B` | Reboot into the Daisy bootloader (used by `tools/flash-seed.sh`) |

Everything else is ignored. **There is no control path yet.** Planned next:
a framed MIDI-in command so the T95 bridge can forward controller MIDI into the
same handler the USB-host APC40 uses (`BoxMidi` in `daisy/box.cpp`), and LED
output coming back the same way.

## Daisy to host

Newline-terminated ASCII lines, fields separated by single spaces, first
character = line type. Lines are sent when their content changes, except S
(every UI tick) and V (once a second). Source: `daisy/box.cpp`, search for `Out("`.

| Line | Fields |
|---|---|
| `S` | Status, 56 fields, see below |
| `G pat digits` | Steps of the current pattern: 256 digits, track-major (16 tracks x 16 steps), 0 off, 1 on, 2 accent |
| `T t v0..v12` | Track `t` knob values 0-127 (13 values, the last is crossfader side: 0 A+B, 1 A, 2 B) |
| `N t name` | Track `t` sound name, or `[303]`, `[loop n bars]`, `[kit sound]` |
| `M v0..v9` | Global values 0-127 (index 9 = master filter) |
| `B folder nfolders file nfiles` | Sample browser position |
| `K k0 k1 k2 k3` | Kit page |
| `W want sent` | APC40 LEDs wanted / confirmed (hex strings) |
| `E type ch d0 d1` | Raw MIDI received from the APC40 |
| `Q pat t hex` | 303 notes of track `t`: 16 bytes hex, bit 7 = slide |
| `A t v0..v7` | 303 knobs of track `t`, 0-127 |
| `L text` | Log text |
| `V n` | Report version (7). Bump it in `box.cpp` and `ui/apcbox-ui.html` together when a line changes |

### S fields

| # | Meaning | # | Meaning |
|---|---|---|---|
| 0 | playing | 1 | current step |
| 2 | current pattern | 3 | queued pattern (-1 none) |
| 4 | step record armed | 5 | tempo x 10 |
| 6 | metronome | 7 | selected track |
| 8 | knob page | 9 | SHIFT held |
| 10 | CLIP/TRACK held | 11 | CPU average x 1000 |
| 12 | CPU max x 1000 | 13 | SD ok |
| 14 | APC40 up | 15 | APC40 present |
| 16 | perf FX on (mask) | 17 | perf FX latched (mask) |
| 18 | roll (mask) | 19 | mute (mask) |
| 20 | solo (mask) | 21 | loading (mask) |
| 22 | tracks with steps (mask) | 23 | limiter count |
| 24 | NaN count | 25 | dropped log bytes |
| 26-33 | trigger counts, tracks 1-8 | 34 | shown bank |
| 35 | banks with content (mask) | 36 | full reset being held |
| 37 | 303 tracks (mask) | 38 | track page (0 = 1-8, 1 = 9-16) |
| 39-46 | trigger counts, tracks 9-16 | 47 | live sampling state |
| 48 | live sampling track | 49 | live sampling bars |
| 50 | input peak 0-100 | 51 | input monitor |
| 52 | copy source (-1 none) | 53 | open set (-1 none) |
| 54 | unsaved changes | 55 | set busy (1 saving, 2 loading) |
