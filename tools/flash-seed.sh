#!/bin/bash
# ApcBox: flash ONE chosen Seed when two are plugged in.
#
#   tools/flash-seed.sh                 asks which serial port (remembers the last one)
#   tools/flash-seed.sh /dev/cu.usbmodem1234      (Linux: /dev/ttyACM0)
#   tools/flash-seed.sh --new           the Seed is already in the bootloader (LED pulsing),
#                                             nothing is sent to any port
#   add a .bin path as the last argument to flash something else
#
# Sends the "go to the bootloader" byte only to the port you pick, then
# flashes only if exactly one Seed is in update mode, by its serial number.
# Close the monitor page's connection to that Seed first (it holds the port).

REPO="$(cd "$(dirname "$0")/.." && pwd)"
APP="$REPO/daisy"
SAVED="$HOME/.apcbox-new-seed-port"

say() { printf '  %s\n' "$*"; }
fail() { printf '\n\033[31mSTOPPED: %s\033[0m\n' "$*"; exit 1; }

dfu_list() {
  dfu-util -l 2>/dev/null | grep '0483:df11' | while read -r line; do
    ser=$(printf '%s' "$line" | sed -n 's/.*serial="\([^"]*\)".*/\1/p')
    case "$line" in
      *"@Internal Flash"*) echo "$ser rom" ;;
      *"@Flash"*) echo "$ser daisy" ;;
    esac
  done | sort -u
}

PORT=""
BIN="$APP/build/ApcBox.bin"            # your own build, if there is one
[ -f "$BIN" ] || BIN="$APP/release/ApcBox.bin"
case "$(uname)" in Linux) PORTGLOB="/dev/ttyACM*"; STTY="stty -F" ;; *) PORTGLOB="/dev/cu.usbmodem*"; STTY="stty -f" ;; esac
for a in "$@"; do
  case "$a" in
    --new) PORT=none ;;
    /dev/*) PORT=$a ;;
    *.bin) BIN=$a ;;
  esac
done
command -v dfu-util >/dev/null || fail "dfu-util isn't installed (brew install dfu-util)."
[ -f "$BIN" ] || fail "no $BIN"

[ -n "$(dfu_list)" ] && [ "$PORT" != none ] && {
  dfu_list | sed 's/^/    /'
  fail "a Seed is already in update mode. If it's the one to flash, run with --new. Otherwise reset it first."
}

if [ -z "$PORT" ]; then
  LIST=$(ls $PORTGLOB 2>/dev/null)
  [ -n "$LIST" ] || fail "no Seed serial ports. Plug it in, or use --new if it's in the bootloader."
  LAST=$(cat "$SAVED" 2>/dev/null)
  echo "Which Seed? (your OTHER Seed is one of these too)"
  n=0
  for p in $LIST; do
    n=$((n + 1))
    mark=""; [ "$p" = "$LAST" ] && mark="   <- last time"
    echo "  $n) $p$mark"
  done
  printf '  number: '; read -r pick
  PORT=$(printf '%s\n' $LIST | sed -n "${pick}p")
  [ -n "$PORT" ] || fail "no such number."
fi

if [ "$PORT" != none ]; then
  [ -e "$PORT" ] || fail "$PORT isn't there."
  echo "$PORT" > "$SAVED"
  say "asking the Seed on $PORT to go to its bootloader..."
  $STTY "$PORT" 115200 2>/dev/null
  printf B > "$PORT" 2>/dev/null || fail "can't write to $PORT. Close the monitor page's connection to it."
fi

echo "  waiting for exactly one Seed in the bootloader..."
SER=""; i=0
while [ $i -lt 60 ]; do
  list=$(dfu_list)
  n=$(printf '%s' "$list" | grep -c .)
  [ "$n" -gt 1 ] && { echo "$list"; fail "two Seeds in update mode. Reset the one you don't mean, run again."; }
  if [ "$n" -eq 1 ]; then
    case "$list" in
      *" daisy") SER=${list%% *}; break ;;
      *" rom") fail "that Seed is in the chip's own mode (no Daisy bootloader). Run seed2-setup.sh instead." ;;
    esac
  fi
  sleep 0.5; i=$((i + 1))
done
[ -n "$SER" ] || fail "no bootloader showed up. Tap RESET, press BOOT while the LED pulses, then run with --new."

say "flashing $(basename "$BIN") to serial $SER"
dfu-util -a 0 -s 0x90040000:leave -D "$BIN" -d ,0483:df11 -S "$SER"
echo
say "done (Error 74 after 'File downloaded successfully' is normal)."
