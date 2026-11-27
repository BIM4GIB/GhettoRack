"""The decks: the DJ module's own sound engine (decks/, the program deckd).

While the Numark DJ module is the active one and the decks' sound card is
there, the bridge runs deckd and talks to it over a Unix socket, in the text
lines decks/README.md lists. The bridge keeps no time here: a controller's
MIDI goes to deckd whole and at once ("M hex"), deckd's lights come back
("X hex") and go to the controller through its port's queue, and what the
decks are doing is kept as the last value of each line for pages to read.

    GET  /decks            what the decks are doing, the tracks' outlines, the music folder's files
    POST /decks {"a": ..}  one of DECK_ACTIONS, or {"a": "load", "deck": 1, "file": "name in the music folder"}
    SSE  "decks"           the same picture without outlines and files, at most DECKS_PUSH_HZ a second

A controller reaches the decks through a route to "decks" (the module's own:
the Numark by name). Sysex and clock from it are not passed on.
"""
from .common import *
from .util import *
from .samples import safe_join

DECKS_PUSH_HZ = 12            # the picture to browsers; a page moves its own playhead between two
DECKS_OUT_MAX = 64 * 1024     # bytes waiting for deckd before a controller's MIDI is dropped
DECKS_MIDI_MAX = 3            # channel messages only
DECKS_FILES_MAX = 2000
DECKS_LOG_KEEP = 20
DECKS_LINE_MAX = 8192         # W: 800 peaks, two hex digits each
DECK_ACTIONS = {"eject", "play", "cue", "seek", "set", "browse", "pick", "scan"}
DECK_SET = {"gain", "hi", "mid", "lo", "filter", "volume", "pfl", "scratch", "rate",
            "xfader", "master", "cuemix", "cuegain"}
SUN_PATH_MAX = 100            # sockaddr_un takes 104 (macOS) or 108 (Linux) bytes


def deck_action(a):
    """The line for deckd, or None: lower-case words and numbers only, nothing that could start a second line"""
    if not isinstance(a, str) or len(a) > 48:
        return None
    w = a.split(" ")
    if w[0] not in DECK_ACTIONS or len(w) > 4:
        return None
    for i, x in enumerate(w[1:]):
        if w[0] == "set" and i == 0:
            if x not in DECK_SET:
                return None
            continue
        try:
            if not math.isfinite(float(x)) or len(x) > 12 or x.strip("-.0123456789"):
                return None
        except ValueError:
            return None
    return a


class Decks:
    def __init__(self, bridge):
        self.b = bridge
        a = bridge.args
        self.binary = a.deckd
        self.music = a.music or os.path.join(os.path.dirname(a.state), "music")
        self.sock_path = os.path.join(os.path.dirname(a.state), "deckd.sock")
        if len(self.sock_path) > SUN_PATH_MAX:     # a long test folder: the name still has to fit
            self.sock_path = os.path.join("/tmp", "deckd-%d.sock" % os.getpid())
        self.proc = None
        self.reader = self.writer = None
        self.read_task = None
        self.error = None
        self.starts = 0
        self.dropped = 0
        self.greeted = set()          # controllers that have been sent the engine's hello
        self.lit = []                 # the ports the engine's lights go to: the ones routed to it when last looked
        self.dirty = False
        self.clear()

    def clear(self):
        self.version = None
        self.decks = [{}, {}]
        self.mix = {}
        self.browse = {}
        self.health = {}
        self.waves = {}
        self.log = collections.deque(maxlen=DECKS_LOG_KEEP)

    # ---- what there is ----

    def wanted(self):
        return self.b.module()["id"] == "dj"

    def card_here(self):
        return self.b.args.decks_card == "null" or os.path.exists(os.path.join(self.b.args.asound, self.b.args.decks_card))

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def connected(self):
        return self.writer is not None

    def controllers(self):
        """The ports the active routes send to the decks"""
        rs = [r for r in self.b.active_routes() if r["to"] == "decks"]
        return [p for p in sorted(self.b.ports.values(), key=lambda p: p.num)
                if any(r["from"] in ("*", p.name) for r in rs)]

    def info(self):
        return {"wanted": self.wanted(), "running": self.running(), "connected": self.connected(),
                "card": self.b.args.decks_card, "card_here": self.card_here(), "engine": bool(self.binary and os.path.exists(self.binary)),
                "controllers": [p.name for p in self.controllers()], "starts": self.starts, "dropped": self.dropped,
                "error": self.error, "health": self.health}

    def picture(self):
        return {"running": self.connected(), "decks": self.decks, "mix": self.mix, "browse": self.browse,
                "health": self.health, "controllers": [p.name for p in self.controllers()]}

    def files(self):
        out = []
        for root, dirs, names in os.walk(self.music):
            dirs[:] = sorted(d for d in dirs if not d.startswith("."))
            for n in sorted(names):
                if not n.startswith(".") and n.lower().endswith(AUDIO_EXT):
                    out.append(os.path.relpath(os.path.join(root, n), self.music))
                    if len(out) >= DECKS_FILES_MAX:
                        return out
        return out

    def full(self):
        """For GET /decks (runs off the loop: it walks the music folder)"""
        return dict(self.picture(), info=self.info(), waves=self.waves, files=self.files(), log=list(self.log))

    # ---- to the engine ----

    def send(self, line):
        if self.writer is None:
            return False
        if self.writer.transport.get_write_buffer_size() > DECKS_OUT_MAX:
            self.dropped += 1
            return False
        self.writer.write(line.encode() + b"\n")
        return True

    def midi_in(self, msg):
        """One channel message from a controller routed to the decks"""
        if len(msg) <= DECKS_MIDI_MAX and msg[0] < 0xF0:
            self.send("M " + msg.hex())

    def do(self, d):
        if not self.connected():
            return 503, {"error": "the decks are not running" + (": " + self.error if self.error else "")}
        a = d.get("a")
        if a == "load":
            deck, rel = d.get("deck"), d.get("file")
            path = safe_join(self.music, rel) if isinstance(rel, str) and rel else None
            if deck not in (1, 2) or path is None or not os.path.isfile(path) or not path.lower().endswith(AUDIO_EXT) \
                    or "\n" in path or "\r" in path:
                return 400, {"error": "deck 1 or 2 and a file in the music folder"}
            line = "load %d %s" % (deck, path)
        else:
            line = deck_action(a)
            if line is None:
                return 400, {"error": "one of: load, " + ", ".join(sorted(DECK_ACTIONS))}
        if not self.send(line):
            return 503, {"error": "the decks are not taking lines"}
        return 200, {"ok": True}

    # ---- from the engine ----

    def on_line(self, line):
        f = line.split(" ")
        k = f[0]
        try:
            if k == "X":
                for p in self.lit:      # not controllers(): when the module is left the routes are gone
                    if self.b.ports.get(p.num) is p:      # already, and the goodbye still has to reach them
                        self.b.midi_out("X %d %s" % (p.num, f[1]))
                return
            if k == "D":
                d, v = int(f[1]) - 1, [int(x) for x in f[2:10]]
                self.decks[d].update(playing=bool(v[0]), cue_held=bool(v[1]), pos=v[2] / 1000, len=v[3] / 1000,
                                     speed=v[4] / 1000, cue=v[5] / 1000, peak=v[6] / 1000, track=v[7])
            elif k == "T":
                d, v = int(f[1]) - 1, [int(x) for x in f[2:12]]
                self.decks[d].update(rate=v[0] / 10000, range=v[1], gain=v[2] / 1000, hi=v[3] / 1000, mid=v[4] / 1000,
                                     lo=v[5] / 1000, filter=v[6] / 1000, volume=v[7] / 1000, pfl=v[8] >= 500, scratch=v[9] >= 500)
            elif k == "G":
                v = [int(x) / 1000 for x in f[1:7]]
                self.mix = {"xfader": v[0], "master": v[1], "cue_mix": v[2], "cue_gain": v[3], "peak": [v[4], v[5]]}
            elif k == "N":
                d, track = int(f[1]) - 1, int(f[2])
                self.decks[d].update(track=track, name=" ".join(f[3:]) if track else None)
                if not track:
                    self.waves.pop(d + 1, None)
            elif k == "W":
                self.waves[int(f[1])] = {"track": int(f[2]), "peaks": f[3]}
            elif k == "B":
                self.browse = {"at": int(f[1]), "count": int(f[2]), "name": " ".join(f[3:])}
            elif k == "H":
                v = [int(x) for x in f[1:8]]
                self.health = {"xruns": v[0], "worst_us": v[1], "period": v[2], "buffer": v[3], "rt": bool(v[4]),
                               "channels": v[5], "events_dropped": v[6]}
            elif k == "L":
                self.log.append(line[2:])
                log.info("decks: %s", line[2:])
            elif k == "V":
                self.version = int(f[1])
            else:
                return
        except (ValueError, IndexError):
            log.warning("decks: a line that does not fit: %s", line[:80])
            return
        self.dirty = True

    async def read_lines(self):
        buf = b""
        try:
            while True:
                data = await self.reader.read(65536)
                if not data:
                    break
                buf += data
                *lines, buf = buf.split(b"\n")
                if len(buf) > DECKS_LINE_MAX:
                    buf = b""
                for l in lines:
                    self.on_line(l.decode("utf-8", "replace").rstrip("\r"))
        except (OSError, asyncio.CancelledError):
            pass

    # ---- starting and stopping ----

    def start(self):
        a = self.b.args
        try:
            os.makedirs(self.music, exist_ok=True)
        except OSError:
            pass
        try:
            os.unlink(self.sock_path)
        except OSError:
            pass
        cmd = [self.binary, "--socket", self.sock_path, "--music", self.music,
               "--period", str(a.decks_period), "--buffer", str(a.decks_buffer)]
        cmd += ["--null"] if a.decks_card == "null" else ["--device", "hw:CARD=%s,DEV=0" % a.decks_card]
        try:
            self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.starts += 1
            self.error = None
            log.info("decks on (%s)", " ".join(cmd))
        except OSError as e:
            self.error = str(e)
            self.proc = None

    async def connect(self):
        try:
            self.reader, self.writer = await asyncio.open_unix_connection(self.sock_path, limit=DECKS_LINE_MAX * 2)
        except OSError:
            self.reader = self.writer = None
            return
        self.greeted = set()
        self.read_task = asyncio.ensure_future(self.read_lines())

    def drop_connection(self):
        if self.read_task is not None:
            self.read_task.cancel()
        if self.writer is not None:
            try:
                self.writer.close()
            except OSError:
                pass
        self.reader = self.writer = self.read_task = None
        self.greeted = set()
        self.lit = []
        self.clear()
        self.dirty = True

    async def stop(self):
        if self.connected():
            self.send("C 0")              # the controller back to how it was: the engine knows how
            await asyncio.sleep(0.2)      # its answer comes through read_lines and goes out to the port
        self.stop_now()
        log.info("decks off")

    def stop_now(self):
        self.drop_connection()
        if self.proc is not None:
            if self.proc.poll() is None:
                self.proc.terminate()
                try:
                    self.proc.wait(3)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
            self.proc = None
        try:
            os.unlink(self.sock_path)
        except OSError:
            pass

    async def task(self):
        tick, last_try = 0, 0.0
        while True:
            await asyncio.sleep(1 / DECKS_PUSH_HZ)
            tick += 1
            if self.connected() and (self.read_task.done() or not self.running()):
                code = self.proc.poll() if self.proc is not None else None
                self.drop_connection()
                if code is not None:
                    self.error = "the engine stopped (%s): is the sound card still there?" % code
                    self.proc = None
                    self.b.announce()
            if tick % 6 == 0:                  # twice a second: should it run?
                want = self.wanted()
                if not want and (self.running() or self.connected()):
                    await self.stop()
                    self.error = None
                    self.b.announce()
                elif want and not self.running() and now() - last_try > 2:
                    last_try = now()
                    if not (self.binary and os.path.exists(self.binary)):
                        self.error = "the engine is not installed (decks/build.sh)"
                    elif not self.card_here():
                        self.error = "no sound card called %s: is the controller plugged in?" % self.b.args.decks_card
                    else:
                        self.start()
                    self.b.announce()
                elif want and self.running() and not self.connected():
                    await self.connect()
                    if self.connected():
                        self.b.announce()
            if self.connected():
                if self.wanted():
                    self.lit = self.controllers()
                here = {id(p) for p in self.lit}
                if here - self.greeted:
                    self.send("C 1")           # stop its demo, ask where its knobs stand, every light again
                self.greeted = here
            if self.dirty:
                self.dirty = False
                self.b.broadcast(sse_event("decks", self.picture()))
