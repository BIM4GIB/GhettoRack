#!/bin/sh
# Attaches the T95's UWE5622 Bluetooth to the kernel: what worked by hand on
# the rig (t95/README.md). Run once per boot by ghettorack-bluetooth.service.
#
# The attach tool must not be killed just like that: on this driver one killed
# while the controller is up or still starting hangs in the kernel
# ("[hciattach_opi]", SIGKILL does nothing), a second one then says
# "Initialization timed out", and only a reboot clears it; it also stalls the
# shutdown for minutes (rig, 2026-11-23). Stopped in order it exits cleanly
# (rig, same day, once: controller off, Bluetooth stack stopped, then SIGTERM;
# the kernel logs "mtty_close device success"). "stop" below does exactly that,
# and the unit runs it at shutdown. Off and on in Settings is still the radio's
# block switch, not this.
if [ "$1" = stop ]; then
  pid=$(pgrep -x hciattach_opi | head -1)
  [ -n "$pid" ] || exit 0
  if ! ls /sys/class/bluetooth/hci* >/dev/null 2>&1; then
    echo "attach tool has no controller (starting or stuck): not touching it" >&2
    exit 0
  fi
  # controller off: through the stack if it's running, else the radio block
  if ! timeout 6 bluetoothctl power off >/dev/null 2>&1; then
    for r in /sys/class/rfkill/rfkill*; do
      [ "$(cat "$r/type" 2>/dev/null)" = bluetooth ] && echo 1 > "$r/soft"
    done
  fi
  systemctl stop bluetooth.service 2>/dev/null
  sleep 2
  kill -TERM "$pid" 2>/dev/null
  i=0
  while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
  kill -0 "$pid" 2>/dev/null && echo "attach tool didn't exit after SIGTERM: leaving it (never SIGKILL)" >&2
  exit 0
fi
if pgrep -x hciattach_opi >/dev/null 2>&1 || ls /sys/class/bluetooth/hci* >/dev/null 2>&1; then
  echo "already attached (or an attach tool is still around): leaving it alone" >&2
  exit 0
fi
/sbin/modprobe sprdbt_tty || { echo "no sprdbt_tty module for this kernel" >&2; exit 1; }
i=0
while [ ! -e /dev/ttyBT0 ] && [ "$i" -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
[ -e /dev/ttyBT0 ] || { echo "the module loaded but /dev/ttyBT0 didn't appear" >&2; exit 1; }
# once the chip has attached: lift the radio block and start the Bluetooth stack
(
  i=0
  while ! ls /sys/class/bluetooth/hci* >/dev/null 2>&1 && [ "$i" -lt 40 ]; do sleep 0.5; i=$((i + 1)); done
  ls /sys/class/bluetooth/hci* >/dev/null 2>&1 || exit 0
  for r in /sys/class/rfkill/rfkill*; do
    [ "$(cat "$r/type" 2>/dev/null)" = bluetooth ] && echo 0 > "$r/soft"
  done
  systemctl start --no-block bluetooth.service
) &
exec /usr/local/bin/hciattach_opi -n -s 1500000 /dev/ttyBT0 sprd
