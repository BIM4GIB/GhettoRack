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
| PIN | 4-8 digits, stored as a salted PBKDF2 hash in `/var/lib/ghettorack/pin` (root, 600). Five wrong ones lock for a minute. Until one is set nothing can be changed, and whoever sets it first owns it. Forgotten: `sudo rm /var/lib/ghettorack/pin` on the box |
| Passwords and PINs | Only accepted over the HTTPS page (8443) or from the box itself; never logged, never on a command line |
| Bluetooth | `ghettorack-bluetooth.service` runs `ghettorack-bt.sh` once per boot: `modprobe sprdbt_tty`, `hciattach_opi -n -s 1500000 /dev/ttyBT0 sprd`, then lift the rfkill block and start `bluetooth.service`. The attach tool is never plainly killed or started twice: on this driver a killed one hangs in the kernel (SIGKILL does nothing), the next one reports "Initialization timed out", only a reboot clears it, and that reboot stalls for minutes (rig, 2026-11-23). Stopped in order it exits cleanly (rig, same day, one run by hand: `bluetoothctl power off`, stop `bluetooth.service`, SIGTERM): the unit's `ExecStop` does that at shutdown (seen on the rig: two reboots without the stall). On / off in Settings is the radio's rfkill switch plus `bluetooth.service`, and whether the unit is enabled for the next boot. Needs `sudo t95/helper/get-bt-tool.sh` once (a closed vendor binary, fetched from Armbian's build repo and checked against the hash of the file that was tried) |

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
Not built yet: the box's own hotspot, pairing, sound cards and SD cards under
Devices, the remote's buttons (still on the start screen).
