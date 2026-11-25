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
| Kiosk | cage + Chromium 156 on the 3.5" screen, software rendering (pixman), no restarts. HDMI not tried |
| Frame rate | With the kernel's placeholder mode (a 1 kHz pixel clock, 0.0065 Hz refresh) the page got about 1 frame a second with the SPI link nearly idle: compositor and browser pace by the refresh rate. With a 30 Hz mode in the driver and the link at 48 MHz (`pi4/screen/framerate.py`, sequencer playing, after a reboot): 28 to 32 frames a second in all six views, 33 ms between frames, worst 49 ms; STEP and MIX send about 1.4 MB/s. At 16 MHz MIX managed 8 frames a second: every frame was half the screen. Rene at the panel: clean |
| Touch delay | `pi4/screen/touchlag.py`, 7 taps: the touch is in the page 4 to 14 ms after the kernel has it. The second Rene felt was the rest: a press lasts 0.2 to 0.3 s and a click only comes at its end, and then 0.1 to 0.5 s until the frame was out at 8 frames a second. Now the page acts when the finger lands, the top bar no longer shows the step position (with the playhead that was one changed area from top to bottom), and the mixer's meters move 7 times a second. Rene after these changes: fast. Not timed again |
| Screen | Picture right (colours, fit, landscape) by eye on a test pattern, first at 16 MHz SPI, then clean at 48 MHz |
| Touch | `screen/calibrate.py`: five crosses on the screen, the chip's own numbers fitted to them; 1.2 px left over at the worst cross (2026-11-25). The first calibration, guessed from four corner taps, was about 4% off in height. Not checked by tapping targets afterwards |
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
sudo python3 pi4/screen/calibrate.py           # touch: five crosses to tap (--wait N seconds each)
python3 pi4/screen/shot.py /tmp/kiosk.png      # what the kiosk's page looks like
```

`config.txt` gets `dtparam=spi=on` and `dtoverlay=ghettorack-screen`; the
copy from before is `/boot/firmware/config.txt.before-screen`. Overlay
parameters: `speed` (Hz, default 48000000), `rotate` (0, 90, 180, 270),
`invx`, `invy`, `swapxy` for touch.

The kiosk draws on this screen only when `/etc/default/kiosk` has

```
WLR_DRM_DEVICES=/dev/dri/by-path/platform-fe204000.spi-cs-0-card
WLR_RENDERER=pixman
```

Touch calibration is one line in `/etc/udev/rules.d/98-ghettorack-touch.rules`.
`screen/98-ghettorack-touch.rules` is the one made on this screen, installed
only where there is none; `calibrate.py` makes one for the screen at hand.

## Open

- The 480x320 layout (`t95/ui/VIEWS.md`, "Small") is on the screen since
  2026-11-25: the kiosk opens `/apcbox?small=1` (`KIOSK_URL` in
  `/etc/default/kiosk`). Seen through the kiosk's own browser on the Pi with
  the real Seed: all six views dense and zoomed, a step, mute, track select,
  pattern change and a fader reaching the Daisy and coming back. Rene: looks
  good on the panel. Not seen: touch on the 14 px cells after the new
  calibration, the frame rate with the mixer's meters moving, LIBRARY with
  files in it.
- Chromium will not make a window narrower than 500 px. The small layout is
  drawn in the left 480, so nothing is cut off; the start screen and Settings
  are still the TV pages.
- Done in v13.29: the firmware no longer draws when no screen answers.
- The A/B itself: both Seeds on one firmware version with the same things
  attached, then the T95's measurements repeated here (ping under load,
  bridge CPU, Wi-Fi ping with a page open, page speed, boot time, Bluetooth
  MIDI reconnect), ideally once more with the Seeds swapped.
- The link runs at 48 MHz, the overlay's default (`speed=` on the `dtoverlay` line in `config.txt`
  changes it). Faster was not tried.
- A kernel update: `install.sh` builds the driver for every kernel on the box and refreshes the
  boot image. Before that a reboot into a new kernel brought the old driver back (seen once).
- `install.sh` fetches `ili9486.c` for the running kernel line from GitHub;
  a kernel that changes that file enough stops `mkdriver.py` with a message.
