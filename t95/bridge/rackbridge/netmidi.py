"""Network MIDI: RTP-MIDI / AppleMIDI sessions, found and joined without setup.

What a Mac (Audio MIDI Setup > Network), iPad apps, rtpMIDI on Windows and
the AliExpress boxes speak. Two UDP ports: control (5004) and data (5005).
The T95 is announced over Bonjour (Avahi service file, see install.sh), and
every _apple-midi._udp session seen on the LAN is invited, so a jam needs
no clicking. A device marked "never" is neither invited nor accepted.

Each session is a port like a USB one (shares the 1..8 numbers) and goes
through the router. No recovery journal: on a home LAN, Wi-Fi loss is rare,
and a stuck note is cleared by the release-on-drop in the router.
"""
from .common import *
from .util import *
from .midi import MidiParser, data_len
from .web import local_ips

AM_SIG = 0xFFFF
AM_VERSION = 2
NET_CK_S = 10.0          # clock sync from us (as initiator) this often
NET_SILENT_S = 60.0      # no packet from a session for this long: gone
NET_INVITE_S = 3.0       # invite retry
NET_BROWSE_S = 10.0      # avahi-browse this often
NET_PACKET_MAX = 1000    # MIDI bytes per RTP packet we send


def ts10k():
    """AppleMIDI clock: 100 us units"""
    return int(time.monotonic() * 10000)


def am_packet(cmd, token, ssrc, name):
    return struct.pack(">H2sIII", AM_SIG, cmd, AM_VERSION, token, ssrc) + name.encode()[:63] + b"\0"


def am_parse(data):
    """(cmd, fields) for an AppleMIDI command packet, or None"""
    if len(data) < 4 or data[0] != 0xFF or data[1] != 0xFF:
        return None
    cmd = data[2:4]
    if cmd in (b"IN", b"OK", b"NO", b"BY") and len(data) >= 16:
        ver, token, ssrc = struct.unpack(">III", data[4:16])
        name = data[16:].split(b"\0", 1)[0].decode("utf-8", "replace")
        return cmd, {"token": token, "ssrc": ssrc, "name": name}
    if cmd == b"CK" and len(data) >= 36:
        ssrc, count = struct.unpack(">IB", data[4:9])
        ts = struct.unpack(">QQQ", data[12:36])
        return cmd, {"ssrc": ssrc, "count": count, "ts": list(ts)}
    if cmd == b"RS" and len(data) >= 12:
        return cmd, {"ssrc": struct.unpack(">I", data[4:8])[0]}
    return None


def ck_packet(ssrc, count, ts):
    return struct.pack(">H2sIB3xQQQ", AM_SIG, b"CK", ssrc, count, *ts)


def rtp_midi_decode(data):
    """(ssrc, seq, raw MIDI bytes) from an RTP-MIDI packet, or None.
    Delta times are dropped; sysex segments are joined back into F0..F7."""
    if len(data) < 13 or data[0] & 0xC0 != 0x80 or data[1] & 0x7F != 0x61:
        return None
    seq, _, ssrc = struct.unpack(">HII", data[2:12])
    i = 12
    h = data[i]
    if h & 0x80:                          # B: 12-bit length
        if len(data) < 14:
            return None
        n = ((h & 0x0F) << 8) | data[i + 1]
        i += 2
    else:
        n = h & 0x0F
        i += 1
    z = h & 0x20
    body = data[i:i + n]
    out = bytearray()
    j, status, first = 0, 0, True
    while j < len(body):
        if not first or z:                # delta time: 1-4 bytes
            k = 0
            while j < len(body) and body[j] & 0x80 and k < 3:
                j += 1
                k += 1
            j += 1
            if j >= len(body):
                break
        first = False
        b = body[j]
        if b in (0xF0, 0xF7):             # sysex start, continuation or end segment
            e = j + 1
            while e < len(body) and body[e] < 0x80:
                e += 1
            end = body[e] if e < len(body) else 0xF7
            if b == 0xF0:
                out.append(0xF0)
            out += body[j + 1:e]
            if end == 0xF7:
                out.append(0xF7)
            # end == 0xF0: more segments follow (in this packet or the next); 0xF4: cancelled
            if end == 0xF4:
                out.append(0xF7)          # close it; the parser keeps it whole or drops it
            j = e + 1
            status = 0
            continue
        if b >= 0xF8:
            out.append(b)
            j += 1
            continue
        if b >= 0xF0:                     # system common: length by type
            ln = {0xF1: 1, 0xF2: 2, 0xF3: 1}.get(b, 0)
            out += body[j:j + 1 + ln]
            j += 1 + ln
            status = 0
            continue
        if b >= 0x80:
            status = b
            j += 1
        elif not status:
            break                         # data with no status: give up on the rest
        ln = data_len(status)
        out.append(status)
        out += body[j:j + ln]
        j += ln
    return ssrc, seq, bytes(out)


def rtp_midi_packet(seq, ssrc, msgs):
    """One RTP-MIDI packet with these complete messages, delta time 0, no journal"""
    body = bytearray()
    for k, m in enumerate(msgs):
        if k:
            body.append(0)                # delta time 0
        body += m
    n = len(body)
    if n > 15:
        hdr = bytes([0x80 | (n >> 8), n & 0xFF])
    else:
        hdr = bytes([n])
    return struct.pack(">BBHII", 0x80, 0x61, seq & 0xFFFF, ts10k() & 0xFFFFFFFF, ssrc) + hdr + bytes(body)


def parse_avahi(text):
    """avahi-browse -rpt output: [(name, address, port)] for IPv4 _apple-midi._udp services"""
    found = []
    for line in text.splitlines():
        f = line.split(";")
        if len(f) >= 9 and f[0] == "=" and f[2] == "IPv4" and f[4] == "_apple-midi._udp":
            try:
                found.append((avahi_unescape(f[3]), f[7], int(f[8])))
            except ValueError:
                pass
    return found


def avahi_unescape(s):
    """avahi-browse -p escapes ; and non-ASCII as \\DDD (decimal bytes)"""
    out = bytearray()
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 3 < len(s) + 1 and s[i + 1:i + 4].isdigit():
            out.append(int(s[i + 1:i + 4]))
            i += 4
        else:
            out += s[i].encode()
            i += 1
    return out.decode("utf-8", "replace")


class NetPort:
    """One RTP-MIDI session, used by the router like a USB port"""

    def __init__(self, num, name, peer, ssrc, initiator):
        self.num, self.name = num, name
        self.key = "net:%s:%d" % peer
        self.path = "rtp-midi %s:%d" % peer
        self.fd = None
        self.net = True
        self.peer = peer                  # (host, control port); data goes to port + 1
        self.ssrc = ssrc                  # theirs
        self.initiator = initiator        # we invited it
        self.parser = MidiParser(SYSEX_PORT_MAX)
        self.out_sysex = collections.deque()
        self.out_fifo = collections.deque()
        self.out_led = collections.OrderedDict()
        self.out_buf = b""
        self.writing = False
        self.announced = 0.0
        self.held = set()
        self.in_msgs = self.out_msgs = 0
        self.dropped_in = self.dropped_out = self.coalesced = self.unrouted = 0
        self.seq = 0                      # ours
        self.last_seen = now()
        self.last_ck = 0.0
        self.latency_ms = None
        self.lost = 0
        self.rx_seq = None
        self.lock = threading.Lock()

    def info(self, kinds):
        return {"port": self.num, "name": self.name, "path": self.path, "kind": kinds.get(self.num, "network"),
                "in": self.in_msgs, "out": self.out_msgs, "dropped_in": self.dropped_in,
                "dropped_out": self.dropped_out, "coalesced": self.coalesced, "unrouted": self.unrouted,
                "realtime_dropped": self.parser.realtime, "bad_in": self.parser.dropped,
                "net": {"initiator": self.initiator, "latency_ms": self.latency_ms, "lost": self.lost}}


class _Proto(asyncio.DatagramProtocol):
    def __init__(self, bridge, data):
        self.bridge, self.data = bridge, data

    def datagram_received(self, pkt, addr):
        try:
            self.bridge.net_packet(pkt, addr, self.data)
        except Exception:          # one bad packet must not stop the bridge
            log.exception("network MIDI packet from %s", addr)

    def error_received(self, exc):
        log.debug("network MIDI: %s", exc)


class NetMixin:
    """Bridge methods for network MIDI"""

    def net_init(self):
        self.net_ssrc = struct.unpack(">I", os.urandom(4))[0]
        self.net_name = self.args.net_name or "GhettoRack"
        self.net_ctl = self.net_data = None
        self.net_data_dup = None
        self.net_pending = {}         # (host, cport) -> {"name", "token", "stage", "sent", "tries"}
        self.net_seen = {}            # (host, cport) -> {"name", "seen"}  (discovered)
        self.net_avahi = None         # None: not tried; False: no avahi-browse; True: works
        self.net_refused = 0
        self.net_my_ips = None
        self.net_static = []
        for x in self.args.net_peer or []:
            addr, _, name = x.partition("=")
            host, _, port = addr.rpartition(":")
            self.net_static.append((name or addr, host, int(port)))

    def net_never(self):
        return set(self.state.data.get("net_never", []))

    def net_session(self, host, ssrc=None, cport=None):
        for p in self.ports.values():
            if getattr(p, "net", False) and p.peer[0] == host and (
                    (ssrc is not None and p.ssrc == ssrc) or (cport is not None and p.peer[1] == cport)):
                return p
        return None

    def net_send(self, pkt, addr, data):
        sock = self.net_data if data else self.net_ctl
        if sock is not None:
            sock.sendto(pkt, addr)

    def net_packet(self, pkt, addr, data):
        host, port = addr[0], addr[1]
        if pkt[:2] != b"\xff\xff":
            if data:
                self.net_rtp(pkt, host, port - 1)
            return
        r = am_parse(pkt)
        if r is None:
            return
        cmd, f = r
        cport = port - 1 if data else port
        if cmd == b"IN":
            if f["name"] in self.net_never():
                self.net_refused += 1
                self.net_send(am_packet(b"NO", f["token"], self.net_ssrc, self.net_name), addr, data)
                return
            if data:
                self.net_open(host, cport, f["ssrc"], f["name"], initiator=False)
            else:
                self.net_pending.pop((host, cport), None)   # they invited us: take theirs
            self.net_send(am_packet(b"OK", f["token"], self.net_ssrc, self.net_name), addr, data)
        elif cmd == b"OK":
            inv = self.net_pending.get((host, cport))
            if inv is None or inv["token"] != f["token"]:
                return
            if not data:
                inv.update(stage="data", tries=0, sent=0.0, name=f["name"] or inv["name"])
                self.net_invite_step((host, cport), inv)
            else:
                self.net_pending.pop((host, cport), None)
                p = self.net_open(host, cport, f["ssrc"], inv["name"], initiator=True)
                if p is not None:
                    self.net_ck(p)
        elif cmd == b"NO":
            inv = self.net_pending.get((host, cport))
            if inv is not None:
                log.info("network MIDI: %s refused our invitation", inv["name"])
                inv.update(stage="refused", sent=now())
        elif cmd == b"BY":
            p = self.net_session(host, cport=cport) or self.net_session(host, ssrc=f["ssrc"])
            if p is not None:
                self.net_close(p, ": said goodbye")
        elif cmd == b"CK" and data:
            p = self.net_session(host, cport=cport) or self.net_session(host, ssrc=f["ssrc"])
            if p is None:
                return
            p.last_seen = now()
            ts = f["ts"]
            if f["count"] == 0:
                ts[1] = ts10k()
                self.net_send(ck_packet(self.net_ssrc, 1, ts), addr, True)
            elif f["count"] == 1:
                ts[2] = ts10k()
                p.latency_ms = round((ts[2] - ts[0]) / 20.0, 2)   # half the round trip
                self.net_send(ck_packet(self.net_ssrc, 2, ts), addr, True)
            elif f["count"] == 2:
                p.latency_ms = round((ts10k() - ts[1]) / 20.0, 2)

    def net_open(self, host, cport, ssrc, name, initiator):
        p = self.net_session(host, cport=cport)
        if p is not None:                # same peer again (a re-invite): keep the port
            p.ssrc, p.last_seen = ssrc, now()
            return p
        free = [n for n in range(1, MAX_PORTS + 1) if n not in self.ports]
        if not free:
            log.warning("network MIDI: no free port for %s", name)
            return None
        base = clean_net_name(name)
        taken = {q.name for q in self.ports.values()}
        nm, k = base, 2
        while nm in taken:
            nm = "%s %d" % (base[:29], k)
            k += 1
        p = NetPort(free[0], nm, (host, cport), ssrc, initiator)
        self.ports[p.num] = p
        log.info("network MIDI session %s (%s:%d): port %d", nm, host, cport, p.num)
        self.refresh_daisy_ports()
        self.announce()
        return p

    def net_close(self, p, why="", bye=False):
        if bye:
            for data in (False, True):
                self.net_send(am_packet(b"BY", 0, self.net_ssrc, self.net_name),
                              (p.peer[0], p.peer[1] + data), data)
        self.release_held(p)
        self.ports.pop(p.num, None)
        log.info("network MIDI session %s gone%s", p.name, why)
        if p.num in self.daisy_ports:
            self.daisy_ports.discard(p.num)
            self.ctl_send(b"%sN%d -\n" % (ESC, p.num))
        self.announce()

    def net_rtp(self, pkt, host, cport):
        r = rtp_midi_decode(pkt)
        if r is None:
            return
        ssrc, seq, raw = r
        p = self.net_session(host, cport=cport) or self.net_session(host, ssrc=ssrc)
        if p is None:
            return
        p.last_seen = now()
        if p.rx_seq is not None:
            gap = (seq - p.rx_seq - 1) & 0xFFFF
            if 0 < gap < 1000:
                p.lost += gap
        p.rx_seq = seq
        for msg in p.parser.feed(raw):
            p.in_msgs += 1
            self.route(p, msg)

    def net_flush(self, p):
        """Everything waiting for a network port, in as few packets as fit"""
        msgs, size = [], 0
        while p.out_sysex or p.out_fifo or p.out_led:
            if p.out_sysex:
                m = p.out_sysex.popleft()
            elif p.out_fifo:
                m = p.out_fifo.popleft()
            else:
                m = p.out_led.popitem(last=False)[1]
            if msgs and size + len(m) + 1 > NET_PACKET_MAX:
                self.net_rtp_send(p, msgs)
                msgs, size = [], 0
            msgs.append(m)
            size += len(m) + 1
            p.out_msgs += 1
        if msgs:
            self.net_rtp_send(p, msgs)

    def net_rtp_send(self, p, msgs):
        with p.lock:                  # the clock thread sends too
            p.seq = (p.seq + 1) & 0xFFFF
            pkt = rtp_midi_packet(p.seq, self.net_ssrc, msgs)
        self.net_send(pkt, (p.peer[0], p.peer[1] + 1), True)

    def net_send_threadsafe(self, p, msg):
        """From the clock thread: asyncio transports aren't thread-safe, a dup of the data socket is"""
        if self.net_data_dup is None:
            return False
        with p.lock:
            p.seq = (p.seq + 1) & 0xFFFF
            pkt = rtp_midi_packet(p.seq, self.net_ssrc, [msg])
        try:
            self.net_data_dup.sendto(pkt, (p.peer[0], p.peer[1] + 1))
            return True
        except OSError:
            return False

    def net_ck(self, p):
        p.last_ck = now()
        self.net_send(ck_packet(self.net_ssrc, 0, [ts10k(), 0, 0]), (p.peer[0], p.peer[1] + 1), True)

    def net_invite(self, name, host, cport):
        key = (host, cport)
        if key in self.net_pending or self.net_session(host, cport=cport) is not None:
            return
        if name in self.net_never():
            return
        self.net_pending[key] = {"name": name, "token": struct.unpack(">I", os.urandom(4))[0],
                                 "stage": "control", "sent": 0.0, "tries": 0}
        self.net_invite_step(key, self.net_pending[key])

    def net_invite_step(self, key, inv):
        inv["sent"] = now()
        inv["tries"] += 1
        data = inv["stage"] == "data"
        self.net_send(am_packet(b"IN", inv["token"], self.net_ssrc, self.net_name),
                      (key[0], key[1] + data), data)

    def net_is_me(self, name, host, port):
        if port != self.args.rtp_port:
            return False
        if self.net_my_ips is None or now() - self.net_my_ips[0] > 60:
            self.net_my_ips = (now(), set(local_ips()))
        return host in self.net_my_ips[1]

    async def net_browse(self):
        if self.net_avahi is False:
            return []
        try:
            proc = await asyncio.create_subprocess_exec(
                "avahi-browse", "-rpt", "_apple-midi._udp",
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
            out, _ = await asyncio.wait_for(proc.communicate(), 8)
        except FileNotFoundError:
            if self.net_avahi is None:
                log.warning("network MIDI: no avahi-browse, so no discovery (apt install avahi-utils)")
            self.net_avahi = False
            return []
        except (asyncio.TimeoutError, OSError) as e:
            log.debug("avahi-browse: %s", e)
            return []
        self.net_avahi = True
        return parse_avahi(out.decode("utf-8", "replace"))

    def net_info(self):
        if self.net_ctl is None:
            return {"on": False}
        t = now()
        return {"on": True, "port": self.args.rtp_port, "name": self.net_name, "avahi": self.net_avahi,
                "sessions": [p.name for p in self.ports.values() if getattr(p, "net", False)],
                "inviting": [v["name"] for v in self.net_pending.values() if v["stage"] != "refused"],
                "seen": [{"name": v["name"], "host": k[0], "port": k[1], "age_s": round(t - v["seen"], 1)}
                         for k, v in sorted(self.net_seen.items())],
                "never": sorted(self.net_never()), "refused": self.net_refused}

    def net_set_never(self, name, never):
        if not isinstance(name, str) or not 0 < len(name) <= 64 or not isinstance(never, bool):
            return 400, {"error": "name and never (true/false)"}
        s = self.net_never()
        if never:
            s.add(name)
            for p in list(self.ports.values()):
                if getattr(p, "net", False) and p.name == clean_net_name(name):
                    self.net_close(p, ": marked never", bye=True)
            for k in [k for k, v in self.net_pending.items() if v["name"] == name]:
                del self.net_pending[k]
        else:
            s.discard(name)
        self.state.data["net_never"] = sorted(s)[:64]
        self.state.save()
        self.announce()
        return 200, {"ok": True}

    async def net_task(self):
        if not self.args.rtp_port:
            return
        loop = asyncio.get_running_loop()
        try:
            self.net_ctl, _ = await loop.create_datagram_endpoint(
                lambda: _Proto(self, False), local_addr=(self.args.host, self.args.rtp_port))
            self.net_data, _ = await loop.create_datagram_endpoint(
                lambda: _Proto(self, True), local_addr=(self.args.host, self.args.rtp_port + 1))
            sock = self.net_data.get_extra_info("socket")
            self.net_data_dup = socket.socket(fileno=os.dup(sock.fileno()))
        except OSError as e:
            log.warning("network MIDI off: ports %d/%d: %s", self.args.rtp_port, self.args.rtp_port + 1, e)
            self.net_ctl = self.net_data = None
            return
        log.info("network MIDI (RTP-MIDI) on udp %d/%d as %s", self.args.rtp_port, self.args.rtp_port + 1,
                 self.net_name)
        last_browse = -NET_BROWSE_S
        try:
            while True:
                t = now()
                if t - last_browse >= NET_BROWSE_S:
                    last_browse = t
                    found = [] if self.args.no_browse else await self.net_browse()
                    t = now()
                    for name, host, port in found + self.net_static:
                        if not self.net_is_me(name, host, port):
                            self.net_seen[(host, port)] = {"name": name, "seen": t}
                    for k in [k for k, v in self.net_seen.items() if t - v["seen"] > 3 * NET_BROWSE_S]:
                        del self.net_seen[k]
                    for (host, port), v in list(self.net_seen.items()):
                        self.net_invite(v["name"], host, port)
                for key, inv in list(self.net_pending.items()):
                    if inv["stage"] == "refused":
                        if t - inv["sent"] > 30:          # ask again later
                            del self.net_pending[key]
                    elif inv["tries"] >= 5:
                        del self.net_pending[key]         # gone quiet; the next browse tries again
                    elif t - inv["sent"] >= NET_INVITE_S:
                        self.net_invite_step(key, inv)
                for p in [p for p in self.ports.values() if getattr(p, "net", False)]:
                    if t - p.last_seen > NET_SILENT_S:
                        self.net_close(p, ": silent")
                    elif p.initiator and t - p.last_ck >= (1.5 if t - p.last_seen < 5 and p.latency_ms is None
                                                           else NET_CK_S):
                        self.net_ck(p)
                await asyncio.sleep(0.5)
        finally:
            for p in [p for p in self.ports.values() if getattr(p, "net", False)]:
                self.net_close(p, ": bridge stopping", bye=True)


def clean_net_name(name):
    name = "".join(c if 0x20 <= ord(c) < 0x7F else "?" for c in name).strip()
    return (name or "network")[:32]
