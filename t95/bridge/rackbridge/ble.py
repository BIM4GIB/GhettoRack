"""Bluetooth LE MIDI: paired devices become controller ports.

This kernel has no ALSA sequencer, so BlueZ's own MIDI plug-in can't make a port.
The bridge does it: for every paired device that offers the MIDI service it opens
its own ATT channel (an L2CAP socket, no BlueZ in between), and puts a
pseudo-terminal in front of it: plain MIDI bytes on the bridge's side, Bluetooth
MIDI packets on the radio's. The rest of the bridge sees one more raw MIDI device
(`midi_candidates`).

Why not BlueZ's GATT calls (rig, 2026-11-24, an iPad with a synth app):
- The T95's radio (UWE5622) can't reconnect a paired device the way BlueZ does it.
  BlueZ asks for encryption some 90 ms after the link is up, while the iPad is still
  negotiating the link; the request is never answered and the link drops 5 to 12 s
  later (six tries of six). Asked 3 s after connecting, encryption is done in 0.3 s
  (four of four). So: connect unencrypted, wait, then encrypt (`BLE_SETTLE_S`).
- With "address resolution" on, the radio reports a private address (iPad, iPhone,
  Mac) as all zeros, so the kernel never sees the device it waits for. It
  is off while BlueZ scans: the connection is asked for, then a scan started.
- BlueZ refused every write to the iPad's MIDI characteristic (NotAuthorized, as
  root too). Not understood; our own channel doesn't ask BlueZ.

Pairing is not done here: that changes the box, so it is the helper's
(`t95/helper`, Settings > Bluetooth). The keys it leaves in the kernel are what
the encryption here uses. BlueZ (D-Bus, `python3-dbus-fast`) is asked only which
devices are paired, and for the scan.

Seen on the rig with one device (an iPad, 2026-11-24): reconnect, encryption, MIDI
both ways. No Bluetooth MIDI keyboard or adapter has been tried.
"""
import ctypes
import errno
import socket
import struct
import tty

from .common import *
from .util import *
from .midi import MidiParser

MIDI_SERVICE = "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
MIDI_CHAR = "7772e5db-3868-4112-a1a9-f2669d106bf3"
MIDI_CHAR_LE = bytes(reversed(bytes.fromhex(MIDI_CHAR.replace("-", ""))))
BLE_POLL_S = 3.0
BLE_RETRY_S = 20.0         # between tries to reach a paired device that is away
BLE_FIND_S = 6.0           # how long one try scans for it (the radio is the Wi-Fi's too)
BLE_SETTLE_S = 3.0         # link up -> ask for encryption (see above; shorter is untried)
BLE_ENC_S = 10.0
BLE_MTU_MIN = 23
BLE_MTU = 247
ATT_CID = 4
SOL_BLUETOOTH, BT_SECURITY, BT_SEC_LOW, BT_SEC_MEDIUM = 274, 4, 1, 2
AF_BLUETOOTH = 31
ATT_REQUESTS = (0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x10, 0x16, 0x18)


def ble_decode(pkt, out=None):
    """One Bluetooth MIDI packet -> plain MIDI bytes. Every status byte in a packet
    follows a timestamp byte (both have bit 7 set); data bytes without one are
    running status, or more of a sysex from the packet before."""
    out = bytearray() if out is None else out
    if len(pkt) < 2 or not pkt[0] & 0x80:
        return out
    i = 1
    while i < len(pkt):
        b = pkt[i]
        i += 1
        if not b & 0x80:
            out.append(b)
        elif i < len(pkt) and pkt[i] & 0x80:    # a timestamp, then the status byte
            out.append(pkt[i])
            i += 1
        # else: a timestamp before running-status data (or a stray one at the end)
    return out


def ble_encode(msgs, mtu=BLE_MTU_MIN, ms=0):
    """MIDI messages -> packets of at most mtu - 3 bytes. A sysex that doesn't
    fit goes on in the next packet (header, then data)."""
    room = max(BLE_MTU_MIN, mtu) - 3
    hdr, ts = 0x80 | (ms >> 7 & 0x3F), 0x80 | (ms & 0x7F)
    out, cur = [], bytearray([hdr])
    for m in msgs:
        if not m:
            continue
        if m[0] != 0xF0:
            if len(cur) + 1 + len(m) > room and len(cur) > 1:
                out.append(bytes(cur))
                cur = bytearray([hdr])
            cur.append(ts)
            cur += m
            continue
        body = m[1:-1] if m[-1] == 0xF7 else m[1:]
        if len(cur) + 3 > room:                   # start a sysex in a packet with room for some of it
            out.append(bytes(cur))
            cur = bytearray([hdr])
        cur += bytes([ts, 0xF0])
        for b in body:
            if len(cur) >= room:
                out.append(bytes(cur))
                cur = bytearray([hdr])
            cur.append(b)
        if len(cur) + 2 > room:
            out.append(bytes(cur))
            cur = bytearray([hdr])
        cur += bytes([ts, 0xF7])
    if len(cur) > 1:
        out.append(bytes(cur))
    return out


def att_socket(addr, addr_type):
    """A non-blocking socket on the ATT channel, unencrypted, with the connection to addr asked for.
    Python's own L2CAP address has no channel number before 3.14, hence libc."""
    def sa(a):
        return struct.pack("<HH6sHB", AF_BLUETOOTH, 0, bytes(reversed(bytes.fromhex(a.replace(":", "")))), ATT_CID, addr_type) + b"\0"
    libc = ctypes.CDLL(None, use_errno=True)
    libc.bind.argtypes = libc.connect.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
    s = socket.socket(AF_BLUETOOTH, socket.SOCK_SEQPACKET, 0)
    try:
        if libc.bind(s.fileno(), sa(":".join(["00"] * 6)), 14):
            raise OSError(ctypes.get_errno(), "bind")
        s.setsockopt(SOL_BLUETOOTH, BT_SECURITY, struct.pack("BB", BT_SEC_LOW, 0))
        s.setblocking(False)
        if libc.connect(s.fileno(), sa(addr), 14) and ctypes.get_errno() != errno.EINPROGRESS:
            raise OSError(ctypes.get_errno(), os.strerror(ctypes.get_errno()))
    except OSError:
        s.close()
        raise
    return s


async def writable(sock, seconds):
    """True once the socket can be written (connected; or encrypted, after asking for that)"""
    loop = asyncio.get_running_loop()
    fut = loop.create_future()
    loop.add_writer(sock.fileno(), lambda: fut.done() or fut.set_result(True))
    try:
        return await asyncio.wait_for(fut, seconds)
    except asyncio.TimeoutError:
        return False
    finally:
        loop.remove_writer(sock.fileno())


class Att:
    """The few ATT exchanges Bluetooth MIDI needs, on a connected socket that keeps packets whole"""

    def __init__(self, sock, on_midi, on_gone):
        self.sock, self.on_midi, self.on_gone = sock, on_midi, on_gone
        self.mtu = BLE_MTU_MIN
        self.value = None           # handle of the MIDI characteristic's value
        self.waiting = None         # (request opcode, future)
        self.dropped = 0
        self.closed = False
        asyncio.get_running_loop().add_reader(sock.fileno(), self.readable)

    def readable(self):
        try:
            p = self.sock.recv(1024)
        except (BlockingIOError, InterruptedError):
            return
        except OSError:
            p = b""
        if not p:
            return self.close(True)
        op, w = p[0], self.waiting
        if w and not w[1].done() and (op == w[0] + 1 or (op == 0x01 and len(p) > 1 and p[1] == w[0])):
            w[1].set_result(p)
        elif op == 0x1B:
            if len(p) > 3 and struct.unpack_from("<H", p, 1)[0] == self.value:
                self.on_midi(p[3:])
        # The other side asks things too. The bridge has no services: say so, so it doesn't wait.
        elif op == 0x02:
            self.send(struct.pack("<BH", 0x03, BLE_MTU))
        elif op == 0x12:
            self.send(b"\x13")
        elif op == 0x1D:
            self.send(b"\x1e")
        elif op in ATT_REQUESTS:
            self.send(struct.pack("<BBHB", 0x01, op, struct.unpack_from("<H", p, 1)[0] if len(p) >= 3 else 0, 0x0A))

    def send(self, pdu):
        try:
            self.sock.send(pdu)
            return True
        except (BlockingIOError, InterruptedError):
            self.dropped += 1       # the radio isn't taking it: lose it rather than wait
        except OSError:
            self.close(True)
        return False

    async def request(self, pdu, seconds=5.0):
        fut = asyncio.get_running_loop().create_future()
        self.waiting = (pdu[0], fut)
        try:
            if not self.send(pdu):
                raise OSError("the link is gone")
            return await asyncio.wait_for(fut, seconds)
        except asyncio.TimeoutError:
            raise OSError("no answer to ATT request 0x%02x" % pdu[0])
        finally:
            self.waiting = None

    async def open_midi(self):
        """Packet size, the MIDI characteristic, notifications on. Raises OSError if it isn't there."""
        p = await self.request(struct.pack("<BH", 0x02, BLE_MTU))
        if p[0] == 0x03 and len(p) >= 3:
            self.mtu = max(BLE_MTU_MIN, min(BLE_MTU, struct.unpack_from("<H", p, 1)[0]))
        start, decls = 1, []        # characteristic declarations: (handle, value handle, uuid)
        while start <= 0xFFFF:
            p = await self.request(struct.pack("<BHHH", 0x08, start, 0xFFFF, 0x2803))
            if p[0] != 0x09 or len(p) < 2 or p[1] < 7:
                break
            n = p[1]
            for i in range(2, len(p) - n + 1, n):
                h, _, vh = struct.unpack_from("<HBH", p, i)
                decls.append((h, vh, p[i + 5:i + n]))
                start = h + 1
        decls.sort()
        at = next((i for i, d in enumerate(decls) if d[2] == MIDI_CHAR_LE), None)
        if at is None:
            raise OSError("no MIDI characteristic on it")
        self.value = decls[at][1]
        last = decls[at + 1][0] - 1 if at + 1 < len(decls) else min(0xFFFF, self.value + 8)
        ccc, start = None, self.value + 1
        while ccc is None and start <= last:
            p = await self.request(struct.pack("<BHH", 0x04, start, last))
            if p[0] != 0x05 or len(p) < 6 or p[1] != 1:
                break
            for i in range(2, len(p) - 3, 4):
                h, u = struct.unpack_from("<HH", p, i)
                start = h + 1
                if u == 0x2902:
                    ccc = h
                    break
        if ccc is None:
            raise OSError("its MIDI characteristic has no notifications")
        p = await self.request(struct.pack("<BHH", 0x12, ccc, 1))
        if p[0] != 0x13:
            raise OSError("it refused notifications (ATT error 0x%02x)" % (p[4] if len(p) > 4 else 0))

    def write_midi(self, pkt):
        return self.send(struct.pack("<BH", 0x52, self.value) + pkt)      # write without response

    def close(self, tell=False):
        if self.closed:
            return
        self.closed = True
        try:
            asyncio.get_running_loop().remove_reader(self.sock.fileno())
        except (OSError, ValueError):
            pass
        self.sock.close()
        if self.waiting and not self.waiting[1].done():
            self.waiting[1].set_exception(OSError("the link is gone"))
        if tell:
            self.on_gone()


class BleLink:
    """One device: its ATT channel, and a pty the bridge opens as a MIDI port"""

    def __init__(self, key, name, on_gone):
        self.key, self.name, self.on_gone = key, name, on_gone
        self.att = None
        self.master, self.slave = os.openpty()      # the slave stays open here: the master never reads "nobody there"
        tty.setraw(self.slave)                      # before anything is written: no echo back to the radio
        self.path = os.ttyname(self.slave)
        os.set_blocking(self.master, False)
        self.parser = MidiParser()
        self.dropped_in = self.dropped_out = 0
        self.closed = False
        asyncio.get_running_loop().add_reader(self.master, self.from_bridge)

    def attach(self, sock):
        self.att = Att(sock, self.from_radio, lambda: self.close(True))
        return self.att

    def from_radio(self, pkt):
        data = ble_decode(pkt)
        try:
            if data and os.write(self.master, data) < len(data):
                self.dropped_in += 1
        except BlockingIOError:                     # the bridge isn't reading: lose it rather than wait
            self.dropped_in += 1
        except OSError:
            self.close(True)

    def from_bridge(self):
        try:
            data = os.read(self.master, 4096)
        except BlockingIOError:
            return
        except OSError as e:
            if e.errno == errno.EIO:                # nobody has the port open (yet)
                return
            return self.close(True)
        if self.att is None or self.att.value is None:
            return
        for pkt in ble_encode(self.parser.feed(data), self.att.mtu, int(now() * 1000) & 0x1FFF):
            if self.closed:
                return
            if not self.att.write_midi(pkt):
                self.dropped_out += 1

    def close(self, tell=False):
        if self.closed:
            return
        self.closed = True
        asyncio.get_running_loop().remove_reader(self.master)
        if self.att is not None:
            self.att.close()
        for fd in (self.master, self.slave):
            try:
                os.close(fd)
            except OSError:
                pass
        if tell:
            self.on_gone(self)


class Ble:
    """Asks BlueZ which paired devices offer MIDI and keeps a BleLink per one in reach"""

    def __init__(self, bridge):
        self.b = bridge
        self.links = {}             # device path -> BleLink
        self.tried = {}             # device path -> when we last tried to reach it
        self.why = {}               # device path -> why the last try failed (said once in the log)
        self.state = "off"          # off / no dbus-fast / no BlueZ / running
        self.seen = 0               # paired MIDI devices BlueZ knows
        self.bus = None

    def info(self):
        return {"state": self.state, "paired": self.seen,
                "connected": [{"name": l.name, "dropped_in": l.dropped_in, "dropped_out": l.dropped_out + l.att.dropped}
                              for l in self.links.values()],
                "away": sorted(set(self.why.values()))}

    def candidates(self):
        return {l.key: (l.path, l.name) for l in self.links.values() if not l.closed}

    def gone(self, link):
        for path, l in list(self.links.items()):
            if l is link:
                del self.links[path]
                self.tried[path] = now()            # not at once: it may have been switched off
        log.info("Bluetooth MIDI %s gone", link.name)

    async def call(self, path, iface, member, signature="", body=None):
        from dbus_fast import Message, MessageType
        r = await asyncio.wait_for(self.bus.call(Message(destination="org.bluez", path=path, interface=iface, member=member,
                                                         signature=signature, body=body or [])), 20)
        if r.message_type != MessageType.METHOD_RETURN:
            raise OSError(r.error_name or "D-Bus error")
        return r

    async def reach(self, adapter, addr, addr_type, link):
        """Connect, let the link settle, encrypt, open MIDI. Raises OSError with the reason."""
        from dbus_fast import Variant
        sock = att_socket(addr, addr_type)          # first: the device has to be wanted before the scan reports it
        try:
            scanning = False
            try:
                await self.call(adapter, "org.bluez.Adapter1", "SetDiscoveryFilter", "a{sv}", [{"Transport": Variant("s", "le")}])
                await self.call(adapter, "org.bluez.Adapter1", "StartDiscovery")
                scanning = True
            except (OSError, asyncio.TimeoutError):
                pass                                # somebody else may be scanning: try anyway
            try:
                up = await writable(sock, BLE_FIND_S)
            finally:
                if scanning:                        # before anything else: the link doesn't settle under a scan
                    try:
                        await self.call(adapter, "org.bluez.Adapter1", "StopDiscovery")
                    except (OSError, asyncio.TimeoutError):
                        pass
            err = sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
            if not up or err:
                raise OSError("not in reach" if not up else os.strerror(err))
            await asyncio.sleep(BLE_SETTLE_S)
            sock.setsockopt(SOL_BLUETOOTH, BT_SECURITY, struct.pack("BB", BT_SEC_MEDIUM, 0))
            ok = await writable(sock, BLE_ENC_S)
            err = sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
            if not ok or err or sock.getsockopt(SOL_BLUETOOTH, BT_SECURITY, 2)[0] < BT_SEC_MEDIUM:
                raise OSError("no encryption (pair it again in Settings if this stays)")
        except BaseException:
            sock.close()
            raise
        await link.attach(sock).open_midi()

    async def scan(self):
        r = await self.call("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects")
        objs = r.body[0]

        def val(props, k, d=None):
            v = props.get(k)
            return getattr(v, "value", v) if v is not None else d
        devs = {}
        for path, ifs in objs.items():
            d = ifs.get("org.bluez.Device1")
            if d is not None and val(d, "Paired", False) and MIDI_SERVICE in [u.lower() for u in val(d, "UUIDs", [])]:
                devs[path] = d
        self.seen = len(devs)
        for path in [p for p in self.links if p not in devs]:      # forgotten in Settings
            self.links.pop(path).close()
        for gone in [p for p in self.why if p not in devs]:
            del self.why[gone]
        for path, d in devs.items():
            if path in self.links or now() - self.tried.get(path, 0) < BLE_RETRY_S:
                continue
            self.tried[path] = now()
            name = str(val(d, "Alias", "") or val(d, "Name", "") or "Bluetooth MIDI")
            addr, typ = str(val(d, "Address", "")), 2 if val(d, "AddressType", "public") == "random" else 1
            link = BleLink("ble:" + path.rsplit("/", 1)[-1], name, self.gone)
            try:
                await self.reach(str(val(d, "Adapter", "/org/bluez/hci0")), addr, typ, link)
            except (OSError, asyncio.TimeoutError) as e:
                link.close()
                why = "%s: %s" % (name, e)
                if self.why.get(path) != why:
                    self.why[path] = why
                    log.info("Bluetooth MIDI %s", why)
                continue
            self.why.pop(path, None)
            if link.closed:
                continue
            self.links[path] = link
            log.info("Bluetooth MIDI %s connected (packets up to %d bytes)", name, link.att.mtu)

    async def task(self):
        if not getattr(self.b.args, "ble", False):
            return
        try:
            from dbus_fast import BusType
            from dbus_fast.aio import MessageBus
        except ImportError:
            self.state = "no dbus-fast"
            log.info("no Bluetooth MIDI: python3-dbus-fast isn't installed")
            return
        while True:
            try:
                self.bus = await MessageBus(bus_type=BusType.SYSTEM, negotiate_unix_fd=True).connect()
                self.state = "running"
                while True:
                    await self.scan()
                    await asyncio.sleep(BLE_POLL_S)
            except asyncio.CancelledError:
                raise
            except Exception as e:                  # BlueZ off, restarted, the bus gone: the bridge goes on without
                if self.state != "no BlueZ":
                    log.info("Bluetooth MIDI: BlueZ isn't answering (%s)", type(e).__name__)
                self.state = "no BlueZ"
                for l in list(self.links.values()):
                    l.close()
                self.links.clear()
                try:
                    if self.bus is not None:
                        self.bus.disconnect()
                except Exception:
                    pass
                self.bus = None
            await asyncio.sleep(BLE_RETRY_S)
