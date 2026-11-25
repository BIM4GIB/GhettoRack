#!/bin/sh
# GhettoRack on a Raspberry Pi 4: the SunFounder 3.5" IPS touch screen on the
# Pi's header (the same screen the Daisy drives in the T95 rig).
#   sudo pi4/screen/install.sh            install or update, then reboot
#   sudo pi4/screen/install.sh --remove   take it out again
# Builds the st7796sr kernel module (DKMS: a kernel update rebuilds it), puts
# an overlay and two lines into /boot/firmware/config.txt, a touch calibration
# into udev, and points the kiosk (t95/kiosk) at the screen.
# Fetches one file of the Raspberry Pi kernel's source from GitHub.
# Touches no disks or partitions.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
VER=1.1
SRC=/usr/src/st7796sr-$VER
CFG=/boot/firmware/config.txt
MARK="# GhettoRack: SunFounder 3.5 inch screen on the header (pi4/screen)"
DKMS=/usr/sbin/dkms

[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
grep -q "Raspberry Pi" /proc/device-tree/model 2>/dev/null || { echo "this is for a Raspberry Pi"; exit 1; }
[ -f "$CFG" ] || { echo "no $CFG"; exit 1; }

if [ "${1:-}" = "--remove" ]; then
  for old in 1.0 "$VER"; do
    [ -x "$DKMS" ] && $DKMS remove "st7796sr/$old" --all 2>/dev/null || true
    rm -rf "/usr/src/st7796sr-$old"
  done
  command -v update-initramfs >/dev/null && update-initramfs -u -k all
  rm -f /boot/firmware/overlays/ghettorack-screen.dtbo /etc/udev/rules.d/98-ghettorack-touch.rules /etc/udev/rules.d/97-ghettorack-nopointer.rules
  # the three lines this script added, nothing else
  sed -i "\|^$MARK\$|,+2d" "$CFG"
  grep -q "^WLR_DRM_DEVICES=.*spi" /etc/default/kiosk 2>/dev/null && rm -f /etc/default/kiosk
  udevadm control --reload
  echo "removed; reboot to let go of the screen"
  exit 0
fi

apt-get install -y --no-install-recommends dkms device-tree-compiler linux-headers-rpi-v8 curl ca-certificates

# The driver is the kernel's own ili9486.c (same shift-register bus) with the
# ST7796U's start-up commands: take the file for the kernel line that runs here.
KLINE=$(uname -r | sed 's/^\([0-9]*\.[0-9]*\).*/\1/')
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -fsSL -o "$TMP/ili9486.c" \
  "https://raw.githubusercontent.com/raspberrypi/linux/rpi-$KLINE.y/drivers/gpu/drm/tiny/ili9486.c" \
  || { echo "could not fetch ili9486.c for kernel $KLINE"; exit 1; }
python3 "$HERE/mkdriver.py" "$TMP/ili9486.c" "$TMP/st7796sr.c" \
  || { echo "the kernel's ili9486.c changed: mkdriver.py needs a look"; exit 1; }

for old in 1.0 "$VER"; do
  $DKMS remove "st7796sr/$old" --all >/dev/null 2>&1 || true
  rm -rf "/usr/src/st7796sr-$old"
done
mkdir -p "$SRC"
install -m 644 "$TMP/st7796sr.c" "$SRC/st7796sr.c"
printf 'obj-m += st7796sr.o\nall:\n\t$(MAKE) -C /lib/modules/$(KVER)/build M=$(CURDIR) modules\n' > "$SRC/Makefile"
cat > "$SRC/dkms.conf" <<EOF
PACKAGE_NAME="st7796sr"
PACKAGE_VERSION="$VER"
BUILT_MODULE_NAME[0]="st7796sr"
DEST_MODULE_LOCATION[0]="/extra"
MAKE[0]="make KVER=\$kernelver"
CLEAN="true"
AUTOINSTALL="yes"
EOF
$DKMS add "st7796sr/$VER"
# For every kernel on the box, not only the one running: the apt line above can
# bring a newer kernel, and the next boot is into that one.
for k in /lib/modules/*; do
  [ -e "$k/build" ] || continue
  $DKMS install "st7796sr/$VER" -k "$(basename "$k")" || echo "note: no st7796sr for kernel $(basename "$k")"
done
# The module is loaded from the boot image (initramfs): without this the next
# boot runs the copy from before.
if command -v update-initramfs >/dev/null; then update-initramfs -u -k all; fi

dtc -@ -I dts -O dtb -o /boot/firmware/overlays/ghettorack-screen.dtbo "$HERE/ghettorack-screen-overlay.dts" 2>/dev/null
[ -f "$CFG.before-screen" ] || cp "$CFG" "$CFG.before-screen"
grep -q "^dtoverlay=ghettorack-screen" "$CFG" || printf '\n%s\ndtparam=spi=on\ndtoverlay=ghettorack-screen\n' "$MARK" >> "$CFG"

# Touch calibration: a starting point from one screen. One made on this box
# (sudo python3 pi4/screen/calibrate.py) is kept.
[ -f /etc/udev/rules.d/98-ghettorack-touch.rules ] || install -m 644 "$HERE/98-ghettorack-touch.rules" /etc/udev/rules.d/98-ghettorack-touch.rules
# No arrow on the screen while no mouse is plugged in (see the rule's own comment)
install -m 644 "$HERE/97-ghettorack-nopointer.rules" /etc/udev/rules.d/97-ghettorack-nopointer.rules
udevadm control --reload
udevadm trigger --action=change --subsystem-match=input

# The kiosk's compositor: this screen only, drawn in software (the panel has
# no GPU behind it), opening the page's 480x320 layout. An /etc/default/kiosk
# someone else wrote is left alone.
if [ ! -f /etc/default/kiosk ] || grep -q "^WLR_DRM_DEVICES=.*spi" /etc/default/kiosk; then
  printf 'WLR_DRM_DEVICES=/dev/dri/by-path/platform-fe204000.spi-cs-0-card\nWLR_RENDERER=pixman\nCHROMIUM_FLAGS=\nKIOSK_URL=http://127.0.0.1:8040/?boot=1&small=1\n' > /etc/default/kiosk
else
  echo "note: /etc/default/kiosk exists and is not ours; see pi4/README.md for the two lines the screen needs"
fi

echo
echo "done. Reboot, then:  sudo python3 $HERE/testpattern.py   (with the kiosk stopped)"
