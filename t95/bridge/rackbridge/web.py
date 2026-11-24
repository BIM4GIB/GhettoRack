"""HTTP, SSE /events, the JSON API, HTTPS certificate."""
from .common import *
from .util import *
from .samples import safe_join, edit_args


class Client:
    """One browser. Its own bounded queue: a slow one can't hold up anyone."""

    def __init__(self, writer, peer, view=""):
        self.writer = writer
        self.peer = peer
        self.view = view            # selector, apcbox...: picks the IR profile
        self.q = collections.deque()
        self.qbytes = 0
        self.wake = asyncio.Event()
        self.need_snapshot = True
        self.resyncs = 0
        self.closed = False

    def push(self, chunk):
        if self.need_snapshot:
            return              # the snapshot it gets next includes this
        if self.qbytes + len(chunk) > CLIENT_QUEUE_BYTES:
            # Too far behind: throw the backlog away, start it over from a snapshot
            self.q.clear()
            self.qbytes = 0
            self.need_snapshot = True
            self.resyncs += 1
        else:
            self.q.append(chunk)
            self.qbytes += len(chunk)
        self.wake.set()


def local_ips():
    ips = set()
    try:
        out = subprocess.run(["hostname", "-I"], capture_output=True, text=True, timeout=5).stdout
        ips.update(out.split())
    except (OSError, subprocess.SubprocessError):
        pass
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))     # no packet is sent; picks the outgoing address
            ips.add(s.getsockname()[0])
    except OSError:
        pass
    ips.add("127.0.0.1")
    return sorted(i for i in ips if ":" not in i)


def tls_context(directory):
    """Self-signed certificate for the HTTPS port (phones only give the page
    the microphone over HTTPS). Made once with openssl, kept in the state
    folder; a new one when the T95's address changes."""
    os.makedirs(directory, exist_ok=True)
    crt, key = os.path.join(directory, "tls.crt"), os.path.join(directory, "tls.key")
    ips = local_ips()
    host = socket.gethostname()
    want = ",".join(["DNS:" + host, "DNS:" + host + ".local"] + ["IP:" + i for i in ips])
    stamp = os.path.join(directory, "tls.san")
    have = open(stamp).read() if os.path.exists(stamp) else ""
    if not (os.path.exists(crt) and os.path.exists(key)) or have != want:
        r = subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "3650",
                            "-subj", "/CN=GhettoRack " + host, "-addext", "subjectAltName=" + want,
                            "-keyout", key, "-out", crt], capture_output=True, timeout=60)
        if r.returncode != 0:
            raise OSError("openssl: " + r.stderr.decode(errors="replace")[:200])
        os.chmod(key, 0o600)
        with open(stamp, "w") as f:
            f.write(want)
        log.info("made a TLS certificate for %s", want)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(crt, key)
    return ctx


class WebMixin:
    """Bridge methods for browsers"""

    def announce(self):
        self.broadcast(sse_event("bridge", self.status()))

    def broadcast(self, chunk):
        for c in list(self.clients):
            c.push(chunk)

    def state_json(self):
        have = {"daisy": self.fd is not None}
        return {"modules": [dict({k: m[k] for k in ("id", "name", "url", "ready", "needs", "about")},
                                 missing=[n for n in m["needs"] if not have.get(n)]) for m in MODULES],
                "module": self.module()["id"], "last": self.state.data["last"],
                "autostart_s": self.state.data["autostart_s"],
                "hdmi_audio": self.hdmi.info(),
                "ir": {"profiles": self.state.data["ir"], "actions": IR_ACTIONS,
                       "profile_names": IR_PROFILES, **self.ir.info()}}

    def post(self, path, body):
        """(code, json) for a POST. Small JSON bodies only."""
        try:
            d = json.loads(body.decode() or "{}")
            if not isinstance(d, dict):
                raise ValueError
        except ValueError:
            return 400, {"error": "JSON object expected"}
        if path == "/select":
            m = d.get("module")
            if m not in [x["id"] for x in MODULES if x["ready"]]:
                return 400, {"error": "unknown or unready module"}
            self.state.data["last"] = m
            self.state.save()
            self.refresh_daisy_ports()     # the module's routing applies now
            self.announce()
            return 200, {"ok": True}
        if path == "/settings":
            if "autostart_s" in d:
                a = d["autostart_s"]
                if not isinstance(a, int) or not 0 <= a <= 60:
                    return 400, {"error": "autostart_s: 0-60"}
                self.state.data["autostart_s"] = a
            if "hdmi_audio" in d:
                if not isinstance(d["hdmi_audio"], bool):
                    return 400, {"error": "hdmi_audio: true/false"}
                self.state.data["hdmi_audio"] = d["hdmi_audio"]
            if not ({"autostart_s", "hdmi_audio"} & d.keys()):
                return 400, {"error": "autostart_s or hdmi_audio"}
            self.state.save()
            return 200, {"ok": True}
        if path in ("/ir/learn", "/ir/forget"):
            prof, action, press = d.get("profile"), d.get("action"), d.get("press", "tap")
            if prof not in IR_PROFILES or action not in IR_ACTIONS or press not in ("tap", "hold"):
                return 400, {"error": "profile, action, press (tap/hold) expected"}
            if path == "/ir/forget":
                self.state.forget(prof, action, press)
            else:
                self.ir.learning = (prof, action, press, now() + IR_LEARN_S)
            return 200, {"ok": True}
        if path == "/ir/cancel":
            self.ir.learning = None
            return 200, {"ok": True}
        if path == "/routes":
            return self.set_routes(d.get("routes"))
        if path == "/do":
            return self.ui_do(d.get("a"))
        if path == "/clock":
            return self.clock.set(d)
        if path == "/clock/transport":
            return self.clock.transport(d.get("cmd"))
        if path == "/net/never":
            return self.net_set_never(d.get("name"), d.get("never"))
        if path in ("/presets/save", "/presets/load", "/presets/delete"):
            return self.preset(path.rsplit("/", 1)[1], d.get("name"))
        if path == "/samples/send":
            return self.queue_send(d.get("src", "lib"), d.get("path", ""), d.get("track"),
                                   d.get("start"), d.get("end"), d.get("norm", False))
        if path == "/samples/delete":
            p = safe_join(self.roots("lib"), d.get("path", ""))
            if p is None or not os.path.isfile(p):
                return 400, {"error": "no such file in the library"}
            os.remove(p)
            return 200, {"ok": True}
        if path == "/samples/cancel":
            n = len(self.xfer_q)
            self.xfer_q.clear()
            return 200, {"ok": True, "dropped": n}
        return 404, {"error": "not found"}

    # ---- Settings: the bridge has no rights; it asks the helper (t95/helper) ----
    SYS_CMDS = {"scan", "join", "confirm", "forget", "hostname", "bt", "bt_scan", "bt_pair", "bt_forget", "restart_bridge", "reboot", "poweroff",
                "pin_set", "pin_check", "hotspot", "hotspot_set"}

    def helper_ask(self, d, timeout=75):
        """One request to the settings helper. Blocking: call it from an executor"""
        import socket
        try:
            s = socket.socket(socket.AF_UNIX)
            s.settimeout(timeout)
            s.connect(self.args.helper_socket)
            s.sendall(json.dumps(d).encode() + b"\n")
            buf = b""
            while not buf.endswith(b"\n") and len(buf) < 65536:
                c = s.recv(65536)
                if not c:
                    break
                buf += c
            s.close()
            out = json.loads(buf.decode())
            return out if isinstance(out, dict) else {"error": "the settings helper answered nonsense"}
        except (OSError, ValueError):
            return {"error": "the settings helper isn't running on the T95 (sudo t95/bridge/install.sh)", "helper": False}

    async def sys_do(self, body, secure):
        try:
            d = json.loads(body.decode() or "{}")
        except ValueError:
            d = None
        if not isinstance(d, dict) or d.get("cmd") not in self.SYS_CMDS:
            return 400, {"error": "unknown settings command"}
        if not secure and any(k in d for k in ("pin", "psk", "old", "new")):
            return 400, {"error": "PINs and passwords only over the https:// address of this page", "https": True}
        out = await asyncio.get_running_loop().run_in_executor(None, self.helper_ask, d)
        return (200 if "error" not in out else 503 if out.get("helper") is False else 400), out

    async def handle_http(self, reader, writer):
        peer = writer.get_extra_info("peername")
        try:
            head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionError):
            writer.close()
            return
        try:
            method, target = head.split(b"\r\n", 1)[0].decode("latin-1").split(" ")[:2]
        except ValueError:
            method, target = "", ""
        path, _, query = target.partition("?")
        params = dict(kv.partition("=")[::2] for kv in query.split("&") if kv)
        try:
            if method == "POST":
                n = 0
                for h in head.decode("latin-1").split("\r\n")[1:]:
                    k, _, v = h.partition(":")
                    if k.strip().lower() == "content-length":
                        n = int(v.strip() or 0)
                if path == "/samples/upload":
                    code, out = await self.upload(reader, n, params.get("name", ""))
                    await self.reply(writer, code, "application/json", json.dumps(out).encode() + b"\n")
                    writer.close()
                    return
                if path == "/samples/import":
                    body = await asyncio.wait_for(reader.readexactly(min(n, POST_MAX)), 10) if n else b""
                    try:
                        rel = json.loads(body.decode() or "{}").get("path", "")
                    except (ValueError, AttributeError):
                        rel = None
                    if rel is None or not self.sd_ready():
                        code, out = 400, {"error": "no SD card in the T95" if rel is not None else "JSON expected"}
                    else:
                        cnt = await asyncio.get_running_loop().run_in_executor(None, self.import_sd, rel)
                        code, out = 200, {"ok": True, "copied": cnt}
                    await self.reply(writer, code, "application/json", json.dumps(out).encode() + b"\n")
                    writer.close()
                    return
                if n > POST_MAX:
                    raise ValueError
                body = await asyncio.wait_for(reader.readexactly(n), 10) if n else b""
                if path == "/sys/do":
                    secure = writer.get_extra_info("ssl_object") is not None or (peer and peer[0] in ("127.0.0.1", "::1"))
                    code, out = await self.sys_do(body, secure)
                    await self.reply(writer, code, "application/json", json.dumps(out).encode() + b"\n")
                    writer.close()
                    return
                code, out = self.post(path, body)
                await self.reply(writer, code, "application/json", json.dumps(out).encode() + b"\n")
            elif method != "GET":
                await self.reply(writer, 405, "text/plain", b"GET or POST\n")
            elif path in ("/", "/index.html"):
                await self.reply(writer, 200, "text/html; charset=utf-8", self.selector)
            elif path == "/clock" and "json" not in params:
                await self.reply(writer, 200, "text/html; charset=utf-8", self.pages["clock"])
            elif path == "/clock":
                body = dict(self.clock.info(), ports=[p.name for p in sorted(self.ports.values(), key=lambda p: p.num)])
                await self.reply(writer, 200, "application/json", json.dumps(body, indent=1).encode() + b"\n")
            elif path == "/audio.pcm":
                code, why = await self.listen.serve(writer)
                if code:
                    await self.reply(writer, code, "text/plain", (why or "no audio").encode() + b"\n")
            elif path == "/midihub":
                await self.reply(writer, 200, "text/html; charset=utf-8", self.hub)
            elif path == "/apcbox" and self.args.apcbox_page:
                await self.reply(writer, 200, "text/html; charset=utf-8", self.pages["apcbox"])
            elif path in ("/apcbox", "/classic", "/apcbox-ui.html"):
                await self.reply(writer, 200, "text/html; charset=utf-8", self.page)
            elif path == "/ui":
                # The page's encoder buttons (same URLs as the Mac build)
                a = params.get("a", "")
                act = {"push": "push", "back": "back"}.get(a)
                if a == "turn":
                    try:
                        d = max(-16, min(16, int(params.get("d", "0"))))
                        act = "turn %d" % d if d else None
                    except ValueError:
                        act = None
                if self.ctl < 2:
                    await self.reply(writer, 503, "text/plain", b"the Daisy's firmware has no UI command (v13.7+)\n")
                elif act and self.ui_send(act):
                    await self.reply(writer, 200, "text/plain", b"ok\n")
                else:
                    await self.reply(writer, 404, "text/plain", b"unknown UI action\n")
            elif path == "/samples":
                code, out = self.list_samples(params.get("src", "lib"),
                                              urllib.parse.unquote(params.get("path", "")))
                await self.reply(writer, code, "application/json", json.dumps(out).encode() + b"\n")
            elif path in ("/samples/peaks", "/samples/preview"):
                wav = path.endswith("preview")
                edit = edit_args(params.get("start"), params.get("end"), params.get("norm"))
                if edit is None:
                    code, out = 400, {"error": "start and end: 0-1, start before end"}
                else:
                    code, out = await self.preview(params.get("src", "lib"),
                                                   urllib.parse.unquote(params.get("path", "")), wav, edit)
                if code == 200 and wav:
                    await self.reply(writer, 200, "audio/wav", out)
                else:
                    await self.reply(writer, code, "application/json", json.dumps(out).encode() + b"\n")
            elif path == "/routes":
                await self.reply(writer, 200, "application/json", json.dumps(self.routes_json(), indent=1).encode() + b"\n")
            elif path == "/state":
                await self.reply(writer, 200, "application/json", json.dumps(self.state_json(), indent=1).encode() + b"\n")
            elif path == "/settings":
                await self.reply(writer, 200, "text/html; charset=utf-8", self.pages["settings"])
            elif path == "/sys/state":
                out = await asyncio.get_running_loop().run_in_executor(None, self.helper_ask, {"cmd": "status"})
                out["bridge"] = {"daisy": self.fd is not None, "control": self.ctl, "firmware": self.firmware,
                                 "controllers": [p.name for p in sorted(self.ports.values(), key=lambda p: p.num)],
                                 "autostart_s": self.state.data.get("autostart_s", 0), "ble_midi": self.ble.info()}
                await self.reply(writer, 200, "application/json", json.dumps(out).encode() + b"\n")
            elif path == "/health":
                await self.reply(writer, 200, "application/json", json.dumps(self.status(), indent=1).encode() + b"\n")
            elif path == "/caps":
                caps = {"source": "daisy-bridge", "bridge": 7, "control": self.ctl >= 2, "control_version": self.ctl, "midi": True, "ir": True,
                        "samples": True, "https_port": self.args.https_port,
                        "selector": "/", "ui": self.ctl >= 2, "tft": False}
                await self.reply(writer, 200, "application/json", json.dumps(caps).encode() + b"\n")
            elif path == "/events":
                await self.stream(reader, writer, peer, params.get("view", "")[:32])
                return
            else:
                await self.reply(writer, 404, "text/plain", b"not found\n")
        except (ConnectionError, asyncio.TimeoutError, asyncio.IncompleteReadError, ValueError):
            pass
        writer.close()

    @staticmethod
    async def reply(writer, code, ctype, body):
        reason = {200: "OK", 400: "Bad Request", 404: "Not Found", 405: "Method Not Allowed",
                  503: "Service Unavailable"}.get(code, "Error")
        writer.write(("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n" % (code, reason, ctype, len(body))).encode() + body)
        await asyncio.wait_for(writer.drain(), 10)

    async def stream(self, reader, writer, peer, view=""):
        if len(self.clients) >= MAX_CLIENTS:
            self.rejected += 1
            await self.reply(writer, 503, "text/plain", b"too many clients\n")
            writer.close()
            return
        c = Client(writer, peer, view)
        self.clients.add(c)

        async def watch_eof():
            # A browser that leaves while the Daisy is quiet must free its slot now,
            # not at the next write
            try:
                while await reader.read(4096):
                    pass
            except (ConnectionError, OSError):
                pass
            c.closed = True
            c.wake.set()
        eof = asyncio.create_task(watch_eof())
        log.info("browser %s connected (%d)", peer, len(self.clients))
        # The Daisy reports around a hundred lines a second. Written as they come, that is a
        # hundred small packets a second to every browser on the Wi-Fi, and on the T95 that
        # alone took the link from a few ms to a second per packet (rig, 2026-11-24: one page
        # open on a laptop, ping to the router 3 ms with the bridge stopped, 900 ms with it).
        # So a browser on the network gets what has gathered every flush_ms in one write.
        # The page on the box's own screen (loopback) is not paced.
        host = str(peer[0]) if isinstance(peer, (tuple, list)) and peer else str(peer)
        local = host.startswith("127.") or host in ("::1", "localhost")
        pace = getattr(self.args, "flush_ms", 0) / 1000 if (not local or getattr(self.args, "flush_local", False)) else 0
        try:
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                         b"Cache-Control: no-store\r\nConnection: keep-alive\r\n"
                         b"X-Accel-Buffering: no\r\n\r\nretry: 1000\n\n")
            last_ping = now()
            while not c.closed:
                if c.need_snapshot:
                    c.need_snapshot = False
                    snap = b"".join(sse_data(l) for l in self.cache.snapshot())
                    writer.write(sse_event("bridge", self.status()) + snap)
                if c.q:
                    # one write for all of it: asyncio sends the first write at once (no Nagle),
                    # so line by line the first line went out as a packet of its own
                    out = b"".join(c.q)
                    c.q.clear()
                    c.qbytes = 0
                    writer.write(out)
                # Only this browser's task waits here; the serial reader never does
                await asyncio.wait_for(writer.drain(), 30)
                if now() - last_ping > 15:
                    writer.write(b": ping\n\n")
                    last_ping = now()
                if pace:
                    await asyncio.sleep(pace)
                c.wake.clear()
                if not c.q and not c.need_snapshot and not c.closed:
                    try:
                        await asyncio.wait_for(c.wake.wait(), 15)
                    except asyncio.TimeoutError:
                        pass
        except (ConnectionError, asyncio.TimeoutError, OSError):
            pass
        finally:
            eof.cancel()
            self.clients.discard(c)
            self.client_resyncs += c.resyncs
            c.closed = True
            writer.close()
            log.info("browser %s left (%d)", peer, len(self.clients))
