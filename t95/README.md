# T95 companion box

The T95 is GhettoRack's Linux companion to the Daisy Seed 3 running ApcBox.
The Daisy remains responsible for sequencing, voices, FX and mixing on its
audio clock, and must continue working standalone with the APC40 on its own
USB host port.

GhettoRack will start with a module selector: choose an instrument mode or
compatible combination, then show only its relevant UI. Numark DJ mode is one
option, not the definition of the whole project. Sampler, sequencer, synth,
quantisation and network clock roles are also in scope.

## Status as of 2026-11-22

**Linux is installed and booting from internal eMMC without the SD card.**
HDMI console, Wi-Fi, SSH and root filesystem expansion have been observed.
The post-install check reported zero failed systemd services.

The read-only telemetry bridge ([`bridge/`](bridge/README.md)) runs on the
T95 with a real Daisy (short run, see the table). Kiosk, controller
forwarding and sample transfer remain planned. Existing ApcBox firmware and its Mac UI are the starting point, not
features already deployed on the T95.

## Hardware and evidence

| Item | Current findings |
|---|---|
| Board | `H616-T95MAX-V4.0`, silkscreen date 2019-12-17 |
| SoC / RAM | Allwinner H616, four Cortex-A53 cores, 4 GB RAM; Linux reports about 3.8 GiB |
| Storage | 32 GB eMMC, Linux reports 29.1 GiB; the case's 64 GB label is inaccurate for this unit |
| Power management | AXP305 marking observed on the board |
| Wireless | AW859A / UWE5622 family; Wi-Fi association and SSH verified with `wlan0` |
| OS | Armbian-unofficial 26.11, Debian Trixie, `6.18.63-current-sunxi64`; PREEMPT reported, not a claim of PREEMPT_RT |
| USB | Two external sockets, referred to here as USB0 and USB1; other controller routing is not established |
| Debug UART | Pads labelled GND / TX / RX. Adapter loopback passed, but no board console was obtained. Root access came from Android `adb root`, not UART |
| Audio | User reports enumerated H616 analogue, SPDIF and HDMI cards; enumeration is not a playback/latency qualification |
| IR | User verified receiver with `ir-keytable`: extended NEC (`necx`), remote address `0x2fe` |
| Daisy link | Daisy on USB1: bus 5, full-speed OHCI, alone. CDC `ttyACM0` and UAC card `ApcBox`; `arecord` showed signal at 48 kHz stereo |
| Kiosk | 2026-11-22: cage + Chromium 156 on HDMI at 1920x1080, GPU compositing on the Mali-G31 (panfrost, Mesa 25.0.7) with `--ignore-gpu-blocklist --enable-gpu-rasterization` in `/etc/default/kiosk`. Selector shows; ApcBox page keeps up at 120 BPM after the page drew less (user: "better, snappier"). First start after boot is slow |
| HDMI audio | 2026-11-22: Daisy's USB audio passed to the TV with `alsaloop -C plughw:CARD=ApcBox -P plughw:CARD=HDMI -t 40000 -S 1` works (card `HDMI`, `ahub_plat-i2s-hifi`, no mixer routing needed; TV reports Sony in ELD). Adds ~40 ms plus the TV's own delay: for listening while testing, not for playing |
| Controllers via T95 | 2026-11-22, v13.6: APC40 and LPK25 on the USB0 hub, found by the bridge, identified by the Daisy (APC40 driving, LPK25 keys). APC40 LEDs lit through the T95; 635 APC40 and 29 LPK25 messages reached the Daisy, none dropped. **Sound not yet checked.** v13.5 dropped T95 controllers at once (unsigned time compare), fixed in v13.6 |
| Ping T95 to Daisy | v13.6, 291 pings: min 0.87, avg 3.1, p99 42.6, max 55.7 ms round trip. Cause found with the loop timing: the OLED's blocking I2C page sends (52 ms). v13.7 (OLED by DMA), 123 pings: min 0.78, avg 2.35, p99 8.8, max 32.8 ms; OLED 0.6 ms per pass and still updating; slowest loop part now the colour screen (7-9.5 ms). 76 pings counted lost around the flash and restart; none lost afterwards (30 s check: 30 answered, 0 more lost) |
| Bridge | 2026-11-22, v13.3 Daisy: bridge running under systemd, found the Daisy by serial, about 83 lines/s for 4.5 min with 0 bad lines; page live over Wi-Fi with APC40 events. Short run only: no long-run, load or latency figures yet |
| Network MIDI | 2026-11-23: Mac (Audio MIDI Setup) session with the T95 connected over Wi-Fi, one-way latency 3.85 ms by clock sync, 0 lost. The Mac started the session; the T95 inviting the Mac by itself not yet seen. MIDI through the hub to and from the Mac not yet checked. First install failed: avahi refused the service file (`--` in an XML comment), fixed |
| v13.9 | 2026-11-23: flashed from the T95; bridge reports control protocol 4, 82 pings: min 0.86, avg 3.15, max 52.1 ms (one spike, right after the restart), 0 lost. Page actions and the new page not yet tried on the rig |
| Listening in a browser | 2026-11-23: LISTEN in the new ApcBox page plays the Daisy's audio on the user's device ("sounds good"). Which devices, the delay, and whether the TV sound shared the capture (dsnoop) or fell back: not yet noted |
| Second socket | USB0: an LPK25 enumerated on bus 8 (OHCI). The USB 2.0 side of that socket is not yet confirmed |
| MIDI | User verified raw MIDI, e.g. an LPK25 at `/dev/snd/midiC3D0`. `CONFIG_SND_RAWMIDI=y`; ALSA sequencer support absent in this build |
| Power | 5 V DC input. Size the supply and powered hub for measured peripheral load; verify polarity and prevent backfeeding |

Use raw MIDI in the bridge. Do not require `aconnect` or `aseqdump` on this
kernel, and do not hardcode ALSA card numbers. Adding ALSA sequencer support
later is a separate kernel-build task, not a prerequisite for the initial bridge.
Bluetooth, sustained audio performance and live-gig reliability remain unverified.

## Linux image and board adaptation

Base release:
[rene/armbian-tvbox, t95-h616](https://github.com/rene/armbian-tvbox/releases/tag/t95-h616)

- Filename: `Armbian-unofficial_26.11.0-trunk_T95-h616_trixie_current_6.18.63_minimal.img`
- Size: 1,656,750,080 bytes.
- Base SHA-256: `a2af7a34571a5dbf6b3f6f9ad093d5c9af94fe5e98af364bd05c8a354889efa7`.

This is an unofficial board-specific build. The base release targets a different
Wi-Fi variant; the tested unit required an AW859A/UWE5622 adaptation:

1. Adapt `allwinner/sun50i-h616-t95.dtb`: remove the XR819 `wifi@1` child and
   its alias/symbol references; add mmc1 `mmc-ddr-1_8v`, matching the included
   Orange Pi Zero2 configuration. Retain the rest of the T95 board configuration.
2. Load `sprdwl_ng` through `/etc/modules-load.d/t95-wifi.conf`.
3. Configure `wlan0` using Netplan and systemd-networkd with local Wi-Fi
   credentials and DHCP.

Matching UWE5622 kernel modules and Linux firmware were already in the base
image. Its board INI matched the extracted Android INI. The Linux
`wcnmodem.bin` differed from Android's and was retained for the successful boot.
Do not copy Android 4.9.170 kernel modules into Linux 6.18.

The helper from the tested `t95-wifi-prep.zip` is committed under
[`wifi-prep/`](wifi-prep/README.md), together with the DTB source, reviewed patch,
compiled DTB and checksum. Follow that README to recreate the adaptation from
the pinned upstream image. This is an image-customisation workflow, not a full
Armbian/kernel build from source. Wi-Fi credentials are entered locally at run
time; personalised images contain a usable Wi-Fi key and must stay private.

Kernel/DTB upgrades can replace these board-specific changes. Preserve the
working configuration and recovery card, and validate upgrades on SD before
changing the internal installation.

## Installation record and recovery

The original Android eMMC was backed up before installation:

| Backup | Exact length |
|---|---:|
| eMMC user area | 31,268,536,320 bytes |
| eMMC boot0 | 4,194,304 bytes |
| eMMC boot1 | 4,194,304 bytes |

Stock device tree and Wi-Fi files were also extracted. These are private
backups, not repository assets. Matching lengths and recorded hashes do not
constitute a tested restore; the original dump was made while Android ran.

The adapted image was written to a 128 GB microSD and its written bytes were
hash-verified. SD boot, HDMI and Wi-Fi worked. After a clean reboot test,
`sudo armbian-install` installed to eMMC using ext4 without encryption.
Power-off, SD removal and a fresh boot confirmed:

- Root filesystem: `/dev/mmcblk2p1`.
- Only the 29.1 GiB eMMC, its boot areas and zram were listed.
- `systemctl --failed` reported no failed units.

At installation time, the SD was `mmcblk0` and eMMC was `mmcblk2`. **Always
re-identify them on the target before writing.** Do not clone the larger SD
device blindly onto the smaller eMMC.

Keep the working SD as recovery media. This platform normally tries a bootable
SD before eMMC; leave it out for normal internal-storage boot. Bootloader/FEL
recovery through the button labelled UBOOT has not been validated here.
Disconnect the Mac-to-T95 USB debug cable before Linux boots into USB host mode.

Useful read-only checks:

```sh
uname -a
findmnt -no SOURCE /
lsblk -o NAME,SIZE,FSTYPE,MOUNTPOINTS
systemctl --failed --no-pager
ip -br addr
```

### First-login locale issue

Automatic location detection returned timezone `null`. Setup then spent over
an hour generating unrelated locales; SSH remained usable. The excessive
locale generation was stopped and `/etc/locale.gen` restricted to:

```text
en_GB.UTF-8 UTF-8
```

British English was generated successfully, and these commands established the
system defaults:

```sh
sudo locale-gen
sudo update-locale --reset LANG=en_GB.UTF-8
sudo timedatectl set-timezone Europe/London
cat /etc/locale.conf
timedatectl
```

On this Trixie build, `update-locale` writes `/etc/locale.conf`.
The old `/etc/default/locale` contained blank values and was misleading.
Clock synchronisation and a subsequent reboot were verified. These are recovery
notes for the observed first-login issue, not commands to rerun at every boot.

## Daisy connection and USB topology

See [serial protocol](../protocol/serial.md) and
[existing UI](../daisy/ui/apcbox-ui.html).

The v13.x ApcBox baseline presents a USB full-speed composite device:
VID `1209`, PID `0001`, product `ApcBox`, serial derived from STM32 unique ID.

- CDC-ACM serial, normally `/dev/ttyACM*`; baud setting is ignored.
- UAC1 stereo 48 kHz / 16-bit audio **from Daisy to host**. The asynchronous
  stream follows the Daisy's audio clock.
- No USB-MIDI class and no host-to-Daisy audio stream yet.
- CDC host commands currently consist only of `ESC R` (full state dump) and
  `ESC BOOT` (enter bootloader), from v13.3. Older firmware acts on any bare
  `R`/`B`: on 2026-11-22 an echoing `stty`/`cat` sent a v13.2 Daisy into its
  bootloader. Musical control and sample transfer need new firmware.

Planned wiring:

| T95 port | Connection |
|---|---|
| USB1 | Daisy USB-C, on a dedicated host controller |
| USB0 | Powered USB hub for APC40, MPK249, LPK25 and other controllers |

Verify actual topology with `lsusb -t` on the assembled rig. Never join the
Daisy's USB-A host socket to the T95 host socket using an ordinary A-to-A cable.
The future second device lane on Daisy pins 36/37 conflicts with using that
same port for the standalone APC40 host; role selection needs an explicit design.

## Implementation sequence

1. **Read-only telemetry bridge.** Done, running on the T95 (see `bridge/`). Own the serial connection, request `ESC R`,
   assemble complete report lines, and serve them over SSE at `/events`.
   Preserve the existing parser and direct Web Serial path. Support reconnect,
   multiple browsers and bounded buffering. Show Daisy link health separately
   from browser-to-bridge connectivity.
2. **Versioned MIDI/control transport.** Built (v13.5/v13.6 and the bridge);
   APC40 and LPK25 work through the T95 (LEDs, messages). Open: sound check,
   the 40-55 ms ping spikes. Add framed MIDI input routed into the
   existing Daisy handler and explicit outbound MIDI for controller LEDs.
   Identify controller sources, define ownership and disconnect behaviour, and
   prevent payload bytes from triggering legacy `R`/`B` commands.
3. **Module selector, kiosk and IR.** 3a written and desktop-tested, not yet
   on the T95: selector at `/`, kiosk (`kiosk/`), remote learn/profiles in
   the bridge. 3b built (v13.7): the remote and the page drive the Daisy
   (encoder, play/stop, tracks) via `ESC U`; the OLED no longer blocks the
   Daisy's main loop (52 ms per page before). Not yet on the rig.
   Focused views for the selected modules;
   Chromium on HDMI and browser access over Wi-Fi. IR profiles per view,
   tap/hold/repeat handling and a persisted learn mode.
4. **Sample transfer.** Built (v13.8, bridge): library on the T95 (eMMC,
   SD card import, uploads, recording from the page over HTTPS :8443), sent
   into a track over USB. On the rig 2026-11-22: v13.8 flashed, recording
   from the phone works (user). Transfer speed on the rig not yet noted. Chunked, checked, flow-controlled transfer into memory
   actually available on the Daisy. Its 64 MB SDRAM is shared with existing
   slots and staging buffers. Query capacity; measure throughput while audio
   and MIDI run. No guaranteed transfer rate has been established.
5. **Modules: MIDI hub.** Built (bridge only, no flash), desktop-tested, not
   yet on the rig. Each mode in the selector is a module with its own page,
   needs and default routes; one active at a time, data model ready for
   chaining later. A router in the bridge sends any MIDI device to any other
   (channel filter/remap, message types, presets). ApcBox is the module whose
   default is "everything to the Daisy"; in the MIDI hub the APC40 is just a
   MIDI device and the Daisy is optional. Network MIDI (RTP-MIDI/AppleMIDI,
   udp 5004/5005): announced over Avahi, every session found is invited
   automatically, per-device "never". Interop checked against pymidi's
   server (an independent implementation) on the desktop; not yet against a
   Mac, iPad or a hardware box.
6. **Clock module.** Built (bridge), desktop-tested, not yet on the rig:
   internal tempo, Link (aalink), or following MIDI clock (Cubase leads; it
   can't follow or do Link), MIDI clock out to any ports. **Cubase sync**
   mode pinned: a VST3 plugin in Cubase reads its transport and shares it on
   Link (planned). Daisy following the clock: firmware, later.
7. **Later extensions.** T95-to-Daisy UAC audio OUT, optional second USB device
   lane, DIN MIDI clock from a Daisy UART, network jam clock leader/follower
   modes, and end-to-end latency tests.

The existing HTTP UI uses `EventSource("/events")`, not WebSocket.
Its Mac-build `/ui` controls and `/tft.bmp` polling must be disabled or
capability-gated for the initial Daisy bridge. Serial `D` reports supply the
OLED mirror, not the TFT framebuffer. The parser also understands `BF`,
`BN` and `KN`; retain these even where the protocol table is incomplete.
LED telemetry `W` is not itself an implemented bidirectional MIDI transport.

## Timing and validation

Keep synthesis and sample-accurate work on the Daisy. The bridge may use a
dedicated core and real-time priority for bounded MIDI forwarding after
measurement; web serving, logging and sample I/O stay outside that work.
PREEMPT alone does not guarantee a latency budget.

Network/MIDI clock leader and follower modes remain a design goal. External
tempo/phase must be translated into the Daisy scheduler; do not drive sequencer
steps directly from browser timers or conflate MIDI clock with USB audio clocks.

Use the Daisy output as the initial monitoring path. Qualify other outputs with
measurements rather than assuming a fixed HDMI delay. Before calling this
gig-ready, test audio/MIDI latency and jitter under load, hotplug/reconnection,
power stability, thermal behaviour and recovery after interrupted sessions.
