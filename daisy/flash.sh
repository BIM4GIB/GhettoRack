#!/bin/sh
# Flash ApcBox onto the Daisy. No buttons needed once ApcBox is running.
#   sh ~/dev/DaisyExamples/seed/ApcBox/flash.sh            (flashes build/ApcBox.bin)
#   sh ~/dev/DaisyExamples/seed/ApcBox/flash.sh other.bin
# Close the monitor page first (it holds the Daisy's serial port).
cd "$(dirname "$0")"
BIN=${1:-build/ApcBox.bin}
[ -f "$BIN" ] || { echo "no $BIN"; exit 1; }

mode() {
  L=$(dfu-util -l 2>/dev/null)
  case "$L" in
    *"@Flash"*) echo daisy ;;
    *"@Internal Flash"*) echo rom ;;
    *) echo none ;;
  esac
}

if [ "$(mode)" = none ]; then
  PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
  if [ -n "$PORT" ]; then
    echo "asking ApcBox on $PORT to go to the bootloader..."
    stty -f "$PORT" 115200 raw -echo 2>/dev/null
    printf '\033BOOT' > "$PORT" 2>/dev/null || echo "couldn't write to $PORT (monitor page still connected?)"
  fi
  echo "waiting for the Daisy bootloader (if nothing happens in a few seconds,"
  echo "unplug the Daisy's USB-C and plug it back in)..."
  i=0
  while [ $i -lt 80 ]; do
    [ "$(mode)" != none ] && break
    sleep 0.25
    i=$((i + 1))
  done
fi

case "$(mode)" in
  daisy)
    dfu-util -a 0 -s 0x90040000:leave -D "$BIN" -d ,0483:df11
    echo "done (an 'Error 74' after 'File downloaded successfully' is normal)"
    ;;
  rom)
    echo "The Daisy is in the chip's built-in mode (BOOT was held during reset)."
    echo "Putting the Daisy bootloader back, then run this script again after"
    echo "unplugging and replugging the Daisy's USB-C:"
    dfu-util -a 0 -s 0x08000000:leave -D ../lib/libDaisy/core/dsy_bootloader_v6_4-intdfu-2000ms.bin -d ,0483:df11
    ;;
  none)
    echo "No Daisy found. Either it isn't running ApcBox yet, or the monitor page"
    echo "has the port. Close the page, or do it by hand once:"
    echo "  tap RESET, press BOOT while the LED pulses, then run this script again."
    ;;
esac
