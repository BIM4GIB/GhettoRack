#!/bin/sh
# Starts Bluetooth on the T95's UWE5622: what worked by hand on the rig
# (t95/README.md). Run by ghettorack-bluetooth.service, which Settings turns
# on and off. Stays in the foreground as the attach tool.
/sbin/modprobe sprdbt_tty || { echo "no sprdbt_tty module for this kernel"; exit 1; }
# an attach tool left over from a start by hand holds the chip: one at a time
if pkill -x hciattach_opi 2>/dev/null; then sleep 1; fi
i=0
while [ ! -e /dev/ttyBT0 ] && [ "$i" -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
[ -e /dev/ttyBT0 ] || { echo "the module loaded but /dev/ttyBT0 didn't appear"; exit 1; }
# once the chip has attached: lift the radio block and start the Bluetooth stack
(
  sleep 8
  for r in /sys/class/rfkill/rfkill*; do
    [ "$(cat "$r/type" 2>/dev/null)" = bluetooth ] && echo 0 > "$r/soft"
  done
  systemctl start bluetooth.service
) &
exec /usr/local/bin/hciattach_opi -n -s 1500000 /dev/ttyBT0 sprd
