#!/bin/sh
# GhettoRack kiosk on the T95: Chromium full screen on HDMI at boot, showing
# the module selector from the bridge (install the bridge first).
#   sudo t95/kiosk/install.sh           install or update
#   sudo t95/kiosk/install.sh --remove  back to the text console on HDMI
# Installs packages (cage, chromium, fonts), adds a "kiosk" user, a systemd
# service on tty1, and sets the default target to graphical. Touches no disks.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }

if [ "${1:-}" = "--remove" ]; then
  systemctl disable --now kiosk.service 2>/dev/null || true
  rm -f /etc/systemd/system/kiosk.service
  systemctl daemon-reload
  systemctl set-default multi-user.target
  systemctl start getty@tty1.service || true
  echo "removed; the kiosk user and packages stay (apt remove cage chromium to drop them)"
  exit 0
fi

[ -f /opt/apcbox-bridge/apcbox_bridge.py ] || echo "note: the bridge isn't installed yet: sudo t95/bridge/install.sh"

apt-get update
apt-get install -y --no-install-recommends cage chromium fonts-dejavu-core wget

id kiosk >/dev/null 2>&1 || useradd --system --create-home --home-dir /var/lib/kiosk --shell /usr/sbin/nologin kiosk
# Screen, GPU, input for the compositor
for g in video render input audio; do
  getent group "$g" >/dev/null && usermod -aG "$g" kiosk
done
mkdir -p /var/lib/kiosk/chromium
chown -R kiosk:kiosk /var/lib/kiosk

install -m 644 "$HERE/kiosk.service" /etc/systemd/system/kiosk.service
systemctl daemon-reload
systemctl set-default graphical.target
systemctl enable kiosk.service
systemctl restart kiosk.service
sleep 3
systemctl --no-pager --lines=8 status kiosk.service || true
echo
echo "HDMI should show the GhettoRack selector. Logs: journalctl -u kiosk -f"
echo "Back to the text console: sudo t95/kiosk/install.sh --remove"
