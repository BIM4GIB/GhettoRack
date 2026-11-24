# Handover, 2026-11-24

Written at the end of a long cloud session that could not reach the rig, for
the next session (Claude Code on Rene's Mac, which can). Read `AGENTS.md`
first, then this, then `docs/BACKLOG.md`. Delete or rewrite this file once it
is stale; it is a snapshot, not a rule book.

## Reaching the rig

- From the Mac: `ssh ghettobox` (alias in Rene's `~/.ssh/config`, key login,
  user `madmin`). The box's address stays out of this repo (it is public).
- Check first: `ssh ghettobox 'hostname; sudo -n true && echo sudo-ok'`.
  Rene was offered passwordless sudo for `madmin`; if `sudo -n` fails he
  chose not to, and installs need him.
- The repo on the box: `~/GhettoRack`. To put a change on it:
  `ssh ghettobox 'cd ~/GhettoRack && git pull && sudo t95/bridge/install.sh'`
  (and `sudo t95/kiosk/install.sh` when `t95/kiosk/` changed).
- Pages: `http://<box>:8040/`, `https://<box>:8443/` (PINs and passwords only
  over https). Health: `curl -s localhost:8040/health` on the box.
- Rene's point, and the reason for this handover: debug on the box yourself.
  Do not hand him snippets to run and paste back.
- Never open the Daisy's serial port with echo on (`AGENTS.md`). The bridge
  owns it.

## What runs on the rig

- Daisy firmware v13.17 (flashed 24 Nov, late): 16 s slots, 8 spare blocks
  for long loops (that part is v13.16), and it says its version
  (`curl -s localhost:8040/health | grep firmware`). Control protocol 10,
  report version 17. It boots and the bridge talks to it; no long take has
  been recorded on the rig yet (`docs/BACKLOG.md` has what to try). v13.15
  was the last one Rene played.
- Bridge, kiosk, settings helper, Bluetooth unit installed from `main` at
  `6f5377d` (Settings banners: one short line, 3 or 6 s; the bridge stops in
  half a second instead of 90 s and a kill).
- `t95/bridge/test_bridge.py` runs on the Mac too (24 Nov: 92 tests, 2 skipped
  for want of `aalink` and `ffmpeg`). The fake Daisy's reader gave up at once
  there (macOS reads end of file from a pseudo-terminal nobody has opened yet,
  Linux an error), and the Mac wakes a timed wait up to 10 ms late, so the two
  clock smoothness bounds are wider there (`TIMER_SLACK_MS`). Not run on the
  box, where a test bridge could find the rig's real controllers and remote.
- On the Mac: Playwright is installed, so the page tests in
  `t95/bridge/test_bridge.py` run. A plain `git push` gets a 403 (the active
  `gh` login is another account); the `BIM4GIB` login in `gh` works.
- The box has no compiler: `daisy/t95sim.cpp` for `EndToEndTest` was built on
  the Mac with a `pty.h` shim (`#include <util.h>`) outside the repo.

## Seen working on the rig (23 and 24 Nov)

- Settings: PIN, box name, Wi-Fi scan, join with a wrong password (back in
  4 s, "wrong password"), Bluetooth attaching at boot and a clean shutdown
  (two reboots), the kiosk coming back after the box was renamed.
- AP mode by hand: a phone saw the network and passed the WPA2 handshake.
- The slow Wi-Fi: found and fixed, see below.

## The slow Wi-Fi (24 Nov), in case it comes back

Symptom: pages sluggish, "box disconnected" now and then. Ping from the box to
the router 700 to 1100 ms, 0% loss. Cause: the bridge wrote every Daisy report
line (about 100 a second) to a browser on the Wi-Fi as its own small packet,
and this chip (UWE5622, `tx_addba failed` in dmesg every 3 s since the
install) queues under that. Cleared by test: Bluetooth, the helper, the
kiosk, power save, signal. Fix: `--flush-ms` (40) in the bridge, one write per
40 ms to browsers on the network. After it: 20 to 160 ms, average 51, with one
page open. If it returns, send
less (drop or thin lines the page does not need), not just less often.
Quick check: `ping -c 10 -q <router>` on the box, with and without
`sudo systemctl stop apcbox-bridge`. Measured 24 Nov with up to 8 clients:
7 to 15 ms average (`t95/README.md`, "Wi-Fi with several browsers").

## Open, in the order I would take them

1. **Hotspot** (parked by Rene, "not critical right now"). With the helper
   the phone joined the box's network but the page at its address did not
   load. Not looked into. First things to check on the box while it is up:
   `ip -4 addr show wlan0` (is the hotspot address there, or did
   systemd-networkd take it off), `pgrep -a dnsmasq`, the phone's address,
   `journalctl -u ghettorack-helper`. "By itself" defaults to off, but Rene
   saved his hotspot password while the default was on, so it may be on for
   him: look at Settings > Hotspot.
2. **Settings banners**: done 24 Nov (`d1bb148`, `c491fb9`). Rene has not said
   yet whether the new look is right.
3. **Not seen on the rig yet**: a join with the right password, confirm and
   forget; learning a remote button in Settings > Remote; the TV remote
   walking the Settings page; the start screen after it was cut down to the
   modules (he was asked what still bothers him there and has not said).
4. **Still to build in Settings**: sound cards and SD cards under Devices (a
   plan was shown to Rene 24 Nov). Built 24 Nov and not tried on the rig: the
   PIN with the TV remote (a number pad), Bluetooth pairing (Settings >
   Bluetooth: SCAN, PAIR, FORGET) and Bluetooth MIDI (`rackbridge/ble.py`;
   nothing nearby offered MIDI, so no real device has been paired or heard).
5. From before: rig checks of the looper (overdub, undo), one-shot sampling,
   kits, clock follow, lights of moved APC40 jobs; mapping for controllers
   other than the APC40. `docs/BACKLOG.md` has the rest.

## Things that cost time, so they don't again

- `tools/check-public.sh` before every push, and look at its exit code and its
  whole output. Piping it through `tail` once let a push go out while it was
  complaining (a false alarm that time). It flags a Wi-Fi key written as an assignment, private addresses,
  MAC addresses: write tests so those shapes do not appear.
- The box has one radio. Anything that changes network (join, hotspot) takes
  the page away from a browser that came in over Wi-Fi. Answer the request
  first, switch a second later, and let the page ask how it ended.
- `netplan apply` takes the Wi-Fi down for most of a minute. The helper never
  runs it; it talks to the running wpa_supplicant over its control socket.
- A killed Bluetooth attach tool hangs in the kernel. Only the ordered stop
  in `t95/helper/ghettorack-bt.sh` is safe.
- The whole bridge suite takes about 6 minutes
  (`cd t95/bridge && python3 test_bridge.py`); one class by name is quick
  (`python3 test_bridge.py SettingsTest`). Helper: `python3 t95/helper/test_helper.py`.
- Not run in the cloud session: the Mac build (`daisy/mac/build.sh`) and
  `daisy/link_test.cpp`. The Mac can do both.
