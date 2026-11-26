"""Sample library on the T95 and transfers to the Daisy (ESC F)."""
import re

from .common import *
from .util import *


def safe_join(root, rel):
    """root/rel, or None if rel tries to leave root"""
    if root is None:
        return None
    rootr = os.path.realpath(root)
    p = os.path.realpath(os.path.join(rootr, (rel or "").lstrip("/")))
    return p if p == rootr or p.startswith(rootr + os.sep) else None


def fix_wav_sizes(data):
    """ffmpeg writing WAV to a pipe can't go back to fill in the sizes"""
    b = bytearray(data)
    if len(b) < 12 or b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        return data
    b[4:8] = struct.pack("<I", len(b) - 8)
    i = 12
    while i + 8 <= len(b):
        cid, size = b[i:i + 4], struct.unpack_from("<I", b, i + 4)[0]
        if cid == b"data":
            b[i + 4:i + 8] = struct.pack("<I", len(b) - i - 8)
            break
        i += 8 + size + (size & 1)
    return bytes(b)


LIVE_ONLY = "Ableton pack format: only Live can read it"
_live_only = {}                 # (path, size, mtime) -> verdict; headers are read once


def live_only(path, st=None):
    """True for an AIFF in Ableton's own compression (AIFC, type 'able': the samples of Live's
    packs). No decoder outside Live reads those, ffmpeg included."""
    if not path.lower().endswith((".aif", ".aiff")):
        return False
    try:
        st = st or os.stat(path)
        key = (path, st.st_size, st.st_mtime)
        if key not in _live_only:
            with open(path, "rb") as f:
                b = f.read(4096)        # the COMM chunk sits past the first 512 bytes in some
            i = b.find(b"COMM")
            if len(_live_only) > 50000:
                _live_only.clear()
            _live_only[key] = b[:4] == b"FORM" and b[8:12] == b"AIFC" and i > 0 and b[i + 26:i + 30] == b"able"
        return _live_only[key]
    except OSError:
        return False


def ffmpeg_says(stderr):
    """ffmpeg's complaint as one plain line: no addresses, no second and third error"""
    for line in stderr.decode(errors="replace").splitlines():
        line = re.sub(r"^\[[^\]]*\]\s*", "", line.strip())
        if line:
            return line[:120]
    return "ffmpeg gave no reason"


def convert_for_daisy(path):
    """(wav bytes, how) ready for the Daisy: mono 16-bit 48 kHz via ffmpeg;
    without ffmpeg, a WAV goes as it is (the Daisy's parser takes PCM 8-32
    bit, float, any rate, up to 8 channels and mixes down itself)"""
    if live_only(path):
        raise ValueError(LIVE_ONLY)
    ff = shutil.which("ffmpeg")
    if ff:
        r = subprocess.run([ff, "-v", "error", "-nostdin", "-i", path, "-map_metadata", "-1",
                            "-fflags", "+bitexact", "-ac", "1", "-ar", "48000", "-sample_fmt", "s16",
                            "-f", "wav", "-"], capture_output=True, timeout=120)
        if r.returncode != 0 or len(r.stdout) < 44:
            raise ValueError("can't read this file: " + ffmpeg_says(r.stderr))
        return fix_wav_sizes(r.stdout), "mono 16-bit 48 kHz"
    if not path.lower().endswith((".wav", ".wave")):
        raise ValueError("only WAV without ffmpeg (sudo apt install ffmpeg)")
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file")
    return data, "as it is (no ffmpeg)"


def edit_wav(wav, start=0.0, end=1.0, norm=False):
    """A 16-bit PCM WAV cut to start..end (fractions of its length) and/or
    made as loud as it goes (peak at -0.3 dB). Cuts get a 2 ms fade so they
    don't click. Anything else than 16-bit PCM: ValueError."""
    i = wav.find(b"data", 12)
    if wav[:4] != b"RIFF" or i < 0 or struct.unpack_from("<H", wav, 34)[0] != 16 \
            or struct.unpack_from("<H", wav, 20)[0] != 1:
        raise ValueError("trim and normalise need ffmpeg for this file (sudo apt install ffmpeg)")
    ch = struct.unpack_from("<H", wav, 22)[0] or 1
    rate = struct.unpack_from("<I", wav, 24)[0] or 48000
    import array
    a = array.array("h")
    body = wav[i + 8:]
    a.frombytes(body[:len(body) // (2 * ch) * 2 * ch])
    if sys.byteorder == "big":
        a.byteswap()
    frames = len(a) // ch
    start = min(max(float(start), 0.0), 1.0)
    end = min(max(float(end), 0.0), 1.0)
    f0, f1 = int(frames * start), int(frames * end)
    if f1 - f0 < 16:
        raise ValueError("nothing left after the trim")
    a = a[f0 * ch:f1 * ch]
    n = len(a) // ch
    fade = min(rate // 500, n // 2)
    for k in range(fade):
        for c in range(ch):
            if f0 > 0:
                a[k * ch + c] = a[k * ch + c] * k // fade
            if f1 < frames:
                a[(n - 1 - k) * ch + c] = a[(n - 1 - k) * ch + c] * k // fade
    if norm:
        top = max(max(a), -min(a))
        if top > 0:
            g = 31650 / top               # -0.3 dB
            if abs(g - 1) > 0.01:
                a = array.array("h", [max(-32768, min(32767, int(v * g))) for v in a])
    if sys.byteorder == "big":
        a.byteswap()
    data = a.tobytes()
    head = bytearray(wav[:i + 8])
    struct.pack_into("<I", head, i + 4, len(data))
    struct.pack_into("<I", head, 4, len(head) - 8 + len(data))
    return bytes(head) + data


def edit_args(start, end, norm):
    """(start, end, norm) checked, or None if they make no sense"""
    try:
        start, end = float(start if start is not None else 0), float(end if end is not None else 1)
    except (TypeError, ValueError):
        return None
    if not (0 <= start < end <= 1):
        return None
    return start, end, norm in (True, 1, "1", "true")


PEAK_POINTS = 96
PREVIEW_MAX_FILE = 40 << 20     # bigger files: no waveform, no preview
PREVIEW_KEEP = 6                # converted files kept for playing again


def wav_peaks(wav):
    """(seconds, 96 peaks 0-255 scaled to the loudest) from a 16-bit mono WAV, or (0, None)"""
    i = wav.find(b"data", 12)
    if i < 0 or wav[:4] != b"RIFF":
        return 0.0, None
    rate = struct.unpack_from("<I", wav, 24)[0] or 48000
    ch = struct.unpack_from("<H", wav, 22)[0] or 1
    if struct.unpack_from("<H", wav, 34)[0] != 16:
        return 0.0, None
    body = memoryview(wav)[i + 8:]
    n = len(body) // 2
    if n < PEAK_POINTS:
        return n / rate / ch, None
    import array
    a = array.array("h")
    a.frombytes(body[:n * 2])
    if sys.byteorder == "big":
        a.byteswap()
    pk = []
    for k in range(PEAK_POINTS):
        seg = a[n * k // PEAK_POINTS:n * (k + 1) // PEAK_POINTS]
        step = max(1, len(seg) // 2000)
        pk.append(max(max(seg[::step]), -min(seg[::step])))
    top = max(pk) or 1
    return n / rate / ch, [min(255, v * 255 // top) for v in pk]


class Transfer:
    """One sample on its way to the Daisy"""

    def __init__(self, tid, src, rel, path, track, edit=None):
        self.id, self.src, self.rel, self.path, self.track = tid, src, rel, path, track
        self.edit = edit if edit and edit != (0.0, 1.0, False) else None
        self.name = os.path.basename(path)
        self.state = "queued"          # converting, sending, loading, done, failed
        self.error = None
        self.size = 0
        self.acked = 0
        self.chunks = 0
        self.resent = 0
        self.started = None
        self.finished = None
        self.how = ""
        self.capacity = None
        self.frames = None

    def info(self):
        t = (self.finished or now()) - self.started if self.started else 0
        return {"id": self.id, "name": self.name, "track": self.track, "state": self.state,
                "error": self.error, "bytes": self.size, "chunks": self.chunks, "acked": self.acked,
                "resent": self.resent, "seconds": round(t, 2), "how": self.how,
                "kb_per_s": round(self.acked * XFER_CHUNK / 1024 / t, 1) if t > 0 and self.acked else None,
                "cut": (self.frames is not None and self.capacity is not None and self.frames > self.capacity)}


class SamplesMixin:
    """Bridge methods for the sample library and transfers"""

    def samples_info(self):
        return {"library": self.library, "sdcard": self.sdcard if self.sd_ready() else None,
                "ffmpeg": bool(shutil.which("ffmpeg")), "daisy_ready": self.ctl >= 3,
                "current": self.xfer_cur.info() if self.xfer_cur else None,
                "queued": [t.info() for t in self.xfer_q],
                "recent": [t.info() for t in self.xfer_done]}

    def sd_ready(self):
        return bool(self.sdcard) and os.path.isdir(self.sdcard) and (
            os.path.ismount(self.sdcard) or self.args.sdcard_any)

    def roots(self, src):
        if src == "lib":
            os.makedirs(self.library, exist_ok=True)
            return self.library
        if src == "sd" and self.sd_ready():
            return self.sdcard
        return None

    def list_samples(self, src, rel):
        root = self.roots(src)
        p = safe_join(root, rel)
        if p is None or not os.path.isdir(p):
            return 400, {"error": "no such folder" if root else "no SD card in the box"}
        dirs, files, hidden = [], [], 0
        try:
            for e in sorted(os.scandir(p), key=lambda e: e.name.lower()):
                if e.name.startswith("."):
                    continue
                if e.is_dir(follow_symlinks=False):
                    dirs.append(e.name)
                elif e.is_file(follow_symlinks=False) and e.name.lower().endswith(AUDIO_EXT):
                    st = e.stat()
                    if live_only(e.path, st):       # nothing on the box can play it: not offered
                        hidden += 1
                    else:
                        files.append({"name": e.name, "size": st.st_size})
        except OSError as e:
            return 400, {"error": str(e)}
        rel = os.path.relpath(p, os.path.realpath(root))
        return 200, {"src": src, "path": "" if rel == "." else rel, "dirs": dirs[:500], "files": files[:2000], "live_only": hidden}

    async def preview(self, src, rel, want_wav, edit=None):
        """What the Daisy would get of a file: (code, peaks JSON) or (200, wav bytes).
        Converted once (ffmpeg), remembered: peaks for every file seen, the audio for the last few."""
        root = self.roots(src)
        p = safe_join(root, rel)
        if p is None or not os.path.isfile(p):
            return 400, {"error": "no such file"}
        st = os.stat(p)
        if st.st_size > PREVIEW_MAX_FILE:
            return 413, {"error": "too big to preview"}
        key = (p, st.st_mtime_ns, st.st_size)
        if not hasattr(self, "pv_peaks"):
            self.pv_peaks, self.pv_wav, self.pv_sem = collections.OrderedDict(), collections.OrderedDict(), asyncio.Semaphore(2)
        if key not in self.pv_peaks or (want_wav and key not in self.pv_wav):
            async with self.pv_sem:
                if key not in self.pv_peaks or (want_wav and key not in self.pv_wav):
                    try:
                        wav, how = await asyncio.get_running_loop().run_in_executor(None, convert_for_daisy, p)
                    except (ValueError, OSError, subprocess.SubprocessError) as e:
                        return 415, {"error": str(e)[:200]}
                    secs, pk = wav_peaks(wav)
                    self.pv_peaks[key] = {"secs": round(secs, 3), "peaks": bytes(pk).hex() if pk else None}
                    while len(self.pv_peaks) > 4000:
                        self.pv_peaks.popitem(last=False)
                    self.pv_wav[key] = wav
                    while len(self.pv_wav) > PREVIEW_KEEP:
                        self.pv_wav.popitem(last=False)
        if want_wav:
            self.pv_wav.move_to_end(key)
            wav = self.pv_wav[key]
            if edit and edit != (0.0, 1.0, False):
                try:
                    wav = await asyncio.get_running_loop().run_in_executor(None, edit_wav, wav, *edit)
                except ValueError as e:
                    return 415, {"error": str(e)}
            return 200, wav
        return 200, self.pv_peaks[key]

    def xfer_event(self, t):
        self.broadcast(sse_event("xfer", t.info()))

    def queue_send(self, src, rel, track, start=None, end=None, norm=False):
        root = self.roots(src)
        p = safe_join(root, rel)
        if p is None or not os.path.isfile(p):
            return 400, {"error": "no such file"}
        if not isinstance(track, int) or not 1 <= track <= 16:
            return 400, {"error": "track 1-16"}
        if self.ctl < 3:
            return 503, {"error": "the Daisy's firmware can't take samples yet (v13.8+)"}
        edit = edit_args(start, end, norm)
        if edit is None:
            return 400, {"error": "start and end: 0-1, start before end"}
        self.xfer_seq += 1
        t = Transfer(self.xfer_seq % 250 + 1, src, rel, p, track, edit)
        self.xfer_q.append(t)
        self.xfer_wake.set()
        self.xfer_event(t)
        return 200, {"ok": True, "id": t.id}

    async def await_reply(self, prefix, timeout):
        end = now() + timeout
        while True:
            left = end - now()
            if left <= 0:
                return None
            try:
                line = await asyncio.wait_for(self.xfer_replies.get(), left)
            except asyncio.TimeoutError:
                return None
            if line.startswith(prefix):
                return line

    async def xfer_task(self):
        while True:
            await self.xfer_wake.wait()
            self.xfer_wake.clear()
            while self.xfer_q:
                t = self.xfer_q.popleft()
                self.xfer_cur = t
                try:
                    await self.run_transfer(t)
                except Exception as e:     # one bad sample mustn't stop the bridge
                    log.exception("transfer %s", t.name)
                    t.state, t.error = "failed", str(e)[:200]
                t.finished = now()
                self.xfer_event(t)
                self.xfer_done.appendleft(t)
                self.xfer_cur = None

    async def run_transfer(self, t):
        t.state = "converting"
        self.xfer_event(t)
        data, t.how = await asyncio.get_running_loop().run_in_executor(None, convert_for_daisy, t.path)
        if t.edit:
            data = await asyncio.get_running_loop().run_in_executor(None, edit_wav, data, *t.edit)
            t.how += ", " + " + ".join(w for w, on in (("trimmed", t.edit[:2] != (0.0, 1.0)), ("normalised", t.edit[2])) if on)
        t.size = len(data)
        t.chunks = (len(data) + XFER_CHUNK - 1) // XFER_CHUNK
        frames = None
        try:
            if data[22:24] == b"\x01\x00" and data[34:36] == b"\x10\x00":   # mono 16-bit
                frames = (len(data) - 44) // 2
        except IndexError:
            pass
        t.frames = frames
        while not self.xfer_replies.empty():
            self.xfer_replies.get_nowait()
        name = "".join(c if c.isalnum() or c in "._-" else "_" for c in t.name)[:32]
        t.started = now()
        t.state = "sending"
        self.ctl_send(b"%sFO %d %d %d %s\n" % (ESC, t.id, t.track, t.size, name.encode()))
        r = await self.await_reply("FO %d " % t.id, 3)
        if not r or " ok" not in r:
            raise RuntimeError("Daisy refused: " + (r.split(" err ", 1)[1] if r and " err " in r else "no answer"))
        try:
            t.capacity = int(r.split()[3])
        except (IndexError, ValueError):
            pass
        self.xfer_event(t)
        acked = set()
        sent = {}          # seq -> (time, tries)
        nxt = 0
        last_event = now()

        def send(seq):
            chunk = data[seq * XFER_CHUNK:(seq + 1) * XFER_CHUNK]
            crc = binascii.crc32(chunk) & 0xFFFFFFFF
            tries = sent.get(seq, (0, 0))[1] + 1
            if tries > XFER_TRIES:
                raise RuntimeError("chunk %d failed %d times" % (seq, XFER_TRIES))
            if tries > 1:
                t.resent += 1
            sent[seq] = (now(), tries)
            self.data_send(b"%sFD %d %d %x %s\n" % (ESC, t.id, seq, crc, base64.b64encode(chunk)))

        while len(acked) < t.chunks:
            if self.fd is None or self.ctl < 3:
                raise RuntimeError("the Daisy went away")
            # fill the window
            while nxt < t.chunks and len(sent) - len(acked & sent.keys()) < XFER_WINDOW:
                if nxt not in acked:
                    send(nxt)
                nxt += 1
            # resend what timed out
            tnow = now()
            for seq, (ts, _) in list(sent.items()):
                if seq not in acked and tnow - ts > XFER_RESEND_S:
                    send(seq)
            try:
                line = await asyncio.wait_for(self.xfer_replies.get(), 0.1)
            except asyncio.TimeoutError:
                continue
            parts = line.split()
            if parts[0] != "FA" or len(parts) < 4 or parts[1] != str(t.id):
                continue
            seq = int(parts[2])
            if parts[3] == "ok":
                acked.add(seq)
                t.acked = len(acked)
            elif len(parts) > 4 and parts[4] == "stale":
                raise RuntimeError("the Daisy dropped the transfer")
            elif seq not in acked:
                send(seq)          # crc / length: send it again now
            if now() - last_event > 0.25:
                last_event = now()
                self.xfer_event(t)
        self.ctl_send(b"%sFC %d\n" % (ESC, t.id))
        r = await self.await_reply("FC %d " % t.id, 3)
        if not r or " ok" not in r:
            raise RuntimeError("commit: " + (r or "no answer"))
        t.state = "loading"
        self.xfer_event(t)
        r = await self.await_reply("FL %d " % t.id, 15)
        if not r or not r.endswith("ok"):
            raise RuntimeError("the Daisy couldn't load it" if r else "no answer while loading")
        t.state = "done"
        log.info("sample %s -> track %d: %d bytes in %.1f s (%d resent)", t.name, t.track, t.size,
                 now() - t.started, t.resent)

    def import_sd(self, rel):
        """Copy a file or a folder (audio only) from the SD card into the library"""
        root = self.roots("sd")
        src = safe_join(root, rel)
        if src is None or not os.path.exists(src):
            return 0
        dest_root = os.path.join(self.library, "sdcard")
        n = 0
        if os.path.isfile(src):
            items = [(src, os.path.join(dest_root, os.path.basename(src)))]
        else:
            items = []
            for d, _, fs in os.walk(src):
                for f in fs:
                    if f.lower().endswith(AUDIO_EXT) and not f.startswith(".") and not live_only(os.path.join(d, f)):
                        full = os.path.join(d, f)
                        items.append((full, os.path.join(dest_root, os.path.relpath(full, os.path.dirname(src)))))
        for a, b in items:
            os.makedirs(os.path.dirname(b), exist_ok=True)
            if not os.path.exists(b) or os.path.getsize(b) != os.path.getsize(a):
                shutil.copyfile(a, b + ".part")
                os.replace(b + ".part", b)
                n += 1
        return n

    async def upload(self, reader, n, name):
        """Raw body into the library's uploads folder"""
        name = os.path.basename(urllib.parse.unquote(name)).strip()
        if not name or name.startswith(".") or not name.lower().endswith(AUDIO_EXT):
            return 400, {"error": "name an audio file (?name=kick.wav)"}
        if not 0 < n <= UPLOAD_MAX:
            return 400, {"error": "empty or over %d MB" % (UPLOAD_MAX >> 20)}
        d = os.path.join(self.library, "uploads")
        os.makedirs(d, exist_ok=True)
        dest = os.path.join(d, name)
        part = dest + ".part"
        try:
            with open(part, "wb") as f:
                left = n
                while left:
                    chunk = await asyncio.wait_for(reader.read(min(left, 65536)), 30)
                    if not chunk:
                        raise ConnectionError("upload cut off")
                    f.write(chunk)
                    left -= len(chunk)
            os.replace(part, dest)
        except (OSError, ConnectionError, asyncio.TimeoutError) as e:
            try:
                os.remove(part)
            except OSError:
                pass
            return 400, {"error": str(e)}
        return 200, {"ok": True, "path": "uploads/" + name}
