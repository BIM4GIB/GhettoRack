# ApcBox telemetry bridge

Runs on the T95. Owns the Daisy's USB serial port and serves the existing
monitor page to any browser on the network, fed by the Daisy's report lines.
**Read-only:** the only thing it sends the Daisy is `ESC R` (state dump).

```
Daisy --USB serial--> apcbox-bridge (T95) --SSE /events--> browsers
                       | line assembler: complete lines only, junk dropped
                       | state cache: latest S/G/T/N/M/D... + last 100 logs
                       | per-browser bounded queue (512 KB)
                       '- / (monitor page)  /health  /caps
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

Flash the Daisy with firmware **v13.3 or later first** (see below), then:

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
| `/` | `daisy/ui/apcbox-ui.html`. It reads `/caps`, shows "T95 bridge" and "Daisy" status separately, and hides the Mac build's encoder/touch controls and TFT preview |
| `/events` | SSE. Default events: one report line each. `event: bridge`: JSON status |
| `/health` | JSON: `daisy`, `port`, `serial`, `data_age_s`, `lines`, `bad_lines`, `connects`, `clients`, `client_resyncs`, `rejected_clients` |
| `/caps` | `{"source": "daisy-bridge", "control": false, "ui": false, "tft": false}` |

## Tests

```
python3 t95/bridge/test_bridge.py
```

Fake Daisy on a pty, reached through a symlink so it can be unplugged and
replugged. Checks echo off at open, `ESC R` on connect and when quiet, lines
split across reads, junk and over-long lines, the late-browser snapshot,
5 browsers at once, a stalled browser while 10 MB of `D` lines go through
(the others get every line in order), the 16-browser limit, and unplug/replug
with a browser connected. Passing on a desktop says nothing about the real
T95 with a real Daisy: latency, CPU load and long runs still need measuring there.

## Not here yet

MIDI forwarding and LED output (needs the framed control protocol in the
firmware first), kiosk, IR, sample transfer.
