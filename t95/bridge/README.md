# ApcBox bridge

Runs on the T95. Owns the Daisy's USB serial port, forwards the T95's MIDI
controllers to the Daisy (and the APC40's LEDs back), and serves the
monitor page to any browser on the network.

```
APC40 / LPK25 / MPK249 --USB--> T95 hub --raw MIDI--+
                                                    |  ESC M p hex   (MIDI in)
                     apcbox-bridge (T95) <----------+  ESC N p name  (plugged / gone)
                       | MIDI parser                   ESC K, ESC P  (keepalive, ping)
                       | per-controller out queue      X p hex       (LEDs back)
                       | line assembler, state cache
                       | per-browser queue (512 KB)
                       '- / (page)  /events  /health  /caps
                                  ^
Daisy <-----USB serial------------'
```

Python 3 standard library only. Files:

| File | What |
|---|---|
| `apcbox_bridge.py` | Entry point (the service runs it) |
| `rackbridge/` | The bridge, one file per building block: `daisy` (Daisy link), `midi` (USB ports), `router` (modules' routing), `netmidi` (RTP-MIDI), `clock` (clock module), `listen` (audio to browsers), `samples`, `ir` (remote), `hdmi` (TV sound), `state`, `web` (HTTP/SSE/API), `bridge` (puts them together) |
| `ghettorack-midi.service` | Avahi service: the T95 shows up as "GhettoRack" for network MIDI |
| `test_bridge.py` | Tests against a fake Daisy on a pseudo-terminal |
| `apcbox-bridge.service` | systemd unit: own throwaway user in `dialout`, no RT priority |
| `99-apcbox.rules` | udev: `/dev/apcbox` link, `dialout` access, ModemManager keeps off it |
| `install.sh` | Copies to `/opt/apcbox-bridge`, installs the rule and unit, starts it. `--remove` undoes it |

## Install on the T95

Flash the Daisy with firmware **v13.3 or later first** (v13.5 for
controllers; see below), then:

```
cd ~ && git clone https://github.com/BIM4GIB/GhettoRack.git   # or git pull
sudo ~/GhettoRack/t95/bridge/install.sh
```

Open `http://<t95-ip>:8040/`. Check it from a shell:

```
curl -s localhost:8040/health
journalctl -u apcbox-bridge -f
```

## Why v13.3 first

A Linux tty starts with echo on. v13.2 and older reboot into the bootloader
on any `B` byte, and the Daisy's own report lines are full of them, so a
plain `cat`/`stty` on the port echoed them back and sent the Daisy into its
bootloader (seen on the T95, 2026-11-22). v13.3 only acts on `ESC R` and
`ESC BOOT`. The bridge also opens the port raw with echo off before reading
(the tests check this), but on old firmware there is still a short window at
open. Don't open the port with `cat`, `stty` or `screen` yourself; use
`/health` or the page.

## Controllers (firmware v13.5 on)

- Controllers are found under `/sys/class/sound/midiC*D*` and named from
  their USB product string, keyed by USB device path, so ALSA card numbers
  can change. Rescanned every second; each gets port 1-8 while plugged in.
- Nothing goes to the Daisy until it sends a `P` line (control protocol 1).
  A v13.3/v13.4 Daisy never does, so the bridge stays read-only with it.
- MIDI in: running status, clock and other realtime bytes in the middle of
  messages, sysex split across reads. Clock, active sensing and system
  common are dropped (and counted): tempo sync will live on the Daisy.
- The Daisy decides what each port is: an APC40 (its identity reply) drives
  the box unless one is plugged into the Daisy's own USB-A; anything else is
  a keyboard and its notes play the selected track (303: the note, folded
  into the 303's range, velocity 100+ = accent; sample track: the sample
  pitched, C4 = as recorded, velocity = level).
- LEDs out: sysex in order, then the newest value per note/CC. If a
  controller stops taking bytes, older LED values are replaced instead of
  queueing up (`coalesced` in `/health`).
- Daisy not taking bytes: at most 64 KB waits; beyond that controller MIDI
  is dropped and counted (`serial_dropped`, per port `dropped_in`).
- Keepalive every 500 ms. If the bridge stops (crash, restart), the Daisy
  drops the T95 controllers after 2 s and releases anything held.
- Ping every second: `ping_ms` in `/health` is the round trip bridge to
  Daisy main loop and back (min/avg/p99/max/jitter). It doesn't include the
  controller's own USB delay or the audio output.

## Module selector and the remote

- `/` is the selector (`t95/ui/selector.html`): modules (ApcBox ready, the
  others later), the remote setup, the start-up countdown. `/apcbox` is the
  ApcBox page; under the bridge it has a "Modules" button back.
- The IR receiver is found under `/sys/class/rc/`. The bridge reads decoded
  scancodes from its lirc device (`/dev/lirc0`), which marks repeat frames
  and the protocol: a press starts only on a fresh NEC frame. On the T95 the
  kernel also produced imon noise (`0x7fffffff`) and lone repeats carrying
  the previous button's code, which made learning bind the wrong buttons;
  those are now ignored (`strays` in `/health`). The input device is held
  exclusively so the kiosk doesn't also see the remote as keys; without lirc
  it's read instead (`MSC_SCAN`). The udev rule leaves only NEC decoding on.
- A button is bound per profile: `all`, `selector`, `apcbox`. A view's own
  profile wins; `all` fills the gaps. Each button can have a tap action and
  a hold action (half a second). Arrows and volume act on press and repeat
  while held, unless they also have a hold action.
- Learn from the page: press "learn" next to an action, then the button.
  That press only teaches. Learning times out after 15 s.
- Browsers say their view when they connect (`/events?view=selector`) and
  get `ir` events for it: `{"action": "ok", "phase": "up"}`. Also `irraw`
  (any button, for the setup page) and `irlearned`.
- Kept in `state.json` (systemd `StateDirectory`, `/var/lib/apcbox-bridge`):
  last module, countdown, bindings. Survives restarts and reboots.
- Remote actions for the Daisy, while a browser shows the ApcBox page:
  left/right turn the encoder, ok pushes it, back is its back, play/stop,
  number keys 1-9 and 0 pick tracks 1-10. With firmware v13.7 (control
  protocol 2) they go as UI actions (`ESC U`). With older firmware only
  play/stop work, as presses of the APC40's PLAY/STOP on the port of the
  APC40 that drives the box (so only with that APC40 on the T95).
- `/ui?a=turn&d=N|push|back`: the page's encoder buttons (same URLs as the
  Mac build). 503 until the Daisy speaks protocol 2.
- Anyone on the network can change the bindings and the last module (the
  POST endpoints). Fine on a home network; don't expose port 8040 further.

## Samples (firmware v13.8 on)

The T95 keeps the sample library; the Daisy only gets a track's sound.

- Library: `/var/lib/apcbox-bridge/samples` on the eMMC (persists). Uploads
  from the page land in `uploads/`; imports from the SD card in `sdcard/`.
- The T95's SD slot: FAT/exFAT cards are mounted read-only at `/media/sdcard`
  (udev rule; only SD cards, never the eMMC, never ext4 cards such as the
  recovery card). Browse it, send from it directly, or import into the
  library. Taking the card out unmounts it. The Daisy's own card can be
  moved into the T95's slot to import it (power the Daisy off first).
- Sending: converted with ffmpeg to mono, 16-bit, 48 kHz (without ffmpeg a
  WAV goes unchanged; the Daisy's parser takes PCM 8-32 bit, float, any
  rate). Then `ESC F` frames: 512-byte chunks, base64, CRC32 each, 4 in
  flight, resent after 0.5 s or an error, at most 8 tries per chunk. Sample
  frames wait behind MIDI and control on the serial link.
- On the Daisy the file goes into the staging pool (no extra SDRAM) while
  the track keeps playing; when complete, the normal loader reads it like a
  file from its card. A set saved on the Daisy afterwards keeps the sound.
  A T95 sample doesn't survive a Daisy power cycle unless it's in a set.
- Only one transfer at a time; a set can't load during one (and the other
  way round).
- Recording: the Samples panel's record button records this device's
  microphone (echo cancellation, noise suppression and auto gain off), up to
  30 s, uploads it and, unless unticked, sends it to the selected track.
  Phones only allow the microphone on HTTPS, so the bridge also serves
  `https://<t95-ip>:8443/` with a self-signed certificate it makes itself
  (openssl, in the state folder, remade when the address changes). The
  browser warns once. On plain HTTP the panel links to the HTTPS page.
- `/health` → `samples`: current transfer, queue, recent results, speed.
  Browsers get `xfer` events with the same.

## Sound on the TV

"Sound on the TV (HDMI)" on the selector page: the bridge runs
`alsaloop -C plughw:CARD=ApcBox -P plughw:CARD=HDMI -t 40000 -S 1` while the
setting is on and both sound cards exist, starts it again when the Daisy
comes back, and remembers the setting (`state.json`). It adds about 40 ms
plus the TV's own delay: for listening while testing, not for playing.
Status in `/health` → `hdmi_audio`. Card names: `--daisy-card`, `--hdmi-card`.

## Listening on a phone, tablet or Mac

LISTEN in the ApcBox page's top bar plays what the Daisy plays, in that
browser. `GET /audio.pcm` is raw 16-bit 48 kHz stereo from `arecord` on the
Daisy's USB audio (192 kB/s per listener, up to 4), started with the first
listener and stopped with the last. The page keeps about 0.15 s buffered and
skips ahead if more piles up; the bridge keeps at most 0.5 s per listener
and small socket buffers, so a bad link drops audio, it doesn't fall behind.
Expect roughly 0.2 s behind the Daisy: for hearing what you do, not for
playing in time.

The capture device opens once, and the TV sound uses it too. Both now open
`plug:daisyshare`, an ALSA dsnoop device defined in
`/var/lib/apcbox-bridge/asound-share.conf` (written by the bridge, passed
with `ALSA_CONFIG_PATH`). If alsaloop can't open it, the TV sound goes back
to `plughw:CARD=ApcBox` by itself, and listening then needs the TV sound
off (the page says so). **Not yet tried on the T95:** whether dsnoop works
with the Daisy's audio there, and the real delay over Wi-Fi.
`curl -s localhost:8040/health` shows `listen` (clients, running, shared,
dropped_bytes, error).

## Behaviour

- Finds the Daisy by USB VID `1209` / PID `0001` under `/sys`, not by tty
  number. Several Daisies: pick one with `--serial <USB serial>`.
- Daisy unplugged or rebooted: browsers stay connected, get a `bridge` event
  with `"daisy": false`, and a fresh snapshot when it's back.
- Daisy quiet for 3 s: asks for a dump again (`ESC R`), at most every 3 s.
- New browser: a `bridge` status event, then the cached state (`V` first),
  then live lines.
- Browser falls more than 512 KB behind: its backlog is thrown away and it
  starts again from a snapshot. Other browsers and the serial reader never
  wait for it. A browser stuck for 30 s is dropped.
- At most 16 browsers; the 17th gets 503.
- Lines longer than 4096 bytes or with non-ASCII bytes are dropped and
  counted (`bad_lines` in `/health`).

## Modules, routing, network MIDI

Every mode in the selector is a module (`MODULES` in `rackbridge/common.py`):
page, what it needs (ApcBox: the Daisy), remote profile, default routes. One
is active (the last chosen); its routes are what the router applies.

```
 USB ports ----+                      +--> USB ports (own FIFO each, never coalesced)
 RTP sessions -+--> router (by name) -+--> RTP sessions
                    from, to, ch,     '--> Daisy (ESC M; only ports routed to it
                    remap, types           are announced; never clock, sysex <= 128)
```

- ApcBox: default `* -> daisy`, behaviour as before.
- MIDI hub (`/midihub`): no defaults; matrix on the page (TV remote: arrows
  walk the grid, ok toggles), presets, network devices with "never".
- A channel route carries that channel's messages only (no clock or sysex).
  Clock/start/stop/continue go port to port. A port that disappears sends
  note offs for whatever it was holding. Two identical USB devices get "2".
- Network MIDI: udp 5004 control, 5005 data (`--rtp-port`, 0 = off). The
  Avahi file announces it; `avahi-browse` every 10 s finds the rest and every
  session found is invited (sync every 10 s, dropped after 60 s silent or BY).
  Devices that invite the T95 are accepted unless marked never. No recovery
  journal. Anyone on the LAN can connect: it's a jam box, not a secure one.

## Clock

The clock module (`/clock`): one tempo for everything.

- Tempo from: this box (buttons, tap, remote up/down), Link (optional
  `aalink`, installed into `/opt/apcbox-bridge/venv`), or MIDI clock from any
  port. Cubase can't follow external clock or speak Link, so with Cubase it
  leads: Studio > Project Synchronization Setup > MIDI Clock Destinations, tick
  the network session.
- Following MIDI clock: incoming ticks are fitted to a line over the last 96
  (tempo changes are spotted and the fit restarts), our ticks go out on that
  line. Gap of 0.5 s: the last tempo is held. Start, stop, continue and song
  position are passed on before the next tick.
- Sends 24 ticks per beat to the ticked ports from one thread: sleep to
  0.5 ms before the tick, spin the rest; GIL switch interval 1 ms. Lateness
  of every tick is on the page and in `/health`.
- Desktop figures (not the T95): internal 120 BPM interval 20.833 ms, sd
  0.1-0.4 ms; following 125 BPM sent with 2 ms jitter: locked at 124.96,
  output sd 0.26 ms; Link 97 BPM output sd 0.04 ms.
- The Daisy (v13.11), both ways. Tick "Daisy" under "send clock to": an
  `ESC T` frame every sixteenth (tempo, position, playing, half the ping time);
  the firmware steers its sequencer onto it and stays on its audio clock.
  Or pick the Daisy as the source: it reports each step with its age (`CL`),
  our ticks go out on its grid (even steps only, so swing doesn't pull it).
  Desktop, against the firmware's control code: Daisy following at 137 BPM
  off by 0.8 ms; T95 following the Daisy at 130 BPM with swing, tick sd 0.4 ms.
  Not flashed or measured on the rig yet.
- Not yet: the Cubase plugin (Cubase sync mode, planned).

## Endpoints

| Path | What |
|---|---|
| `/` | The module selector (`t95/ui/selector.html`) |
| `/apcbox` | The T95's ApcBox page (`t95/ui/apcbox.html`): full-screen views, works with no APC40, needs firmware v13.9 |
| `/audio.pcm` | The Daisy's audio for the page's LISTEN button: raw s16le 48 kHz stereo, endless |
| `/samples/peaks?src=&path=` | JSON `{secs, peaks}`: 96 peaks (hex) of what the Daisy would get of that file, for the waveform in the list |
| `/samples/preview?src=&path=` | That file as the Daisy would get it (WAV), for hearing it in the browser. `&start=&end=` (0-1, fractions of its length) and `&norm=1`: trimmed (2 ms fades at the cuts) and normalised to -0.3 dB, as `/samples/send` would send it |
| `POST /do` | `{"a": "mute 3 2"}` or a list of up to 32: page actions for the Daisy (`ESC U`), checked against a list before they go on the wire |
| `/classic` | `daisy/ui/apcbox-ui.html`, the monitor page. It reads `/caps`, shows "T95 bridge" and "Daisy" status separately, and hides the Mac build's encoder/touch controls and TFT preview |
| `/events` | SSE. Default events: one report line each. `event: bridge`: JSON status |
| `/health` | JSON: `daisy`, `port`, `serial`, `data_age_s`, `lines`, `bad_lines`, `connects`, `clients`, `client_resyncs`, `rejected_clients`, `control`, `serial_out_bytes`, `serial_dropped`, `controllers` (port, name, kind, counters), `ping_ms` |
| `/caps` | `{"source": "daisy-bridge", "midi": true, "ir": true, "selector": "/", "control": false, "ui": false, "tft": false}` |
| `/state` | JSON: modules, last, autostart_s, IR profiles, actions, receiver, learning |
| `POST /select` | `{"module": "apcbox"}`: the active module (its routing applies now) |
| `/midihub` | The MIDI hub page (`t95/ui/midihub.html`) |
| `GET /routes` | active module, its defaults and routes, sources, destinations, presets, types, `net` |
| `POST /routes` | `{"routes": [{"from", "to", "ch"?, "remap"?, "types"?}]}` for the active module (max 64) |
| `POST /presets/save\|load\|delete` | `{"name"}`, per module |
| `GET /clock?json=1`, `POST /clock` | clock state; set `on`, `source`, `bpm`, `outputs`, `send_stopped`, `link` |
| `POST /clock/transport` | `{"cmd": "start"\|"stop"\|"continue"\|"toggle"}` (internal and Link sources) |
| `POST /net/never` | `{"name", "never": true/false}`: don't invite or accept that network device |
| `POST /settings` | `{"autostart_s": 0-60}` and/or `{"hdmi_audio": true/false}` |
| `POST /ir/learn` | `{"profile", "action", "press": "tap"/"hold"}`: the next button press is bound |
| `POST /ir/forget`, `/ir/cancel` | remove a binding, stop learning |
| `GET /samples?src=lib\|sd&path=` | folders and audio files (paths can't leave the library or the card) |
| `/settings` | The Settings page (`t95/ui/settings.html`): network, Bluetooth, system, devices |
| `/sys/state` | What the settings helper reports (Wi-Fi, address, name, saved networks, trial, PIN set, Bluetooth) plus the bridge's own (Daisy, controllers, start-up delay). `{"helper": false}` when the helper isn't running |
| `POST /sys/do` | `{"cmd": ...}` passed to the settings helper (`t95/helper/README.md`): `scan`, `join`, `confirm`, `forget`, `hostname`, `bt`, `restart_bridge`, `reboot`, `poweroff`, `pin_set`, `pin_check`. A request carrying a PIN or a password is refused unless it came over HTTPS or from the box itself |
| `POST /samples/send` | `{"src", "path", "track"}`: convert and send to the Daisy. Optional `"start"`, `"end"` (0-1) and `"norm": true`: trim and normalise first |
| `POST /samples/upload?name=` | raw file body (up to 64 MB) into `uploads/`; page recordings come as `.webm` (Chrome) or `.mp4` (Safari) |
| `POST /samples/import` | `{"path"}`: copy a file or folder (audio only) from the SD card into the library |
| `POST /samples/delete`, `/samples/cancel` | delete from the library, drop the queue |

## Tests

```
python3 t95/bridge/test_bridge.py
```

Fake Daisy and fake controllers on ptys, reached through symlinks so they
can be unplugged and replugged. Telemetry: echo off at open, `ESC R` on
connect and when quiet, lines split across reads, junk and over-long lines,
the late-browser snapshot, 5 browsers at once, a stalled browser while 10 MB
of `D` lines go through, the 16-browser limit, unplug/replug. Controllers:
nothing sent before `P`, names and keepalive after, running status, clock
bytes mid-message, sysex in pieces, LEDs out in order and not to browsers,
LED coalescing when a controller stalls, controller and Daisy hotplug, a
Daisy that stops reading while a keyboard floods, ping, discovery against a
T95-shaped `/sys` tree. Remote (fake input device): learning, per-view
actions, tap/hold, repeat, bad requests, state across a restart, and the
selector page in a browser (learn, ok opens the module, boot countdown,
home from the ApcBox page, a button cancels the countdown). Samples:
listing and path escapes, upload, SD import, sending through a fake Daisy
that loses 15% of the acks and fakes CRC errors (bytes must match), a
24-bit stereo file converted, refusals, and a 2 s sample end to end into
the firmware's transfer code and loader (t95sim). Routing: ApcBox to the
Daisy, the hub unannouncing ports and routing port to port with channel
remap, type filters, clock, sysex in pieces, bad routes, presets across a
restart, the hub page in a browser. Network MIDI against a separate fake
peer: the bridge inviting, a peer inviting, clock sync, MIDI both ways with
delta times and running status, sysex split over packets, note offs on BY,
never, BY at shutdown. Passing on a desktop says nothing about the real
T95 with a real Daisy: latency, CPU load and long runs still need measuring there.

## Not here yet

Knobs, pads and pitch bend on the keyboards into ApcBox (notes only so far;
in the hub everything passes), MIDI clock into the Daisy, RTP-MIDI recovery
journal, chaining modules, sending a whole kit or set from the T95 in one go, sets kept on the T95. No real-time priority: add it only if
the ping figures on the T95 say so.
