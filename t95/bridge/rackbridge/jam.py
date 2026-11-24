"""Jam recorder: what the Daisy plays, recorded on the T95.

The Daisy's master arrives as USB audio (16-bit, 48 kHz, stereo). arecord, a
process of its own, writes it as WAV into the bridge's folder on the eMMC
(`jams/<take>/part.wav`, `part-01.wav`...: a new file every half hour, so a
power cut costs the open part at most). The bridge only starts it, stops it
and looks at the files once in a while: no audio passes through the bridge.
It shares the capture device with the TV sound and listening (listen.py).

Takes are played and downloaded as one WAV (`/jam/file`), the parts strung
together on the way out. Copying takes to the SD card is not here yet.
"""
from .common import *
from .util import *
import array
import re
import shutil

JAM_RATE, JAM_CH = 48000, 2
JAM_FRAME = 2 * JAM_CH                 # bytes per sample frame
JAM_BPS = JAM_RATE * JAM_FRAME         # bytes per second
JAM_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9 _.-]{0,47}")
JAM_TICK_S = 0.5
JAM_TAKES_MAX = 500
WAV_MAX = 0xFFFFFFFF - 44              # data bytes one WAV header can state


def jam_name_ok(name):
    return (isinstance(name, str) and bool(JAM_NAME.fullmatch(name)) and ".." not in name
            and not name.endswith((" ", ".")))


def wav_data(path):
    """(offset, bytes) of the sound in a WAV file. The length is what is in the
    file, not what the header says: arecord states it when it closes the file,
    and the open part (or one cut off by a power cut) says something else."""
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            head = f.read(4096)
    except OSError:
        return None
    if head[:4] != b"RIFF" or head[8:12] != b"WAVE":
        return None
    pos = 12
    while pos + 8 <= len(head):
        cid, n = head[pos:pos + 4], struct.unpack("<I", head[pos + 4:pos + 8])[0]
        if cid == b"data":
            off = pos + 8
            return off, max(0, (size - off) // JAM_FRAME * JAM_FRAME)
        pos += 8 + n + (n & 1)
    return None


def wav_header(nbytes):
    nbytes = min(nbytes, WAV_MAX)
    return (b"RIFF" + struct.pack("<I", 36 + nbytes) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, JAM_CH, JAM_RATE, JAM_BPS, JAM_FRAME, 16)
            + b"data" + struct.pack("<I", nbytes))


def wav_fix(path):
    """Make a closed part's header say what the file holds (a part cut off by a power cut)"""
    d = wav_data(path)
    if d is None or d[0] != 44:
        return False
    try:
        with open(path, "r+b") as f:
            head = f.read(44)
            want = wav_header(d[1])
            if head[4:8] == want[4:8] and head[40:44] == want[40:44]:
                return False
            f.seek(4)
            f.write(want[4:8])
            f.seek(40)
            f.write(want[40:44])
        return True
    except OSError:
        return False


def part_key(name):
    m = re.fullmatch(r"part(?:-(\d+))?\.wav", name)
    return int(m.group(1) or 0) if m else None


class Jam:
    def __init__(self, bridge):
        self.b = bridge
        a = bridge.args
        self.dir = getattr(a, "jams", None) or os.path.join(os.path.dirname(a.state), "jams")
        self.min_free = int(getattr(a, "jam_min_free_mb", 1024)) << 20
        self.split_s = int(getattr(a, "jam_split_s", 1800))
        self.proc = None
        self.take = None
        self.started = None
        self.error = None
        self.note = None                   # why the last take ended, if not by hand
        self.bytes = 0
        self.peak = [0.0, 0.0]
        self.overruns = 0
        self.takes_n = 0
        self.lock = asyncio.Lock()
        self.last_sent = None

    # ---- files ----
    def parts(self, take):
        d = os.path.join(self.dir, take)
        try:
            names = [n for n in os.listdir(d) if part_key(n) is not None]
        except OSError:
            return []
        return [os.path.join(d, n) for n in sorted(names, key=part_key)]

    def segments(self, take):
        """[(path, offset, bytes)] of a take's sound, in order"""
        out = []
        for p in self.parts(take):
            d = wav_data(p)
            if d and d[1]:
                out.append((p, d[0], d[1]))
        return out

    def free(self):
        try:
            v = os.statvfs(self.dir if os.path.isdir(self.dir) else os.path.dirname(self.dir))
            return v.f_frsize * v.f_bavail
        except OSError:
            return None

    def list(self):
        """Runs in a thread"""
        takes = []
        try:
            names = [n for n in os.listdir(self.dir) if jam_name_ok(n) and os.path.isdir(os.path.join(self.dir, n))]
        except OSError:
            names = []
        for n in names:
            segs = self.segments(n)
            if not segs and n != self.take:
                continue
            nbytes = sum(s[2] for s in segs)
            try:
                at = os.path.getmtime(segs[-1][0]) if segs else time.time()
            except OSError:
                at = 0
            takes.append({"take": n, "seconds": round(nbytes / JAM_BPS, 1), "bytes": nbytes, "parts": len(segs),
                          "at": int(at), "recording": n == self.take})
        takes.sort(key=lambda t: t["at"], reverse=True)
        self.takes_n = len(takes)
        return dict(self.info(), takes=takes[:JAM_TAKES_MAX], more=len(takes) > JAM_TAKES_MAX,
                    min_free=self.min_free, split_s=self.split_s)

    def repair(self):
        """Runs in a thread at start: parts a power cut left with a wrong header"""
        n = 0
        try:
            for t in os.listdir(self.dir):
                for p in self.parts(t):
                    n += wav_fix(p)
        except OSError:
            pass
        if n:
            log.info("jam: %d part(s) had a wrong length in the header (cut off); put right", n)

    # ---- state ----
    def recording(self):
        return self.proc is not None and self.proc.poll() is None

    def info(self):
        return {"recording": self.recording(), "take": self.take, "seconds": round(self.bytes / JAM_BPS, 1),
                "bytes": self.bytes, "peak": self.peak, "overruns": self.overruns, "free": self.free(),
                "daisy_audio": os.path.exists(os.path.join(self.b.args.asound, self.b.args.daisy_card)),
                "error": self.error, "note": self.note}

    def tell(self):
        self.b.broadcast(sse_event("jam", self.info()))

    def measure(self):
        """The take's length so far, and the level of its last tenth of a second"""
        segs = self.segments(self.take) if self.take else []
        self.bytes = sum(s[2] for s in segs)
        self.peak = [0.0, 0.0]
        if segs:
            p, off, n = segs[-1]
            want = min(n, JAM_BPS // 10)
            try:
                with open(p, "rb") as f:
                    f.seek(off + n - want)
                    a = array.array("h", f.read(want))
                if sys.byteorder == "big":
                    a.byteswap()
                for ch in range(JAM_CH):
                    s = a[ch::JAM_CH]
                    if s:
                        self.peak[ch] = round(max(max(s), -min(s)) / 32768, 3)
            except (OSError, ValueError):
                pass
        if self.take:
            try:
                with open(os.path.join(self.dir, self.take, "arecord.log"), "rb") as f:
                    self.overruns = f.read(65536).count(b"overrun")
            except OSError:
                pass

    async def start(self):
        async with self.lock:
            if self.recording():
                return 400, {"error": "already recording"}
            if not os.path.exists(os.path.join(self.b.args.asound, self.b.args.daisy_card)):
                return 503, {"error": "the Daisy's audio isn't on the T95 (USB)"}
            if not self.b.listen.shared and (self.b.hdmi.running() or self.b.listen.clients):
                return 503, {"error": "the Daisy's audio is in use (TV sound or listening) and can't be shared on this box"}
            try:
                os.makedirs(self.dir, exist_ok=True)
            except OSError as e:
                return 503, {"error": "no folder for takes: %s" % e}
            free = self.free()
            if free is not None and free < self.min_free + 60 * JAM_BPS:
                return 503, {"error": "not enough room on the T95 (%d MB free, %d MB are kept)" % (free >> 20, self.min_free >> 20)}
            base = time.strftime("jam %Y-%m-%d %H.%M.%S")
            take, n = base, 1
            while os.path.exists(os.path.join(self.dir, take)):
                n += 1
                take = "%s %d" % (base, n)
            d = os.path.join(self.dir, take)
            cmd = [self.b.args.arecord, "-q", "-D", self.b.listen.device(), "-f", "S16_LE", "-r", str(JAM_RATE), "-c", str(JAM_CH),
                   "-t", "wav", "--max-file-time=%d" % self.split_s, os.path.join(d, "part.wav")]
            try:
                os.mkdir(d)
                with open(os.path.join(d, "arecord.log"), "wb") as errf:
                    self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=errf,
                                                 env=self.b.listen.env())
            except OSError as e:
                self.error = "arecord: %s" % e
                return 503, {"error": self.error}
            self.take, self.started, self.error, self.note = take, now(), None, None
            self.bytes, self.peak, self.overruns = 0, [0.0, 0.0], 0
            log.info("jam: recording %s (%s)", take, " ".join(cmd))
            self.tell()
            return 200, dict(self.info(), ok=True)

    def end_proc(self):
        """Blocking, a few seconds at most: arecord closes its file on SIGTERM"""
        p = self.proc
        if p is not None and p.poll() is None:
            p.terminate()
            try:
                p.wait(4)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait(2)

    async def stop(self, note=None):
        async with self.lock:
            if self.proc is None:
                return 400, {"error": "not recording"}
            await asyncio.get_running_loop().run_in_executor(None, self.end_proc)
            await asyncio.get_running_loop().run_in_executor(None, self.finish, note)
            return 200, dict(self.info(), ok=True)

    def finish(self, note=None):
        take, self.proc = self.take, None
        self.measure()
        for p in self.parts(take):
            wav_fix(p)
        d = os.path.join(self.dir, take)
        try:
            if not self.segments(take):          # nothing came: no empty take in the list
                shutil.rmtree(d, ignore_errors=True)
            elif os.path.getsize(os.path.join(d, "arecord.log")) == 0:
                os.remove(os.path.join(d, "arecord.log"))
        except OSError:
            pass
        self.note = note
        log.info("jam: %s ended, %.1f s%s", take, self.bytes / JAM_BPS, " (" + note + ")" if note else "")
        self.take = None
        self.peak = [0.0, 0.0]

    def stop_now(self):
        """At shutdown: no event loop to wait on"""
        if self.proc is not None:
            self.end_proc()
            self.finish("the bridge stopped")

    async def task(self):
        loop = asyncio.get_running_loop()
        await loop.run_in_executor(None, self.repair)
        while True:
            await asyncio.sleep(JAM_TICK_S)
            if self.proc is None:
                continue
            await loop.run_in_executor(None, self.measure)
            if self.proc is None:                # stopped while we looked
                continue
            if self.proc.poll() is not None:     # it ended by itself: the Daisy left, or the device failed
                why = ""
                try:
                    with open(os.path.join(self.dir, self.take, "arecord.log"), "rb") as f:
                        lines = f.read(4096).decode("utf-8", "replace").strip().splitlines()
                    why = lines[-1][:160] if lines else ""
                except OSError:
                    pass
                async with self.lock:
                    if self.proc is not None:
                        await loop.run_in_executor(None, self.finish, "recording stopped by itself" + (": " + why if why else ""))
                        self.error = self.note
                self.tell()
                continue
            free = self.free()
            if free is not None and free < self.min_free:
                await self.stop("stopped: the T95 is nearly full")
                self.tell()
                continue
            self.tell()

    # ---- pages ----
    def take_dir(self, take):
        if not jam_name_ok(take):
            return None
        d = os.path.join(self.dir, take)
        return d if os.path.isdir(d) else None

    async def post(self, path, d):
        loop = asyncio.get_running_loop()
        if path == "/jam/start":
            return await self.start()
        if path == "/jam/stop":
            code, out = await self.stop()
            self.tell()
            return code, out
        take = d.get("take")
        src = self.take_dir(take)
        if src is None:
            return 400, {"error": "no such take"}
        if take == self.take:
            return 400, {"error": "that take is still recording"}
        if path == "/jam/delete":
            await loop.run_in_executor(None, lambda: shutil.rmtree(src, ignore_errors=True))
            return 200, {"ok": True}
        if path == "/jam/rename":
            to = d.get("to")
            if isinstance(to, str):
                to = to.strip()
            if not jam_name_ok(to):
                return 400, {"error": "a name: letters, digits, space, - _ . (up to 48)"}
            dst = os.path.join(self.dir, to)
            if os.path.exists(dst):
                return 400, {"error": "there is a take of that name"}
            try:
                os.rename(src, dst)
            except OSError as e:
                return 400, {"error": str(e)}
            return 200, {"ok": True, "take": to}
        if path == "/jam/library":
            return await loop.run_in_executor(None, self.to_library, take)
        return 404, {"error": "not found"}

    def to_library(self, take):
        """The take's parts into the sample library (folder jams): links, no second copy"""
        dst = os.path.join(self.b.library, "jams")
        parts = self.parts(take)
        made = []
        try:
            os.makedirs(dst, exist_ok=True)
            for i, p in enumerate(parts):
                name = take + (" %d" % (i + 1) if len(parts) > 1 else "") + ".wav"
                q = os.path.join(dst, name)
                if os.path.exists(q):
                    continue
                try:
                    os.link(p, q)
                except OSError:
                    shutil.copyfile(p, q)
                made.append(name)
        except OSError as e:
            return 400, {"error": "into the library: %s" % e}
        return 200, {"ok": True, "folder": "jams", "files": made, "parts": len(parts)}

    async def serve_file(self, writer, take, head, download):
        """A take as one WAV: a header, then every part's sound. Byte ranges, so a browser can seek."""
        if self.take_dir(take) is None:
            return 404, "no such take"
        loop = asyncio.get_running_loop()
        segs = await loop.run_in_executor(None, self.segments, take)
        data = min(sum(s[2] for s in segs), WAV_MAX)
        if not data:
            return 404, "nothing in that take"
        total = 44 + data
        start, end, code = 0, total - 1, 200
        m = re.search(r"\r\nrange:\s*bytes=(\d*)-(\d*)", head, re.I)
        if m and (m.group(1) or m.group(2)):
            if m.group(1):
                start = int(m.group(1))
                end = min(int(m.group(2)), total - 1) if m.group(2) else total - 1
            else:
                start = max(0, total - int(m.group(2)))
            if start > end or start >= total:
                writer.write(("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%d\r\nContent-Length: 0\r\n"
                              "Connection: close\r\n\r\n" % total).encode())
                await asyncio.wait_for(writer.drain(), 10)
                return None, None
            code = 206
        h = "HTTP/1.1 %d %s\r\nContent-Type: audio/wav\r\nContent-Length: %d\r\nAccept-Ranges: bytes\r\nCache-Control: no-store\r\n" % (
            code, "Partial Content" if code == 206 else "OK", end - start + 1)
        if code == 206:
            h += "Content-Range: bytes %d-%d/%d\r\n" % (start, end, total)
        if download:
            h += 'Content-Disposition: attachment; filename="%s.wav"\r\n' % take
        writer.write((h + "Connection: close\r\n\r\n").encode())
        pos = start
        if pos < 44:
            chunk = wav_header(data)[pos:min(44, end + 1)]
            writer.write(chunk)
            pos += len(chunk)
        at = 44                                  # where this part's sound starts in the whole
        for p, off, n in segs:
            if pos > end:
                break
            n = min(n, total - at)
            if pos < at + n:
                def read(f, k):
                    return f.read(k)
                try:
                    f = open(p, "rb")
                except OSError:
                    return None, None            # deleted meanwhile
                with f:
                    f.seek(off + pos - at)
                    while pos < at + n and pos <= end:
                        chunk = await loop.run_in_executor(None, read, f, min(262144, at + n - pos, end + 1 - pos))
                        if not chunk:
                            return None, None    # the file got shorter under us: the browser sees a short answer
                        writer.write(chunk)
                        await asyncio.wait_for(writer.drain(), 30)
                        pos += len(chunk)
            at += n
        return None, None
