#!/bin/sh
# Install or update the ApcBox telemetry bridge on the T95.
# Run from a clone of the repo:  sudo t95/bridge/install.sh
# Copies two files to /opt/apcbox-bridge, installs a udev rule and a systemd
# service, starts it. Touches no disks or partitions. Undo: --remove
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEST=/opt/apcbox-bridge

[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }

if [ "${1:-}" = "--remove" ]; then
  systemctl disable --now apcbox-bridge.service 2>/dev/null || true
  rm -f /etc/systemd/system/apcbox-bridge.service /etc/udev/rules.d/99-apcbox.rules
  rm -rf "$DEST"
  systemctl daemon-reload
  udevadm control --reload
  echo "removed"
  exit 0
fi

python3 -c 'import asyncio, termios' 2>/dev/null || { echo "needs python3: sudo apt install python3"; exit 1; }
# ffmpeg converts samples (MP3, FLAC, AIFF, 24-bit...) to what the Daisy plays
command -v ffmpeg >/dev/null || { apt-get update && apt-get install -y --no-install-recommends ffmpeg; } || \
  echo "note: no ffmpeg; samples other than WAV can't be sent"

mkdir -p "$DEST"
install -m 644 "$HERE/apcbox_bridge.py" "$DEST/apcbox_bridge.py"
install -m 644 "$REPO/daisy/ui/apcbox-ui.html" "$DEST/apcbox-ui.html"
install -m 644 "$REPO/t95/ui/selector.html" "$DEST/selector.html"
install -m 644 "$HERE/99-apcbox.rules" /etc/udev/rules.d/99-apcbox.rules
install -m 644 "$HERE/apcbox-bridge.service" /etc/systemd/system/apcbox-bridge.service

udevadm control --reload
udevadm trigger --action=add --subsystem-match=tty
udevadm trigger --action=add --subsystem-match=rc
udevadm trigger --action=add --subsystem-match=lirc
udevadm trigger --action=add --subsystem-match=usb --attr-match=idVendor=0483
udevadm trigger --action=add --subsystem-match=block --property-match=ID_FS_TYPE=vfat
udevadm trigger --action=add --subsystem-match=block --property-match=ID_FS_TYPE=exfat
systemctl daemon-reload
systemctl enable apcbox-bridge.service
systemctl restart apcbox-bridge.service
sleep 2
systemctl --no-pager --lines=5 status apcbox-bridge.service || true
IP=$(hostname -I 2>/dev/null | awk '{print $1}')
echo
echo "open http://${IP:-<t95-ip>}:8040/   health: curl -s localhost:8040/health"
