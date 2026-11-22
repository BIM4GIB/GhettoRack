# ApcBox serial protocol (report version 8)

The Daisy's USB-C is a composite USB device, full speed (12 Mbit/s):

| Interfaces | Class | Endpoints | Host sees |
|---|---|---|---|
| 0, 1 | CDC-ACM serial | 0x82 interrupt, 0x01 out / 0x81 in bulk 64 | `/dev/ttyACM*` (Linux), `/dev/cu.usbmodem*` (macOS) |
| 2, 3 | USB Audio Class 1 | 0x83 isochronous async IN, 47/48/49 frames per 1 ms | input device "ApcBox": stereo, 48 kHz, 16-bit, the master output |

VID 0x1209, PID 0x0001 (pid.codes test PID), product "ApcBox", serial = STM32
unique ID in hex. The baud rate setting is ignored. Source: `daisy/usbdev.cpp`.

There is no USB-MIDI interface and no audio OUT (host to Daisy) yet.

## Host to Daisy

From firmware v13.3, every command starts with ESC (0x1B):

| Bytes | Does |
|---|---|
| `ESC R` (1B 52) | Send a full state dump (every line below, now) |
| `ESC BOOT` (1B 42 4F 4F 54) | Reboot into the Daisy bootloader (used by `tools/flash-seed.sh`) |

From v13.5, the T95 control protocol (version 1). Send these only after the
Daisy has sent a `P` line: older firmware would act on the `R`/`B` letters
inside them.

| Frame | Does |
|---|---|
| `ESC K` | Keepalive. No frame for 2 s: the Daisy drops every T95 controller and releases what they held. The bridge sends one every 500 ms |
| `ESC M` *p* *hex* `\n` | One complete MIDI message from T95 port *p* (`1`-`8`), as hex digits (spaces ignored, at most 128 bytes). Channel messages or one whole sysex; no running status, no realtime bytes |
| `ESC N` *p* *name* `\n` | Port *p* now has a controller called *name* (printable ASCII, at most 32). `-` = gone. Re-announcing the same name is harmless |
| `ESC P` *id* `\n` | Ping. The Daisy answers `Y` *id* from its main loop (*id*: printable, at most 32) |

Example: `ESC M2 903c64 \n` = note on, C4, velocity 100, from port 2.
The Daisy asks each new port for its identity. An Akai APC40 reply (47 73)
makes it an APC40: it drives the box like one on the Daisy's own USB-A,
which always wins. Anything else is a keyboard: notes play the selected track.

Bare bytes are ignored, and so are unknown letters after ESC. The Daisy never sends ESC, so a host tty that
echoes the Daisy's output back (the Linux default until `-echo` is set) can't
trigger a command. Before v13.3 any single `R` or `B` byte did, and report
lines are full of both: an echoing `stty`/`cat` on the T95 sent a v13.2 Daisy
into its bootloader. Hosts should still open the port raw with echo off.

Older firmware ignores the ESC and acts on the `R` or `B` after it, so hosts
can send the ESC forms to any version. Parser: `daisy/hostcmd.h`, test
`daisy/hostcmd_test.cpp`.

Inside a framed command an ESC starts over and the payload is text, so a
corrupted frame can't turn into another command. Parser: `daisy/hostcmd.h`;
routing: `daisy/ports.h`; tests: `daisy/hostcmd_test.cpp`, `daisy/sets_test.cpp`.

## Daisy to host

Newline-terminated ASCII lines, fields separated by single spaces, first
character = line type. Lines are sent when their content changes, except S
(every UI tick) and V (once a second). A parser must accept unknown line
types and ignore them. Source: `daisy/box.cpp`, search for `Out("`.

| Line | Fields |
|---|---|
| `S` | Status, 56 fields, see below |
| `G pat digits` | Steps of the current pattern: 256 digits, track-major (16 tracks x 16 steps), 0 off, 1 on, 2 accent |
| `T t v0..v12` | Track `t` knob values 0-127 (13 values, the last is crossfader side: 0 A+B, 1 A, 2 B) |
| `N t name` | Track `t` sound name, or `[303]`, `[loop n bars]`, `[kit sound]` |
| `M v0..v9` | Global values 0-127 (index 9 = master filter) |
| `B folder nfolders file nfiles` | Sample browser position |
| `BF name` | Sample browser folder name |
| `BN name` | Sample browser file name (`-` when none) |
| `K k0 k1 k2 k3` | Kit page |
| `KN i k name` | Name of kit `k` in slot `i` (0-4) of the shown kit page (`-` when none) |
| `D hex` | OLED picture: 1024 bytes (128x64, 1 bit per pixel, SSD1306 page order) as 2048 hex digits, at most every 200 ms when it changes. The monitor page draws it; it is not the TFT picture |
| `W want sent` | APC40 LEDs wanted / confirmed (hex strings) |
| `E type ch d0 d1` | Raw MIDI received from the APC40 |
| `Q pat t hex` | 303 notes of track `t`: 16 bytes hex, bit 7 = slide |
| `A t v0..v7` | 303 knobs of track `t`, 0-127 |
| `L text` | Log text |
| `X p hex` | MIDI for T95 controller *p*: APC40 mode switch (sysex) and LED messages, up to 16 per line. For the bridge, which doesn't pass X to browsers |
| `I p hex` | MIDI the Daisy got from a T95 keyboard on port *p* (for the page log) |
| `P ver att apc keys active` | T95 control protocol version (1), then hex port masks (bit 0 = port 1): attached, identified as APC40, keyboards; then who drives the box: -1 none, 0 the Daisy's USB-A, 1-8 a T95 port. On change and every second |
| `Y id` | Answer to `ESC P id` |
| `V n` | Report version (8). Bump it in `box.cpp` and `ui/apcbox-ui.html` together when a line changes |

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
