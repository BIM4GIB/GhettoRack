# Settings helper

The Settings page (`/settings`, `t95/ui/settings.html`) changes things on the
T95 that need root: Wi-Fi, the box's name, Bluetooth, reboot. The bridge runs
without rights on purpose, so it asks `ghettorack_helper.py` over a local
socket (`/run/ghettorack/helper.sock`, group `ghettorack`). The helper does
only what is on its list (the docstring has it) and asks for the PIN for
everything that changes the box. `sudo t95/bridge/install.sh` installs both.

| What | How |
|---|---|
| Wi-Fi | This box runs Netplan + systemd-networkd + wpa_supplicant. Networks joined in Settings go into `/etc/netplan/40-ghettorack-wifi.yaml` (mode 600, JSON), which Netplan merges with the install's own files; those are never rewritten. Scanning is `wpa_cli` |
| No lockout | A join is a trial in the running wpa_supplicant only (over its control socket: add the network, select it), so the saved config isn't touched and nothing restarts. Wrong password or no connection within 25 s: the network is removed and the old ones are enabled again, within seconds. Once the box is on it, the page has 60 s to say "confirm" (it does so by itself as soon as it reaches the box); only then is it written to the Netplan file (`netplan generate` checks it; the file takes effect at the next start, the running Wi-Fi already has the network). No confirm: back as above |
| Why not `netplan apply` | The first version wrote the file and ran `netplan apply` for the try and again for the way back. On the rig (2026-11-23) each one took the Wi-Fi down for a long while: a wrong password meant minutes of a stuck page. It is not run by the helper any more |
| Hotspot | The box's own network for where there is no Wi-Fi: named like the box, WPA2, channel 6, the box at `10.42.0.1` (pages at `https://10.42.0.1:8443`), no internet and no route handed out, so a phone keeps its own. It is one more network in the running wpa_supplicant (`mode=2`), plus `dnsmasq` for addresses only (`dnsmasq-base`, started by the helper, no service). Never without a password; the password is in `/var/lib/ghettorack/hotspot` (root, 600) |
| Hotspot by itself | With "by itself" on (default once a password is set): no network for 30 s and the hotspot comes up. While nobody is on it, it looks every 2 minutes whether a known network is back and returns to it. If the driver can't scan as an access point, the hotspot steps aside for 30 s instead to let the Wi-Fi look, and returns if nothing is found |
| Hotspot by hand | From Settings: the box leaves the Wi-Fi for it. Nobody joins within 2 minutes: back to the Wi-Fi (the same "goes back by itself" as a join). Once somebody has joined it stays until turned off. A join made from the hotspot that fails brings the hotspot back |
| PIN | 4-8 digits, stored as a salted PBKDF2 hash in `/var/lib/ghettorack/pin` (root, 600). Five wrong ones lock for a minute. Until one is set nothing can be changed, and whoever sets it first owns it. Forgotten: `sudo rm /var/lib/ghettorack/pin` on the box |
| Passwords and PINs | Only accepted over the HTTPS page (8443) or from the box itself; never logged, never on a command line |
| Bluetooth | `ghettorack-bluetooth.service` runs `ghettorack-bt.sh` once per boot: `modprobe sprdbt_tty`, `hciattach_opi -n -s 1500000 /dev/ttyBT0 sprd`, then lift the rfkill block and start `bluetooth.service`. The attach tool is never plainly killed or started twice: on this driver a killed one hangs in the kernel (SIGKILL does nothing), the next one reports "Initialization timed out", only a reboot clears it, and that reboot stalls for minutes (rig, 2026-11-23). Stopped in order it exits cleanly (rig, same day, one run by hand: `bluetoothctl power off`, stop `bluetooth.service`, SIGTERM): the unit's `ExecStop` does that at shutdown (seen on the rig: two reboots without the stall). On / off in Settings is the radio's rfkill switch plus `bluetooth.service`, and whether the unit is enabled for the next boot. Needs `sudo t95/helper/get-bt-tool.sh` once (a closed vendor binary, fetched from Armbian's build repo and checked against the hash of the file that was tried) |
| Bluetooth devices | `bt_scan` (no PIN): `bluetoothctl scan on` for 8 s, and while it runs the named devices in reach, MIDI ones first (those that offer the Bluetooth LE MIDI service). `bt_pair` (PIN): one `bluetoothctl` session in the order this radio needs: `scan on`, `connect`, `scan off`, `pair`, `yes` to the code (the device shows the same one and its owner confirms there; devices that want a code typed are not handled), `trust`. A phone or tablet is listed under another address once paired; the answer names that one. `bt_forget` (PIN): `remove`. Addresses are checked against their pattern before they reach a command line. The status lists the paired devices. Reading MIDI from a paired device is the bridge's (`t95/bridge/rackbridge/ble.py`), which may connect to it but not pair. VISIBLE (the box as a Bluetooth MIDI device, 3 minutes) is the bridge's too: it needs no root, only the helper's `pin_check` before it starts. Seen on the T95 (2026-11-24): the scan from the Settings page, 5 named devices listed, none of them MIDI. An iPad was paired on the T95 by hand in that order (2026-11-24); `bt_pair` itself and forgetting only with a fake `bluetoothctl` |
| SD card | `card_status`, `card_copy take`, `card_eject` (no PIN: they change the card, not the box). The udev rule mounts a card read-only, so pulling it is always safe. `card_copy` writes one take of the jam recorder (`/var/lib/apcbox-bridge/jams/<take>/`) as one WAV to `jams/<take>.wav` on the card: `mount -o remount,rw` for the copy, a `.part` file renamed at the end, `fsync`, then `remount,ro` whatever happened. Only when what is mounted at `/media/sdcard` is vfat or exfat on an `mmcblk` device whose type is `SD` (not the eMMC, not the ext4 recovery card). The takes folder is written by the bridge, which has no rights: take names are checked against the bridge's pattern, the take must be a real folder and its parts plain files (opened with `O_NOFOLLOW`), so nothing outside it can be copied out. Refused: no room, over 4 GB, a file of that name on the card, a copy already running. `card_eject` is `systemctl stop media-sdcard.mount`: the helper has its own mount namespace, and an `umount` run in it left the card mounted for the rest of the box (rig, 2026-11-24) |

Tests: `python3 t95/helper/test_helper.py` (fake `wpa_cli`, `netplan`,
`systemctl`; no root, no network). The bridge's side is in
`t95/bridge/test_bridge.py` (`SettingsTest`).

Seen on the T95 (2026-11-23): PIN, box name, scan, Bluetooth at boot and at
shutdown. A wrong password in the join trial: the box left its network, got
"wrong password" from wpa_supplicant (TEMP-DISABLED, reason WRONG_KEY) and was
back on the old network 4 s after it left (journal, same day). The page lost
the answer to "join" in that gap (one radio: the box has to leave the network
the page came in on) and showed a stale "not answering"; the helper now
answers first and leaves a second later, and the page waits for the result.
Not seen there yet: that page fix, a join with the right password, confirm
and forget.
The hotspot: only this much is seen on the T95 (by hand, 2026-11-23): wlan0 goes
into AP mode in the running wpa_supplicant, a phone sees the network and
passes the WPA2 handshake. With the helper (same day): the phone joined but
the page at the box's address didn't load; not looked into yet, so "by
itself" is off by default. Not seen there: the helper doing it, the address
staying on wlan0 next to systemd-networkd, dnsmasq handing out addresses, the
pages over it, starting by itself, scanning as an access point, the way back.
Sound cards and the SD card under Devices are the bridge's (read-only, no helper:
`t95/bridge/rackbridge/devices.py`).
