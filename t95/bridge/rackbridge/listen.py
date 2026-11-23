"""Listening on a phone, tablet or Mac: the Daisy's USB audio to browsers.

GET /audio.pcm streams what the Daisy plays as raw PCM (16-bit little-endian,
48 kHz, stereo), captured with arecord while at least one browser listens.
The page plays it through Web Audio with a small buffer: about 0.2 s behind
the Daisy on a good Wi-Fi link, so for hearing what you're doing, not for
playing in time.

The Daisy's capture device can be opened once, and the TV sound (alsaloop,
hdmi.py) uses it too, so both go through an ALSA dsnoop device
("daisyshare", defined in a config file this writes next to the state file
and passed with ALSA_CONFIG_PATH). If that device doesn't work on a box, the
TV sound falls back to opening the card directly, and listening then only
works while the TV sound is off.

A slow browser gets old audio dropped (never more than half a second
queued): it must not fall further and further behind.
"""
from .common import *
from .util import *

LISTEN_RATE, LISTEN_CH = 48000, 2
LISTEN_CHUNK = 1920                 # 10 ms
LISTEN_QUEUE = LISTEN_RATE * 4 // 2   # bytes: half a second
LISTEN_MAX = 4                      # browsers at once
SHARE_PCM = "daisyshare"


def write_share_conf(path, card):
    """An ALSA config with a dsnoop device on the Daisy's card, on top of the system's"""
    conf = """# Written by the bridge (rackbridge/listen.py): the Daisy's USB audio, shared
# between the TV sound and browsers that listen.
</usr/share/alsa/alsa.conf>
pcm.%s {
    type dsnoop
    ipc_key 778899
    ipc_perm 0660
    slave {
        pcm "hw:CARD=%s,DEV=0"
        format S16_LE
        rate %d
        channels %d
        period_size 480
        buffer_size 3840
    }
}
""" % (SHARE_PCM, card, LISTEN_RATE, LISTEN_CH)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(conf)
    os.replace(tmp, path)


class Listener:
    def __init__(self):
        self.q = collections.deque()
        self.bytes = 0
        self.dropped = 0
        self.wake = asyncio.Event()
        self.closed = False

    def push(self, chunk):
        self.q.append(chunk)
        self.bytes += len(chunk)
        while self.bytes > LISTEN_QUEUE:      # too far behind: lose the oldest
            old = self.q.popleft()
            self.bytes -= len(old)
            self.dropped += len(old)
        self.wake.set()


class Listen:
    def __init__(self, bridge):
        self.b = bridge
        self.clients = set()
        self.proc = None
        self.reader = None
        self.error = None
        self.starts = 0
        self.dropped = 0
        self.conf = os.path.join(os.path.dirname(bridge.args.state), "asound-share.conf")
        self.shared = True                # the dsnoop device works (until it's seen not to)
        try:
            write_share_conf(self.conf, bridge.args.daisy_card)
        except OSError as e:
            log.warning("can't write %s (%s): TV sound and listening can't share the Daisy's audio", self.conf, e)
            self.shared = False

    def env(self):
        e = dict(os.environ)
        if self.shared:
            e["ALSA_CONFIG_PATH"] = self.conf
        return e

    def device(self):
        return "plug:" + SHARE_PCM if self.shared else "plughw:CARD=" + self.b.args.daisy_card

    def info(self):
        return {"clients": len(self.clients), "running": self.proc is not None and self.proc.returncode is None,
                "shared": self.shared, "starts": self.starts, "dropped_bytes": self.dropped + sum(c.dropped for c in self.clients),
                "error": self.error}

    async def start(self):
        if self.proc is not None and self.proc.returncode is None:
            return True
        if not os.path.exists(os.path.join(self.b.args.asound, self.b.args.daisy_card)):
            self.error = "the Daisy's audio isn't on the T95 (USB)"
            return False
        if not self.shared and self.b.hdmi.running():
            self.error = "the TV sound is using the Daisy's audio: turn it off to listen here"
            return False
        cmd = [self.b.args.arecord, "-q", "-D", self.device(), "-f", "S16_LE", "-r", str(LISTEN_RATE), "-c", str(LISTEN_CH),
               "-t", "raw", "--buffer-time=60000", "--period-time=10000"]
        try:
            self.proc = await asyncio.create_subprocess_exec(*cmd, stdin=asyncio.subprocess.DEVNULL, stdout=asyncio.subprocess.PIPE,
                                                             stderr=asyncio.subprocess.PIPE, env=self.env())
        except OSError as e:
            self.error = "arecord: %s" % e
            return False
        self.starts += 1
        self.error = None
        log.info("listening: %s", " ".join(cmd))
        self.reader = asyncio.ensure_future(self.read(self.proc))
        return True

    async def read(self, proc):
        got = 0
        try:
            while True:
                chunk = await proc.stdout.read(LISTEN_CHUNK)
                if not chunk:
                    break
                got += len(chunk)
                if proc is not self.proc:         # being stopped: a new capture may already serve new listeners
                    break
                for c in self.clients:
                    c.push(chunk)
        finally:
            err = b""
            try:
                err = await asyncio.wait_for(proc.stderr.read(400), 1)
                await asyncio.wait_for(proc.wait(), 2)
            except (asyncio.TimeoutError, OSError):
                pass
            if self.clients and proc is self.proc:    # it stopped by itself: tell the listeners
                msg = err.decode("utf-8", "replace").strip().splitlines()
                self.error = "capture stopped" + (": " + msg[-1][:160] if msg else "")
                log.warning("listening: %s", self.error)
                if got == 0 and self.shared and "daisyshare" in " ".join(msg):
                    log.warning("listening: the shared device doesn't work here")
                for c in list(self.clients):
                    c.closed = True
                    c.wake.set()

    async def stop(self):
        p, self.proc = self.proc, None
        if p is not None and p.returncode is None:
            try:
                p.terminate()
                await asyncio.wait_for(p.wait(), 3)
            except (asyncio.TimeoutError, ProcessLookupError):
                try:
                    p.kill()
                except ProcessLookupError:
                    pass
            log.info("listening: stopped")

    async def serve(self, writer):
        """One browser's /audio.pcm"""
        if len(self.clients) >= LISTEN_MAX:
            return 503, "too many listeners"
        if not await self.start():
            return 503, self.error
        c = Listener()
        self.clients.add(c)
        try:
            # Keep little in the kernel and in asyncio: audio waiting there is delay the page
            # can't skip. What doesn't fit waits in the Listener's queue, where old audio is dropped.
            sock = writer.get_extra_info("socket")
            if sock is not None:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 32768)
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            writer.transport.set_write_buffer_limits(high=16384)
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nCache-Control: no-store\r\n"
                         b"X-Audio: s16le %d %d\r\nX-Accel-Buffering: no\r\nConnection: close\r\n\r\n" % (LISTEN_RATE, LISTEN_CH))
            while not c.closed:
                await c.wake.wait()
                c.wake.clear()
                while c.q:
                    chunk = c.q.popleft()
                    c.bytes -= len(chunk)
                    writer.write(chunk)
                await asyncio.wait_for(writer.drain(), 5)
        except (ConnectionError, OSError, asyncio.TimeoutError):
            pass
        finally:
            self.clients.discard(c)
            self.dropped += c.dropped
            if not self.clients:
                await self.stop()
        return None, None
