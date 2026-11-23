# ApcBox serial protocol (report version 14)

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

From v13.5, the T95 control protocol: version 1 (v13.5, v13.6) has K, M, N
and P; version 2 (v13.7) adds U; version 3 (v13.8) adds F; version 4 (v13.9) adds the U actions
for the T95 pages; version 5 (v13.10) adds the set actions; version 6 (v13.11) adds T; version 7 (v13.12) adds the looper, quantise and roll actions; version 8 (v13.13) adds `lrec redo` and `quant fx`. Send these only after the Daisy has sent a
`P` line, and U only when its version is 2 or more: older firmware would act
on the `R`/`B` letters inside them.

| Frame | Does |
|---|---|
| `ESC K` | Keepalive. No frame for 2 s: the Daisy drops every T95 controller and releases what they held. The bridge sends one every 500 ms |
| `ESC M` *p* *hex* `\n` | One complete MIDI message from T95 port *p* (`1`-`8`), as hex digits (spaces ignored, at most 128 bytes). Channel messages or one whole sysex; no running status, no realtime bytes |
| `ESC N` *p* *name* `\n` | Port *p* now has a controller called *name* (printable ASCII, at most 32). `-` = gone. Re-announcing the same name is harmless |
| `ESC P` *id* `\n` | Ping. The Daisy answers `Y` *id* from its main loop (*id*: printable, at most 32) |
| `ESC U` *action* `\n` | UI action, control protocol 2 (v13.7): `turn N` (-16..16, the encoder), `push`, `back`, `play`, `stop`, `track N` (1-16). Unknown actions are logged and ignored |
| `ESC U` *action* `\n` | Control protocol 4 (v13.9), for the T95 pages; none of it needs an APC40. Tracks, steps, patterns and FX count from 1; values are 0-127 like the `T`/`A`/`M` lines; *v* flags are 0 off, 1 on, 2 toggle. `mute t v`, `solo t v`, `knob t i val` (*i* 0-11 as in `T`; 12 = crossfader side 0-2), `acid t i val` (*i* 0-7 as in `A`), `glob i val` (*i* 0-9 as in `M`), `tempo x10` (600-2000), `step p t s val` (0 off, 1 on, 2 accent), `note p t s note slide`, `pat p` (queue), `pcopy from to`, `pclear p t` (*t* 0 = whole pattern), `undo`, `fx n 0\|1` (hold; released if the T95 goes quiet for 2 s), `rec v`, `metro v`, `mon v`, `type t 303\|sample`, `lrec arm t`, `lrec stop`, `lrec cancel`, `kit k page`, `audition t`. At most 31 characters |
| `ESC U set ...` `\n` | Control protocol 5 (v13.10): sets on the Daisy's card, slots 1-8. `set load n` (switches on the bar), `set save n [name]` (no name: keeps the slot's, or the open set's), `set new [name]` (first free slot), `set name n name`, `set delete n`, `set trans k` (0 cut, 1 filter, 2 tape stop, 3 echo out). Names: up to 16 of `a-z A-Z 0-9 space - _ # +`, anything else becomes `_`. Ignored while a set is saving or loading (except `trans`) |
| `ESC U` *action* `\n` (protocol 7) | Control protocol 7 (v13.12), more actions. Looper, into track *t* from the Daisy's audio input: `lrec over t` (overdub on the track's loop, in place; only at the loop's own tempo; ends on `lrec stop` at once or after the take length), `lrec shot t` (a one-shot sample: starts when the input passes about -34 dB, ends on `lrec stop` or when the slot is full; the transport isn't touched), `lrec undo` (the last take: an overdub comes off, a new take gives back what the track held; cancels a running take), `lrec len n` (0 = until stopped, 1/2/4/8 bars: the take stops by itself), `lrec count v` (count-in: clicks while armed; from stopped, one bar of them first). `lrec stop` on an armed take cancels it, and cancelling gives the track's sound back. Quantise: `quant rec n` (takes start on 0 the bar, 1 the beat), `quant pat n` (pattern switches land on 0 the bar, 1 the beat, 2 the next step), `quant mute n` (mute and solo land 0 at once, 1 on the beat, 2 on the bar). `ingain n` (input level, 64 = as it comes, 127 = x4). `roll t 0\|1` (hold: the track repeats; let go of if the T95 goes quiet). Protocol 8 (v13.13): `lrec redo` (the take `lrec undo` took off comes back; undo and redo go back and forth until the next take), `quant fx n` (held FX start and end 0 at once, 1 on the step (the default), 2 on the beat; at once while stopped). None of these settings are saved over a power cycle |
| `ESC T` *bpm100 ticks playing latency_us* `\n` | Control protocol 6 (v13.11): the T95's clock, sent every sixteenth while the clock module has the Daisy ticked. Tempo x100 (6000-20000, outside that the Daisy doesn't follow), position in MIDI clock ticks since the start (24 per beat; the Daisy uses it modulo one bar), playing 0/1, and how long frames take to arrive. The Daisy sets its tempo, starts and stops with it, and steers its sequencer onto that position: an error under half a sixteenth is slewed out over a second or two (tempo moves under 1 %), a bigger one is a jump. The sequencer stays on the audio clock. 2 s without a frame: the Daisy is on its own tempo again and keeps playing |
| `ESC F O` *id track size name* `\n` | Sample transfer, control protocol 3 (v13.8): start sending a WAV file of *size* bytes for *track* (1-16). One at a time; refused while a set loads or if it doesn't fit the staging pool |
| `ESC F D` *id seq crc32 base64* `\n` | Chunk *seq* (512 bytes, the last may be shorter), CRC32 in hex. Any order, repeats are harmless. At most 800 characters per frame |
| `ESC F C` *id* `\n` | All chunks sent: if every one arrived, the Daisy loads the file into the track like a file from its card |
| `ESC F A` *id* `\n` | Abort. 10 s without a frame also aborts |

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
| `N t name` | Track `t` sound name, or `[303]`, `[loop n bars]`, `[recorded]` (a one-shot from the input, v13.12), `[kit sound]` |
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
| `P ver att apc keys active` | T95 control protocol version (2 from v13.7), then hex port masks (bit 0 = port 1): attached, identified as APC40, keyboards; then who drives the box: -1 none, 0 the Daisy's USB-A, 1-8 a T95 port. On change and every second |
| `Y id` | Answer to `ESC P id` |
| `FO id ok capacity` / `FO id err why` | Sample transfer started (track slot capacity in samples) or refused: `busy`, `set-busy`, `track`, `too-big` |
| `FA id seq ok` / `FA id seq err why` | Chunk stored, or not: `crc`, `length`, `range`, `stale` (no such transfer) |
| `FC id ok` / `FC id err why` | Commit accepted (loading), or `missing N`, `stale`, `set-busy`, `aborted` |
| `FL id ok\|err` | The loader finished with it: in the track, or not a playable WAV |
| `H hex` | Level meters, v13.9: 36 hex digits = 16 tracks (post-fader) then master left, right; peak since the last line, 00-ff (ff = full scale), held and falling. Sent when it changes (every 33 ms while there is sound) |
| `O hex` | v13.9: which of the 64 patterns have steps, 16 hex digits (pattern 1 = lowest bit) |
| `Z t len rate hex` | v13.9: waveform outline of track `t`'s sound: length in samples at `rate`, then 96 peaks as 2 hex digits each, scaled so the loudest is ff (`-` and length 0 for a 303 track or an empty slot). Sent when the sound changes and after `ESC R`, one track per report pass; not in the once-a-second repeat |
| `J i exists bpm10 samples loops synths name` | v13.10: set slot `i` (0-7) on the card; `J i 0` when empty. On change and in the once-a-second repeat |
| `JT k name` | v13.10: the set transition (0-3) and its name |
| `KP a b n` | v13.10: the kit on tracks 1-8, on tracks 9-16 (-1 = edited since), and how many kits there are |
| `KL k name` | v13.10: name of kit `k`, for every kit; after `ESC R` and when the list changes |
| `CK f err` | v13.11: `f` 1 while following the T95's clock, `err` how far the sequencer was from it at the last frame, smoothed, in 0.1 ms (+ = behind) |
| `CL playing step bpm100 age_us n` | v13.11: each sequencer step as it plays (and the stop), with how long ago it fired, counted in audio samples, so the report's own delay drops out. The T95's clock module follows the Daisy from these. Not passed on to browsers |
| `LP len count qrec qpat qmute gain mode undo redo qfx` | v13.12: looper and quantise settings as set with `lrec len`, `lrec count`, `quant ...`, `ingain`; `mode` is what the current or last take is (0 loop, 1 overdub, 2 one-shot); `undo` is the track (1-16) whose last take `lrec undo` would take back, 0 = none. v13.13 (report 14) adds `redo` (the track `lrec redo` would bring a take back on, 0 = none) and `qfx`; undo and redo also cover a sample changed with the APC40's browse buttons. On change |
| `V n` | Report version (14). Bump it in `box.cpp` and `ui/apcbox-ui.html` together when a line changes |

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
