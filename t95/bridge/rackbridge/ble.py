"""Bluetooth LE MIDI: paired devices become controller ports.

This kernel has no ALSA sequencer, so BlueZ's own MIDI plug-in can't make a port.
Instead the bridge asks BlueZ (D-Bus, `python3-dbus-fast`) for the MIDI
characteristic of every paired device that has one, and puts a pseudo-terminal in
front of it: plain MIDI bytes on the bridge's side, Bluetooth MIDI packets on the
radio's. The rest of the bridge sees one more raw MIDI device (`midi_candidates`).

Pairing is not done here: that changes the box, so it is the helper's
(`t95/helper`, Settings > Bluetooth). Connecting to a device that is already
paired is.

Not tried against a real Bluetooth MIDI device yet (2026-11-24): the packet code
and the link are covered by tests, the BlueZ side only as far as "runs on the T95
and finds nothing".
"""
import errno
import tty

from .common import *
from .util import *
from .midi import MidiParser

MIDI_SERVICE = "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
MIDI_CHAR = "7772e5db-3868-4112-a1a9-f2669d106bf3"
BLE_POLL_S = 3.0
BLE_RETRY_S = 10.0         # between tries to connect to a paired device that is away
BLE_MTU_MIN = 23


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


class BleLink:
    """One device: packets in on rfd, out on wfd; a pty the bridge opens as a MIDI port"""

    def __init__(self, key, name, rfd, wfd, mtu, on_gone):
        self.key, self.name, self.rfd, self.wfd, self.mtu, self.on_gone = key, name, rfd, wfd, mtu, on_gone
        self.master, self.slave = os.openpty()      # the slave stays open here: the master never reads "nobody there"
        tty.setraw(self.slave)                      # before anything is written: no echo back to the radio
        self.path = os.ttyname(self.slave)
        for fd in (self.master, rfd, wfd):
            os.set_blocking(fd, False)
        self.parser = MidiParser()
        self.dropped_in = self.dropped_out = 0
        self.closed = False
        loop = asyncio.get_running_loop()
        loop.add_reader(rfd, self.from_radio)
        loop.add_reader(self.master, self.from_bridge)

    def from_radio(self):
        try:
            pkt = os.read(self.rfd, 1024)
        except BlockingIOError:
            return
        except OSError:
            pkt = b""
        if not pkt:
            return self.close(True)
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
        for pkt in ble_encode(self.parser.feed(data), self.mtu, int(now() * 1000) & 0x1FFF):
            try:
                os.write(self.wfd, pkt)
            except BlockingIOError:
                self.dropped_out += 1
            except OSError:
                return self.close(True)

    def close(self, tell=False):
        if self.closed:
            return
        self.closed = True
        loop = asyncio.get_running_loop()
        for fd in (self.rfd, self.master):
            loop.remove_reader(fd)
        for fd in {self.rfd, self.wfd, self.master, self.slave}:
            try:
                os.close(fd)
            except OSError:
                pass
        if tell:
            self.on_gone(self)


class Ble:
    """Watches BlueZ for paired MIDI devices and keeps a BleLink per connected one"""

    def __init__(self, bridge):
        self.b = bridge
        self.links = {}             # device path -> BleLink
        self.tried = {}             # device path -> when we last asked it to connect
        self.state = "off"          # off / no dbus-fast / no BlueZ / running
        self.seen = 0               # paired MIDI devices BlueZ knows
        self.bus = None

    def info(self):
        return {"state": self.state, "paired": self.seen,
                "connected": [{"name": l.name, "dropped_in": l.dropped_in, "dropped_out": l.dropped_out} for l in self.links.values()]}

    def candidates(self):
        return {l.key: (l.path, l.name) for l in self.links.values() if not l.closed}

    def gone(self, link):
        for path, l in list(self.links.items()):
            if l is link:
                del self.links[path]
        log.info("Bluetooth MIDI %s gone", link.name)

    async def call(self, path, iface, member, signature="", body=None):
        from dbus_fast import Message, MessageType
        r = await asyncio.wait_for(self.bus.call(Message(destination="org.bluez", path=path, interface=iface, member=member,
                                                         signature=signature, body=body or [])), 20)
        if r.message_type != MessageType.METHOD_RETURN:
            raise OSError(r.error_name or "D-Bus error")
        return r

    async def acquire(self, path, member):
        """(fd, mtu) from AcquireNotify / AcquireWrite"""
        r = await self.call(path, "org.bluez.GattCharacteristic1", member, "a{sv}", [{}])
        fd, mtu = r.body[0], r.body[1]
        if r.unix_fds and isinstance(fd, int) and fd < len(r.unix_fds):
            fd = r.unix_fds[fd]                     # the body holds an index into the fds that came along
        return fd, int(mtu)

    async def scan(self):
        r = await self.call("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects")
        objs = r.body[0]

        def val(props, k, d=None):
            v = props.get(k)
            return getattr(v, "value", v) if v is not None else d
        devs, chars = {}, {}
        for path, ifs in objs.items():
            d = ifs.get("org.bluez.Device1")
            if d is not None and val(d, "Paired", False) and MIDI_SERVICE in [u.lower() for u in val(d, "UUIDs", [])]:
                devs[path] = d
            c = ifs.get("org.bluez.GattCharacteristic1")
            if c is not None and str(val(c, "UUID", "")).lower() == MIDI_CHAR:
                chars[path] = c
        self.seen = len(devs)
        for path in [p for p in self.links if p not in devs]:      # forgotten in Settings
            self.links.pop(path).close()
        for path, d in devs.items():
            if path in self.links:
                continue
            name = str(val(d, "Alias", "") or val(d, "Name", "") or "Bluetooth MIDI")
            if not val(d, "Connected", False):
                if now() - self.tried.get(path, 0) > BLE_RETRY_S:
                    self.tried[path] = now()
                    try:
                        await self.call(path, "org.bluez.Device1", "Connect")
                    except (OSError, asyncio.TimeoutError):
                        pass                        # away or asleep: again later
                continue
            if not val(d, "ServicesResolved", False):
                continue
            ch = next((p for p in chars if p.startswith(path + "/")), None)
            if ch is None:
                continue
            try:
                rfd, mtu = await self.acquire(ch, "AcquireNotify")
                wfd, _ = await self.acquire(ch, "AcquireWrite")
            except (OSError, asyncio.TimeoutError) as e:
                log.warning("Bluetooth MIDI %s: %s", name, e)
                continue
            self.links[path] = BleLink("ble:" + path.rsplit("/", 1)[-1], name, rfd, wfd, mtu, self.gone)
            log.info("Bluetooth MIDI %s connected (packets up to %d bytes)", name, mtu)

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
