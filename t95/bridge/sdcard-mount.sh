#!/bin/sh
# Called by udev (99-apcbox.rules) for a FAT / exFAT partition of a card in the box's SD slot:
# mounts it read-only at /media/sdcard for the sample library. Not the card the box itself
# runs from: a Raspberry Pi boots from its slot, and its boot partition is FAT too.
dev=$1
[ -b "$dev" ] || exit 0
disk=${dev%p[0-9]*}
for m in / /boot /boot/firmware /boot/efi; do
  src=$(/usr/bin/findmnt -no SOURCE "$m" 2>/dev/null) || continue
  case "$src" in "$disk"|"$disk"p[0-9]*) exit 0 ;; esac
done
exec /usr/bin/systemd-mount --no-block --collect --options=ro,nosuid,nodev,noexec,umask=022 "$dev" /media/sdcard
