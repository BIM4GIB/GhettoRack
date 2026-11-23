#!/bin/sh
# Bluetooth on the T95 (UWE5622 chip) needs two things that aren't in the repo:
# the normal Bluetooth stack (bluez, from apt) and the chip's attach tool,
# hciattach_opi: a closed binary from the chip's vendor, the one Armbian ships
# for Orange Pi boards with this chip. This fetches that one file from Armbian's
# build repository, checks it is the file that was tried on the rig, and puts
# it in /usr/local/bin. It starts nothing: Bluetooth is turned on in Settings.
#   sudo t95/helper/get-bt-tool.sh        undo: sudo rm /usr/local/bin/hciattach_opi
set -eu
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
URL=https://raw.githubusercontent.com/armbian/build/main/packages/blobs/bt/hciattach/hciattach_opi_arm64
SUM=d0dbdabbadd181155a151c1f8e9aba19c3727e6243e841216f238b9bacf9111d
DEST=/usr/local/bin/hciattach_opi
command -v bluetoothctl >/dev/null || apt-get install -y --no-install-recommends bluez
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT
curl -fL -o "$TMP" "$URL"
GOT=$(sha256sum "$TMP" | cut -d' ' -f1)
if [ "$GOT" != "$SUM" ]; then
  echo "not the file that was tested (sha256 $GOT): nothing installed."
  echo "Armbian may have updated it; check t95/helper/README.md before trusting a new one."
  exit 1
fi
install -m 755 "$TMP" "$DEST"
echo "installed $DEST. Turn Bluetooth on in Settings."
