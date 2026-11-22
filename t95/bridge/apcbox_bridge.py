#!/usr/bin/env python3
"""ApcBox bridge: Daisy USB serial <-> T95 controllers, and SSE for browsers.

    controllers --raw MIDI--> bridge --ESC M frames--> Daisy
    controllers <--raw MIDI-- bridge <--X lines------- Daisy (APC40 LEDs)
    Daisy --report lines--> bridge --SSE /events--> browsers

To the Daisy it sends ESC R (state dump) on connect and when the Daisy goes
quiet. Only once the Daisy reports a "P" line (firmware v13.5 on, control
protocol 1) does it also send controller MIDI (ESC M), port names (ESC N),
keepalives (ESC K) and pings (ESC P). Python 3 standard library only.

  /             the monitor page (daisy/ui/apcbox-ui.html)
  /events       SSE: a snapshot of the latest state, then every line live.
                Default events carry one report line each; "bridge" events
                carry the bridge's own status as JSON.
  /health       JSON: Daisy link, data age, counters, clients, controllers,
                round-trip times to the Daisy
  /caps         JSON: what this server supports (the page reads it to hide
                the Mac build's /ui and /tft.bmp controls)
  /             module selector and IR remote setup (t95/ui/selector.html)
  /apcbox       the ApcBox page
  /state        JSON: modules, last module, IR profiles; POST /select,
                /settings, /ir/learn, /ir/forget, /ir/cancel change it

IR remote: the kernel decodes the remote (rc core, NEC). The bridge reads
scancodes from the lirc device, which marks repeat frames and the protocol:
a press starts only on a fresh NEC frame, so stray repeats (which carry the
previous button's code) and other decoders' noise are ignored. No keymap is
involved. Without lirc it falls back to the input device (MSC_SCAN).
Buttons are bound to actions per profile (one per view, plus "all"), learnt
from the setup page, and sent to browsers as "ir" events for their view.

The Daisy is found by USB VID/PID (and optionally serial) under /sys, not by
tty index; controllers by their USB device under /sys/class/sound, not by
ALSA card number. --port and --midi force paths instead (tests, odd setups).
See t95/bridge/README.md.

The code is in rackbridge/ (one file per building block). This file is the
entry point the service and the tests use.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from rackbridge.bridge import main  # noqa: E402
from rackbridge.midi import find_midi, MidiParser  # noqa: E402,F401  (tests)
from rackbridge.ir import find_ir  # noqa: E402,F401
from rackbridge.samples import convert_for_daisy, fix_wav_sizes  # noqa: E402,F401

if __name__ == "__main__":
    main()
