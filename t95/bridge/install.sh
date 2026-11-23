#!/bin/sh
# Install or update the ApcBox telemetry bridge on the T95.
# Run from a clone of the repo:  sudo t95/bridge/install.sh
# Copies the bridge and its pages to /opt/apcbox-bridge, installs a udev rule,
# a systemd service and an avahi (Bonjour) service for network MIDI, starts it. Touches no disks or partitions. Undo: --remove
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEST=/opt/apcbox-bridge

[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }

if [ "${1:-}" = "--remove" ]; then
  systemctl disable --now apcbox-bridge.service 2>/dev/null || true
  rm -f /etc/systemd/system/apcbox-bridge.service /etc/udev/rules.d/99-apcbox.rules
  rm -f /etc/avahi/services/ghettorack-midi.service
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
# avahi announces the T95 for network MIDI and finds other devices (Bonjour)
command -v avahi-browse >/dev/null && [ -d /etc/avahi/services ] || \
  { apt-get update && apt-get install -y --no-install-recommends avahi-daemon avahi-utils; } || \
  echo "note: no avahi; network MIDI devices must connect to the T95 themselves"

mkdir -p "$DEST"
install -m 644 "$HERE/apcbox_bridge.py" "$DEST/apcbox_bridge.py"
rm -rf "$DEST/rackbridge"
install -d "$DEST/rackbridge"
install -m 644 "$HERE"/rackbridge/*.py "$DEST/rackbridge/"
install -m 644 "$REPO/daisy/ui/apcbox-ui.html" "$DEST/apcbox-ui.html"
install -m 644 "$REPO/t95/ui/selector.html" "$DEST/selector.html"
install -m 644 "$REPO/t95/ui/midihub.html" "$DEST/midihub.html"
install -m 644 "$REPO/t95/ui/clock.html" "$DEST/clock.html"
install -m 644 "$REPO/t95/ui/apcbox.html" "$DEST/apcbox.html"
# Ableton Link for the clock module (optional): a venv that also sees the system
# packages, with aalink (a wheel of Ableton's Link library). Without it Link is off.
if [ ! -x "$DEST/venv/bin/python3" ]; then
  python3 -m venv --system-site-packages "$DEST/venv" 2>/dev/null || \
    { apt-get install -y --no-install-recommends python3-venv && python3 -m venv --system-site-packages "$DEST/venv"; } || true
fi
"$DEST/venv/bin/pip" install -q --upgrade "aalink==0.2.3" || echo "note: no aalink; the clock works without Link"
install -m 644 "$HERE/99-apcbox.rules" /etc/udev/rules.d/99-apcbox.rules
install -m 644 "$HERE/apcbox-bridge.service" /etc/systemd/system/apcbox-bridge.service
# no venv: run with the system Python (no Link)
[ -x "$DEST/venv/bin/python3" ] || sed -i 's#/opt/apcbox-bridge/venv/bin/python3#/usr/bin/python3#' /etc/systemd/system/apcbox-bridge.service
if [ -d /etc/avahi/services ]; then
  install -m 644 "$HERE/ghettorack-midi.service" /etc/avahi/services/ghettorack-midi.service
  systemctl enable --now avahi-daemon.service 2>/dev/null || true
fi

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
