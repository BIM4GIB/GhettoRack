# Raspberry Pi 4 as the companion box

An A/B test against the T95 (`t95/README.md`): the same bridge, pages and
kiosk on a Raspberry Pi 4, with its own Daisy Seed, and the 3.5" touch screen
on the Pi's header instead of on the Daisy. Nothing here replaces the T95
setup; the code under `t95/` is shared and unchanged.

## Status as of 2026-11-25

The Pi runs the bridge, the settings helper and the kiosk from `main`,
installed with the stock `t95/bridge/install.sh` and `t95/kiosk/install.sh`.
A second Seed on its USB runs ApcBox. The screen shows the kiosk and takes
touch, with a layout of its own (480x320). **No comparison with the T95 has
been measured yet.**

## Hardware and evidence

| Item | Findings |
|---|---|
| Board | Raspberry Pi 4 Model B Rev 1.4, 8 GB (the T95 has 4 GB) |
| OS | Raspberry Pi OS Lite 64-bit, Debian 13 (Trixie), kernel `6.18.50+rpt-rpi-v8`, on a 64 GB microSD |
| Network | NetworkManager, Wi-Fi. The T95 uses Netplan and systemd-networkd |
| USB | One controller (xhci) for all four sockets, behind one internal hub. The Daisy does not get a controller of its own as on the T95 |
| Power | `vcgencmd get_throttled` stayed `0x0` through the installs, with the Seed and the screen on |
| Bridge | Installed unchanged, found the Seed by VID/PID, control protocol 10, report 19, 0 bad lines in 310,000 |
| Second Seed | Fresh Seed, bootloader and v13.26 written from the Pi with `dfu-util` (the steps of `tools/seed2-setup.sh`, done by hand). After the bootloader the Seed came back in the Daisy bootloader by itself; no replug was needed. Wired: OLED, SD module (CS soldered to 3V3 on the module). Not wired: audio, USB-A host, encoder |
| Seed's card | 32 GB SDHC, FAT32, filled with `tools/sdprep.py --go` (4 kits, 19 folders, 4,123 files). The Seed lists the four kits. Not a copy of the T95 rig's card: that one also has a kit `stk` and its settings files |
| Ping Pi to Daisy | 300 pings: min 0.29, avg 0.60, p99 5.1, max 6.3 ms. After a reboot, 36 pings: avg 0.48, max 1.27. **Not comparable with the T95's figures yet:** this Seed has no screen, APC40 or audio attached, and the T95 figures are from v13.6 to v13.9 |
| Seed's loop | Its own log: slowest pass 119.5 ms in 10 s, of which 114.9 ms in the screen code, with no screen attached. The firmware still draws for a screen that is on the Pi now |
| Kiosk | cage + Chromium 156 on the 3.5" screen, software rendering (pixman), about 9% of one core idle, no restarts. HDMI not tried |
| Screen | Picture right (colours, fit, landscape) by eye on a test pattern. 16 MHz SPI, not pushed further; frame rate not measured |
| Touch | Four corner taps read; calibration installed; ten taps then landed within about 12 px of where they were made (centre: 234,160 of 240,160) |
| Survives a reboot | Driver, touch, kiosk and bridge came back by themselves (one reboot) |
| Bluetooth | The Pi's own radio; the bridge's Bluetooth MIDI side reports ready. Nothing paired |
| IR / remote | No IR receiver. The bridge took the HDMI port's CEC device (`vc4-hdmi-0`) for a remote. Not tried with a TV |

## What differs from the T95

- `t95/wifi-prep/` and the device-tree patch: not needed.
- `ghettorack-bluetooth.service` (the UWE5622 attach tool): T95 only. Installed
  by the bridge's installer but not enabled; leave it off.
- Settings > Wi-Fi and the hotspot: the helper drives Netplan and
  wpa_supplicant. They will not work here until it knows NetworkManager.
- Two udev rules in `t95/bridge/99-apcbox.rules` could misfire on a Pi and
  have not: the "SD card in the slot" rule matches the Pi's boot partition
  (same card type, FAT) but it was not mounted at `/media/sdcard` after a
  reboot; the "set remotes to NEC" rule left the CEC devices at `cec`. Both
  should become board-specific.
- TV sound: the HDMI cards are `vc4hdmi0` and `vc4hdmi1`, not `HDMI`.

## The screen on the Pi's header

The SunFounder 3.5" IPS screen is a Pi HAT: an ST7796U controller behind an
SPI to 16-bit shift register board, and an XPT2046 touch chip. It uses the
same pins as the OzzMaker PiScreen and Waveshare 3.5" screens.

| Signal | Pi header pin | GPIO |
|---|---|---|
| SCK, MOSI, MISO | 23, 19, 21 | SPI0 |
| Screen CS | 24 | CE0 |
| DC | 18 | 24 |
| Reset | 22 | 25 |
| Touch CS | 26 | CE1 |
| Touch IRQ | 11 | 17 |

The kernel's own driver for those screens (`piscreen` overlay with `drm`,
module `ili9486`) talks to the board correctly but sends another controller's
start-up commands: colours came out inverted and the picture did not fit.
`screen/install.sh` builds `st7796sr` instead: the kernel's `ili9486.c` with
the start-up sequence ApcBox uses on this screen in `daisy/tft.h`. It is a
DRM driver, which the kiosk's compositor needs.

```
sudo pi4/screen/install.sh            # module (DKMS), overlay, touch, kiosk; then reboot
sudo pi4/screen/install.sh --remove
sudo systemctl stop kiosk && sudo python3 pi4/screen/testpattern.py
python3 pi4/screen/shot.py /tmp/kiosk.png      # what the kiosk's page looks like
```

`config.txt` gets `dtparam=spi=on` and `dtoverlay=ghettorack-screen`; the
copy from before is `/boot/firmware/config.txt.before-screen`. Overlay
parameters: `speed` (Hz, default 16000000), `rotate` (0, 90, 180, 270),
`invx`, `invy`, `swapxy` for touch.

The kiosk draws on this screen only when `/etc/default/kiosk` has

```
WLR_DRM_DEVICES=/dev/dri/by-path/platform-fe204000.spi-cs-0-card
WLR_RENDERER=pixman
```

Touch calibration is one line in `screen/98-ghettorack-touch.rules`, worked
out from four corner taps on this one screen. Another screen may want other
numbers.

## Open

- The 480x320 layout (`t95/ui/VIEWS.md`, "Small") is on the screen since
  2026-11-25: the kiosk opens `/apcbox?small=1` (`KIOSK_URL` in
  `/etc/default/kiosk`). Seen through the kiosk's own browser on the Pi with
  the real Seed: all six views dense and zoomed, a step, mute, track select,
  pattern change and a fader reaching the Daisy and coming back. Not seen: how
  it reads on the panel itself, touch on the small targets (calibration is
  good to about 12 px, the smallest targets are 14 px), the frame rate with
  the mixer's meters moving, LIBRARY with files in it.
- Chromium will not make a window narrower than 500 px. The small layout is
  drawn in the left 480, so nothing is cut off; the start screen and Settings
  are still the TV pages.
- Done in v13.29: the firmware no longer draws when no screen answers.
- The A/B itself: both Seeds on one firmware version with the same things
  attached, then the T95's measurements repeated here (ping under load,
  bridge CPU, Wi-Fi ping with a page open, page speed, boot time, Bluetooth
  MIDI reconnect), ideally once more with the Seeds swapped.
- SPI speed and frame rate of the screen.
- `install.sh` fetches `ili9486.c` for the running kernel line from GitHub;
  a kernel that changes that file enough stops `mkdriver.py` with a message.
