#!/bin/sh
# Attaches the T95's UWE5622 Bluetooth to the kernel: what worked by hand on
# the rig (t95/README.md). Run once per boot by ghettorack-bluetooth.service.
#
# The attach tool must never be stopped or started twice: on this driver a
# killed one hangs in the kernel ("[hciattach_opi]", SIGKILL does nothing), a
# second one then says "Initialization timed out", and only a reboot clears
# it (rig, 2026-11-23). So: one attach, no restart, nothing here kills it.
# Off and on afterwards is the radio's block switch (Settings does that).
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
