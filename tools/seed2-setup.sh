#!/bin/bash
# GhettoRack: set up a fresh Daisy Seed (bootloader + ApcBox firmware).
#
#   tools/seed2-setup.sh            flashes daisy/build/ApcBox.bin if you built one,
#                                   otherwise daisy/release/ApcBox.bin
#
# Safe with another Seed plugged in and running:
#   - it never sends anything to a serial port (no "B" reboot trick)
#   - every flash needs exactly ONE Seed in USB update mode, and targets it
#     by its serial number, so the running Seed can't be hit by mistake
# Works on macOS and on Linux (the T95).

REPO="$(cd "$(dirname "$0")/.." && pwd)"
APP="$REPO/daisy"
BIN="$APP/build/ApcBox.bin"
[ -f "$BIN" ] || BIN="$APP/release/ApcBox.bin"
BOOTBIN="$REPO/lib/libDaisy/core/dsy_bootloader_v6_4-intdfu-2000ms.bin"
case "$(uname)" in Linux) PORTGLOB="/dev/ttyACM*" ;; *) PORTGLOB="/dev/cu.usbmodem*" ;; esac

bold() { printf '\n\033[1m%s\033[0m\n' "$*"; }
say() { printf '  %s\n' "$*"; }
fail() { printf '\n\033[31mSTOPPED: %s\033[0m\n' "$*"; exit 1; }
pause() { printf '\n  %s ' "${1:-Press Enter to go on.}"; read -r _; }
ask_yes() { printf '\n  %s [y/N] ' "$1"; read -r a; [ "$a" = y ] || [ "$a" = Y ]; }

# The Seeds in USB update (DFU) mode, one line each: "<serial> <kind>"
# kind = rom (chip's own loader, BOOT held at reset) or daisy (Daisy bootloader)
dfu_list() {
  dfu-util -l 2>/dev/null | grep '0483:df11' | while read -r line; do
    ser=$(printf '%s' "$line" | sed -n 's/.*serial="\([^"]*\)".*/\1/p')
    case "$line" in
      *"@Internal Flash"*) echo "$ser rom" ;;
      *"@Flash"*) echo "$ser daisy" ;;
    esac
  done | sort -u
}

# Waits (up to $2 seconds) for exactly one Seed in mode $1; sets SER
wait_one() {
  want=$1; secs=${2:-60}; i=0
  while [ $i -lt $((secs * 2)) ]; do
    list=$(dfu_list)
    n=$(printf '%s' "$list" | grep -c . )
    if [ "$n" -gt 1 ]; then
      echo; echo "$list"
      fail "more than one Seed is in update mode. Unplug or reset the one you're not setting up, then run this again."
    fi
    if [ "$n" -eq 1 ]; then
      kind=$(printf '%s' "$list" | awk '{print $2}')
      SER=$(printf '%s' "$list" | awk '{print $1}')
      [ "$kind" = "$want" ] && return 0
    fi
    sleep 0.5; i=$((i + 1))
  done
  return 1
}

ports() { ls $PORTGLOB 2>/dev/null; }

# ---------------------------------------------------------------- 0
bold "0. Checks"
command -v dfu-util >/dev/null || fail "dfu-util isn't installed (brew install dfu-util)."
[ -f "$BIN" ] || fail "no firmware: build it (cd daisy && make) or check daisy/release/"
[ -f "$BOOTBIN" ] || fail "no bootloader file: run git submodule update --init --recursive"
say "firmware:   $BIN"
say "bootloader: $BOOTBIN"
PORTS_BEFORE=$(ports)
say "serial ports now (your running Seed is one of these):"
for p in $PORTS_BEFORE; do say "  $p"; done
[ -z "$PORTS_BEFORE" ] && say "  (none)"
if [ -n "$(dfu_list)" ]; then
  say "A Seed is already in update mode:"; dfu_list | sed 's/^/    /'
  say "That's fine if it's the NEW one. If it's your running Seed, stop here (Ctrl-C)."
fi
pause "Only the NEW Seed will be flashed. Press Enter to start."

# ---------------------------------------------------------------- 1
bold "1. Daisy bootloader on the NEW Seed"
say "On the NEW Seed (USB-C to the Mac):"
say "  hold BOOT, press RESET, release RESET, release BOOT."
say "Leave the other Seed alone."
say "Skip this step if the new Seed already has the Daisy bootloader:"
say "press Ctrl-C now and run:  tools/flash-seed.sh --new"
pause "Do the buttons, then press Enter."
echo "  waiting for the new Seed in the chip's update mode..."
wait_one rom 60 || fail "didn't see it. Check the USB-C cable carries data, and try the buttons again."
say "found it: serial $SER"
dfu-util -a 0 -s 0x08000000:leave -D "$BOOTBIN" -d ,0483:df11 -S "$SER"
echo
say "bootloader written (an error right at the end, after 'File downloaded successfully', is normal)."

# ---------------------------------------------------------------- 2
bold "2. ApcBox firmware on the NEW Seed"
say "Unplug the NEW Seed's USB-C and plug it back in."
say "Its LED pulses: that's the Daisy bootloader waiting."
say "If it stops pulsing before this finds it, tap RESET and press BOOT while the LED pulses."
pause "Replug it, then press Enter."
echo "  waiting for the Daisy bootloader..."
wait_one daisy 60 || fail "didn't see the bootloader. Tap RESET, press BOOT while the LED pulses, then run: tools/flash-seed.sh --new"
say "found it: serial $SER"
dfu-util -a 0 -s 0x90040000:leave -D "$BIN" -d ,0483:df11 -S "$SER"
echo
say "firmware written (Error 74 at the end is normal)."

# ---------------------------------------------------------------- 3
bold "3. Check"
echo "  waiting for the new Seed's serial port..."
NEWPORT=""
i=0
while [ $i -lt 30 ] && [ -z "$NEWPORT" ]; do
  for p in $(ports); do
    printf '%s\n' "$PORTS_BEFORE" | grep -qx "$p" || NEWPORT=$p
  done
  sleep 0.5; i=$((i + 1))
done
if [ -n "$NEWPORT" ]; then
  say "new Seed is on $NEWPORT"
  echo "$NEWPORT" > "$HOME/.apcbox-new-seed-port"
  say "(saved, so flash-seed.sh can find it next time)"
else
  say "no new serial port showed up yet. Its LED should be flashing fast (running, no APC40)."
  say "If the LED is dark, tap RESET on the new Seed."
fi
say ""
say "Next:"
say "  - LED flashing fast = ApcBox running, no APC40 yet. Slow = APC40 found."
say "  - Monitor: open $APP/ui/apcbox-ui.html in Chrome, Connect Daisy, pick the NEW port."
say "    The version chip should say V 7."
say "  - Later updates to the new Seed:  tools/flash-seed.sh"
bold "Done."
