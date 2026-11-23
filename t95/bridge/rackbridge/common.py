"""Constants, logging, stdlib imports shared by every building block."""



import argparse
import asyncio
import base64
import binascii
import shutil
import socket
import ssl
import subprocess
import collections
import fcntl
import glob
import json
import logging
import os
import struct
import sys
import termios
import math
import threading
import time
import urllib.parse

VID, PID = "1209", "0001"
ESC_R = b"\x1bR"          # full state dump; v13.2 and older also act on it
MAX_LINE = 4096           # longest real line is D: 2 + 2048 hex digits
CLIENT_QUEUE_BYTES = 512 * 1024
MAX_CLIENTS = 16
STALE_S = 3.0             # no line for this long: ask for a dump again
RESCAN_S = 1.0
LOG_KEEP = 100            # L lines replayed to a new browser
CACHE_MAX = 512
ESC = b"\x1b"
MAX_PORTS = 8             # T95 controller ports 1..8
SERIAL_OUT_MAX = 64 * 1024  # bytes waiting for the Daisy before MIDI is dropped
SYSEX_MAX = 128           # one ESC M frame (HostCmd::kMaxMidi on the Daisy)
SYSEX_PORT_MAX = 8192     # port to port (patch dumps); longer ones are dropped
OUT_SYSEX_MAX = 16        # sysex messages waiting for one controller
KEEPALIVE_S = 0.5         # the Daisy drops T95 controllers after 2 s without one
PING_S = 1.0
ROUTE_TYPES = ("note", "cc", "pc", "at", "pb", "sysex", "clock")
ROUTES_MAX = 64
OUT_FIFO_MAX = 512        # routed messages waiting for one port
CLOCK_PPQ = 24            # MIDI clock ticks per quarter note
CLOCK_BPM_MIN, CLOCK_BPM_MAX = 20.0, 300.0
CLOCK_FIT = 96            # incoming ticks the tempo fit uses (4 beats)
CLOCK_LOST_S = 0.5        # no incoming tick for this long: hold the last tempo
CLOCK_SPIN_S = 0.0005     # sleep to this close to a tick, then spin
PING_KEEP = 300

# IR remote
EV_MSC, MSC_SCAN = 0x04, 0x04
EVIOCGRAB = 0x40044590    # _IOW('E', 0x90, int)
LIRC_SET_REC_MODE = 0x40046912   # _IOW('i', 0x12, __u32)
LIRC_MODE_SCANCODE = 8
LIRC_SCANCODE = struct.Struct("QHHIQ")   # struct lirc_scancode, 24 bytes
LIRC_FLAG_REPEAT = 2
NEC_PROTOS = {9, 10, 11}  # RC_PROTO_NEC, NECX, NEC32 (the T95 remote is NECX)
IMON_NOISE = 0x7FFFFFFF   # what the imon decoder makes of noise on the T95
IR_EVENT = struct.Struct("llHHi")    # struct input_event (64-bit time)
IR_RELEASE_S = 0.2        # no repeat frame for this long: button released (NEC repeats ~110 ms)
IR_HOLD_S = 0.5
IR_REPEAT_S = 0.11
IR_LEARN_S = 15.0
IR_ACTIONS = ["up", "down", "left", "right", "ok", "back", "home", "menu",
              "play", "stop", "next", "prev", "vol_up", "vol_down",
              "1", "2", "3", "4", "5", "6", "7", "8", "9", "0"]
IR_REPEATABLE = {"up", "down", "left", "right", "vol_up", "vol_down"}
IR_PROFILES = ["all", "selector", "apcbox", "midihub", "clock"]
# Remote actions the bridge plays on the Daisy itself, while a browser shows
# the ApcBox page. Firmware with control protocol 2 (v13.7) takes them as UI
# actions (ESC U): encoder, transport, track select. Older firmware: only
# play/stop, as presses of the APC40's own buttons on the port of the APC40
# that drives the box (so only when that APC40 is on the T95).
# What a page may send with POST /do (control protocol 4, v13.9): first word
# -> lowest control version. Only lower-case letters, digits, spaces and "-".
UI_ACTIONS = {"play": 2, "stop": 2, "track": 2, "turn": 2, "push": 2, "back": 2,
              "mute": 4, "solo": 4, "knob": 4, "acid": 4, "glob": 4, "tempo": 4, "step": 4, "note": 4,
              "pat": 4, "pcopy": 4, "pclear": 4, "undo": 4, "fx": 4, "rec": 4, "metro": 4, "mon": 4,
              "type": 4, "lrec": 4, "kit": 4, "audition": 4}
UI_ACTION_MAX = 31        # HostCmd::kMaxText on the Daisy is 32
DAISY_UI = {"play": "play", "stop": "stop", "left": "turn -1", "right": "turn 1",
            "ok": "push", "back": "back", "0": "track 10",
            **{str(n): "track %d" % n for n in range(1, 10)}}
APC_BUTTONS = {"play": 0x5B, "stop": 0x5C}
# Modules: what the selector offers. Each declares its page, what it needs,
# its remote profile, and its default routes (a module's own routes, set on
# its page, come on top). One active at a time for now; the active one's
# routing is what the router applies. See rackbridge/router.py.
MODULES = [
    {"id": "apcbox", "name": "ApcBox groovebox", "url": "/apcbox", "ready": True, "needs": ["daisy"],
     "profile": "apcbox", "about": "Daisy: 16 tracks, samples and 303s, APC40 + keyboards",
     "routes": [{"from": "*", "to": "daisy"}]},
    {"id": "midihub", "name": "MIDI hub", "url": "/midihub", "ready": True, "needs": [],
     "profile": "midihub", "about": "any MIDI device to any other, network MIDI", "routes": []},
    {"id": "clock", "name": "Clock", "url": "/clock", "ready": True, "needs": [], "profile": "clock",
     "about": "one tempo for everything: follow Cubase or Link, send MIDI clock", "routes": []},
    {"id": "dawsync", "name": "Cubase sync", "url": None, "ready": False, "needs": [], "profile": None,
     "about": "planned: a plugin in Cubase shares its transport (Link)", "routes": []},
    {"id": "dj", "name": "Numark DJ", "url": None, "ready": False, "needs": [], "profile": None,
     "about": "later", "routes": []},
]
POST_MAX = 4096

# Samples (control protocol 3): the T95 keeps the library, the Daisy gets a
# track's sound over USB. See daisy/xfer.h for the Daisy side.
AUDIO_EXT = (".wav", ".wave", ".aif", ".aiff", ".flac", ".mp3", ".ogg", ".m4a",
             ".webm", ".mp4")   # .webm / .mp4: recordings from the page (Chrome / Safari)
XFER_CHUNK = 512          # bytes per ESC F D frame (kXferChunk on the Daisy)
XFER_WINDOW = 4           # chunks in flight; the Daisy's receive ring takes 8 KB
XFER_RESEND_S = 0.5
XFER_TRIES = 8            # sends per chunk before giving up
UPLOAD_MAX = 64 * 1024 * 1024

# Line types whose second field says which track/slot they describe
KEYED = {"T", "N", "Q", "A", "KN", "Z"}

log = logging.getLogger("bridge")

__all__ = [n for n in dir() if not n.startswith("__")]  # star-import: stdlib modules and constants too
