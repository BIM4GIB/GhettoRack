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
| No lockout | A join is a trial: `netplan apply`, then the page has 60 s to say "confirm" from the new network. If it doesn't, the file goes back to what it was and is applied again |
| PIN | 4-8 digits, stored as a salted PBKDF2 hash in `/var/lib/ghettorack/pin` (root, 600). Five wrong ones lock for a minute. Until one is set nothing can be changed, and whoever sets it first owns it. Forgotten: `sudo rm /var/lib/ghettorack/pin` on the box |
| Passwords and PINs | Only accepted over the HTTPS page (8443) or from the box itself; never logged, never on a command line |
| Bluetooth | `ghettorack-bluetooth.service` runs `ghettorack-bt.sh` once per boot: `modprobe sprdbt_tty`, `hciattach_opi -n -s 1500000 /dev/ttyBT0 sprd`, then lift the rfkill block and start `bluetooth.service`. The attach tool is never stopped or started twice: on this driver a killed one hangs in the kernel (SIGKILL does nothing), the next one reports "Initialization timed out", and only a reboot clears it (rig, 2026-11-23). So on / off in Settings is the radio's rfkill switch plus `bluetooth.service`, and whether the unit is enabled for the next boot. Needs `sudo t95/helper/get-bt-tool.sh` once (a closed vendor binary, fetched from Armbian's build repo and checked against the hash of the file that was tried) |

Tests: `python3 t95/helper/test_helper.py` (fake `wpa_cli`, `netplan`,
`systemctl`; no root, no network). The bridge's side is in
`t95/bridge/test_bridge.py` (`SettingsTest`).

Not verified on the T95 yet: everything. In particular `netplan apply` with
the extra file, the trial and its way back, the Bluetooth service at boot.
Not built yet: the box's own hotspot, pairing, sound cards and SD cards under
Devices, the remote's buttons (still on the start screen).
