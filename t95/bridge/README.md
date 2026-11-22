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
| `apcbox_bridge.py` | The bridge |
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
- Remote actions that need the Daisy (play, stop, the encoder) don't do
  anything yet: they need a firmware UI command (step 3b).
- Anyone on the network can change the bindings and the last module (the
  POST endpoints). Fine on a home network; don't expose port 8040 further.

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

## Endpoints

| Path | What |
|---|---|
| `/` | The module selector (`t95/ui/selector.html`) |
| `/apcbox` | `daisy/ui/apcbox-ui.html`. It reads `/caps`, shows "T95 bridge" and "Daisy" status separately, and hides the Mac build's encoder/touch controls and TFT preview |
| `/events` | SSE. Default events: one report line each. `event: bridge`: JSON status |
| `/health` | JSON: `daisy`, `port`, `serial`, `data_age_s`, `lines`, `bad_lines`, `connects`, `clients`, `client_resyncs`, `rejected_clients`, `control`, `serial_out_bytes`, `serial_dropped`, `controllers` (port, name, kind, counters), `ping_ms` |
| `/caps` | `{"source": "daisy-bridge", "midi": true, "ir": true, "selector": "/", "control": false, "ui": false, "tft": false}` |
| `/state` | JSON: modules, last, autostart_s, IR profiles, actions, receiver, learning |
| `POST /select` | `{"module": "apcbox"}`: remember the last module |
| `POST /settings` | `{"autostart_s": 0-60}` |
| `POST /ir/learn` | `{"profile", "action", "press": "tap"/"hold"}`: the next button press is bound |
| `POST /ir/forget`, `/ir/cancel` | remove a binding, stop learning |

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
home from the ApcBox page, a button cancels the countdown). Passing on a desktop says nothing about the real
T95 with a real Daisy: latency, CPU load and long runs still need measuring there.

## Not here yet

Knobs, pads and pitch bend on the keyboards (notes only so far), MIDI clock
in/out, remote control of the Daisy itself (step 3b), sample transfer. No real-time priority: add it only if
the ping figures on the T95 say so.
