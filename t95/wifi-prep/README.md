# Recreate the T95 UWE5622 Wi-Fi image

These are the original helper and device-tree files from `t95-wifi-prep.zip`
used to prepare the successfully booted H616-T95MAX-V4.0 / AW859A image.
The Python helper, DTS, DTB, patch and DTB checksum are imported unchanged.
No SSID, Wi-Fi password or derived network key is included.

This adapts a pinned, prebuilt Armbian image. It does **not** rebuild Armbian,
the kernel or U-Boot from source, and it never writes a physical disk.
See [the T95 notes](../README.md) for hardware evidence and installation history.

## Files

| File | Purpose |
|---|---|
| `prepare.py` | Validate the base image, patch its EXT4 filesystem and create a separate image |
| `t95-uwe5622.dts` | Complete adapted device-tree source, decompiled from the base image and edited |
| `device-tree.patch` | Reviewable diff against the original decompiled T95 tree |
| `t95-uwe5622.dtb` | Compiled tree used for the successful hardware bring-up |
| `t95-uwe5622.sha256` | SHA-256 of that compiled tree |

The DT change adds `mmc-ddr-1_8v` to mmc1 and removes the XR819 `wifi@1`
child, its `ethernet1` alias and its `wlan` symbol. It preserves the remaining
T95 configuration. This is not the stock Android DTB.

## Base image

[Upstream release](https://github.com/rene/armbian-tvbox/releases/tag/t95-h616)

[Download the exact image](https://github.com/rene/armbian-tvbox/releases/download/t95-h616/Armbian-unofficial_26.11.0-trunk_T95-h616_trixie_current_6.18.63_minimal.img)

- Filename: `Armbian-unofficial_26.11.0-trunk_T95-h616_trixie_current_6.18.63_minimal.img`
- Size: **1,656,750,080 bytes**.
- SHA-256: `a2af7a34571a5dbf6b3f6f9ad093d5c9af94fe5e98af364bd05c8a354889efa7`.
- EXT4 partition begins at byte **4,194,304** and spans **1,652,555,776** bytes.

Download outside the checkout. For the commands below, rename the download to
`~/Downloads/t95-linux/t95-armbian.img`. The helper checks its size and SHA-256
and refuses another image. A future upstream build needs separate review.

## Prepare on macOS

Install dependencies:

```sh
brew install python e2fsprogs
```

From the GhettoRack repository root:

```sh
python3 t95/wifi-prep/prepare.py "$HOME/Downloads/t95-linux/t95-armbian.img"
```

The helper asks for your SSID and an 8-63 character ASCII WPA2-Personal
passphrase. Password entry is hidden; do not add credentials to scripts, shell
arguments, commits or issue reports. WPA3-only and enterprise networks are not
configured by this version.

Output: `~/Downloads/t95-linux/t95-uwe5622-wifi.img`. The helper refuses to
replace an existing output file. Reserve roughly 3.5 GB of additional free disk
space for temporary filesystem extraction and the output image. No `sudo` is
needed. It finds Homebrew e2fsprogs on Apple Silicon or Intel Macs.

On Linux, install Python 3 and e2fsprogs, ensure `debugfs` is on PATH (commonly
`/usr/sbin`), then run the same Python command with your own image path.
`e2fsck` must be alongside `debugfs`.

### What the helper writes

Only the extracted filesystem of the new image is changed:

- `/boot/dtb-6.18.63-current-sunxi64/allwinner/sun50i-h616-t95.dtb`
- `/etc/modules-load.d/t95-wifi.conf`, loading `sprdwl_ng`
- `/etc/netplan/30-t95-wifi.yaml`, configuring `wlan0` with networkd and DHCP

The network file uses JSON syntax, which is valid YAML, and mode 0600. It embeds
a locally derived WPA2 PSK, not the literal passphrase. **That key still grants
network access: the generated image is private.** The output is mode 0600;
temporary files are kept in a private temporary directory. Do not publish a
personalised image, even if the original password is not visible in it.

The helper verifies changed file bytes by reading them back, checks the network
file mode, runs `e2fsck -fn`, and preserves the base image's first 4 MiB,
including its partition table and bootloader. It prints the output SHA-256.
This recreates the adaptation, not a deterministic whole-image hash: network
settings and filesystem metadata can vary between runs.

The base already includes matching UWE5622 kernel modules and Linux firmware.
No stock Android modules, credentials or firmware dump are required by this
helper. Original Android firmware remains useful as a private recovery/reference
archive, but is not injected here.

## Rebuild the DTB from source

Normal image preparation uses the checked-in DTB, so `dtc` is optional.
For a byte-identical rebuild of that DTB use **device-tree-compiler 1.6.1**,
the version used to produce it. Later compilers may produce different bytes or
warnings; do not silently replace the checksum to bypass review.

From the repository root, with `dtc` on PATH:

```sh
workdir=$(mktemp -d)
dtc -I dts -O dtb \
  -o "$workdir/t95-uwe5622.dtb" t95/wifi-prep/t95-uwe5622.dts
cmp t95/wifi-prep/t95-uwe5622.dtb "$workdir/t95-uwe5622.dtb"
shasum -a 256 "$workdir/t95-uwe5622.dtb"
```

Expected SHA-256:

```text
e2b0fcf9cf8af747b7cb7982b0ab9531351d1aa476b16ef41b7e3ac7f3cc42b9
```

Warnings from decompiled vendor board descriptions may be emitted; retain them
for review. The import was checked by compiling with dtc 1.6.1 and comparing
the complete binary, not just by ignoring warnings.

To review the patch against the pinned base, extract its EXT4 partition to a
regular temporary file, use `debugfs` to dump the original DTB from the path
above, and decompile it with dtc 1.6.1 as `original-t95.dts`. Apply
`device-tree.patch` with `patch -o` to a separate file, then compare that source
with `t95-uwe5622.dts`. Do not apply this patch directly to the Linux kernel's
include-based DTS: its context is the decompiled tree from this exact image.

## Validation and limitations

- Imported payloads match the previously delivered ZIP byte-for-byte.
- Python syntax, the input-image rejection path and packaged DTB checksum were
  checked on import. dtc 1.6.1 reproduces the shipped DTB exactly; reverse/forward
  patch application reproduces the included source.
- The unchanged helper previously passed an end-to-end image preparation run
  with dummy credentials and `e2fsck -fn` on Linux, and was then run on the user's
  Mac. The resulting image was hash-verified after SD writing.
- That image booted on the user's box, provided Wi-Fi/SSH/HDMI, and was installed
  successfully to internal eMMC. This import does not imply a new hardware test
  or successful operation on other T95 board variants.
- The helper does not fix the observed first-login timezone/locale issue; see
  [the recovery notes](../README.md#first-login-locale-issue).
- A failed write may leave a partial output image. Do not flash it; inspect the
  error and remove only that known failed output before retrying.

This directory contains no disk-flashing command. Back up and re-identify the
actual SD target before writing. Keep a working SD recovery card before updating
the internal installation.

## Provenance and licensing

The helper originated in the 2026-11-21 T95 bring-up session and was distributed
with an MIT licence designation; `LICENSE.prepare` contains that grant for
`prepare.py` only. Device-tree material derives from the Linux/Armbian base
release; its upstream licensing applies, not the helper's MIT grant. Preserve
upstream attribution when redistributing derived board descriptions or images.
