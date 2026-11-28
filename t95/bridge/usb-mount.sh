#!/bin/sh
# Called by udev (99-apcbox.rules) for a partition of a USB stick or disk plugged into the box:
# mounts it read-only at /media/usb, for the Numark DJ module's music (played straight off it,
# or copied onto the box). Read-only: nothing on the box writes to somebody's stick, so it can
# be pulled at any time. Not the disk the box itself runs from (a Pi can boot from USB).
# One at a time: with something at /media/usb already, a second stick is left alone.
dev=$1
[ -b "$dev" ] || exit 0
disk=$(/usr/bin/lsblk -no PKNAME "$dev" 2>/dev/null | head -n 1)
for m in / /boot /boot/firmware /boot/efi; do
  src=$(/usr/bin/findmnt -no SOURCE "$m" 2>/dev/null) || continue
  [ "$src" = "$dev" ] && exit 0
  [ -n "$disk" ] && [ "$(/usr/bin/lsblk -no PKNAME "$src" 2>/dev/null | head -n 1)" = "$disk" ] && exit 0
done
/usr/bin/findmnt -n /media/usb >/dev/null 2>&1 && exit 0
opts=ro,nosuid,nodev,noexec
case "$(/usr/sbin/blkid -o value -s TYPE "$dev" 2>/dev/null || /sbin/blkid -o value -s TYPE "$dev" 2>/dev/null)" in
  vfat|exfat|ntfs|ntfs3) opts=$opts,umask=022 ;;
esac
exec /usr/bin/systemd-mount --no-block --collect --options="$opts" "$dev" /media/usb
