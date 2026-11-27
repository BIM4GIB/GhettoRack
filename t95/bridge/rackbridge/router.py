"""The router: any MIDI source to any destination, by device name.

Sources and destinations are the ports (USB and network MIDI sessions) plus
"daisy". A route: {"from": name or "*", "to": name or "daisy", "ch": 0 (all)
or 1-16 (then channel messages of that channel only), "remap": 0 or 1-16, "types": subset of ROUTE_TYPES (missing: all)}.
The active module's default routes plus its own (set on its page, saved
per module, with named presets) are applied. Only ports routed to the Daisy
are announced to it, so in the MIDI hub an APC40 is just a MIDI device.
Clock bytes are never sent to the Daisy (its tempo is its own). A port that
goes away with notes held sends their note offs first.
"""
from .common import *
from .util import *


def msg_type(msg):
    s = msg[0]
    if s == 0xF0:
        return "sysex"
    if s >= 0xF8 or s == 0xF2:
        return "clock"            # clock, start/stop/continue, song position
    return {0x80: "note", 0x90: "note", 0xA0: "at", 0xB0: "cc", 0xC0: "pc", 0xD0: "at", 0xE0: "pb"}.get(s & 0xF0)


def valid_route(r):
    """A clean copy of a route, or None"""
    if not isinstance(r, dict):
        return None
    f, t = r.get("from"), r.get("to")
    if not (isinstance(f, str) and 0 < len(f) <= 40 and isinstance(t, str) and 0 < len(t) <= 40) or t == "*":
        return None
    out = {"from": f, "to": t}
    for k in ("ch", "remap"):
        v = r.get(k, 0)
        if not isinstance(v, int) or not 0 <= v <= 16:
            return None
        if v:
            out[k] = v
    if "types" in r:
        ty = r["types"]
        if not isinstance(ty, list) or not all(x in ROUTE_TYPES for x in ty):
            return None
        out["types"] = sorted(set(ty))
    return out


class RouterMixin:
    """Bridge methods for routing"""

    def module(self):
        """The active module (the last one chosen; ApcBox before any choice)"""
        mid = self.state.data.get("last") or "apcbox"
        return next((m for m in MODULES if m["id"] == mid), MODULES[0])

    def active_routes(self):
        m = self.module()
        return list(m["routes"]) + list(self.state.data["routes"].get(m["id"], []))

    def port_by_name(self, name):
        return next((p for p in self.ports.values() if p.name == name), None)

    def route(self, p, msg):
        """One complete message from port p through the active routes"""
        ty = msg_type(msg)
        if ty == "clock" and self.clock.source == p.name:
            self.clock.feed(msg)      # the clock module follows this port
        if ty == "note":
            k = (msg[0] & 15, msg[1])
            if msg[0] & 0xF0 == 0x90 and msg[2]:
                p.held.add(k)
            else:
                p.held.discard(k)
        sent = False
        for r in self.active_routes():
            if r["from"] not in ("*", p.name) or r["to"] == p.name:
                continue
            if "types" in r and ty not in r["types"]:
                continue
            out = msg
            if r.get("ch") and (msg[0] >= 0xF0 or (msg[0] & 15) + 1 != r["ch"]):
                continue           # a channel route carries that channel only (no clock, no sysex)
            if msg[0] < 0xF0:
                if r.get("remap"):
                    out = bytes([(msg[0] & 0xF0) | (r["remap"] - 1)]) + msg[1:]
            if r["to"] == "daisy":
                if ty == "clock" or p.num not in self.daisy_ports or len(out) > SYSEX_MAX:
                    continue
                if not self.ctl_send(b"%sM%d %s\n" % (ESC, p.num, out.hex().encode())):
                    p.dropped_in += 1
                sent = True
            else:
                q = self.port_by_name(r["to"])
                if q is not None:
                    self.port_send(q, out)
                    sent = True
        if not sent:
            p.unrouted += 1

    def release_held(self, p):
        """A port going away with notes down: note offs to wherever they went"""
        for ch, note in sorted(p.held):
            self.route(p, bytes([0x80 | ch, note, 0]))
        p.held.clear()

    def port_send(self, q, msg):
        if len(q.out_fifo) >= OUT_FIFO_MAX:
            q.out_fifo.popleft()
            q.dropped_out += 1
        q.out_fifo.append(msg)
        self.midi_flush(q)

    def refresh_daisy_ports(self):
        """Announce to the Daisy exactly the ports the active module routes to it"""
        want = set()
        for p in self.ports.values():
            if any(r["to"] == "daisy" and r["from"] in ("*", p.name) for r in self.active_routes()):
                want.add(p.num)
        for n in sorted(want - self.daisy_ports):
            self.announce_port(self.ports[n])
        for n in sorted(self.daisy_ports - want):
            self.ctl_send(b"%sN%d -\n" % (ESC, n))
        self.daisy_ports = want

    def routes_json(self):
        m = self.module()
        dests = [p.name for p in sorted(self.ports.values(), key=lambda p: p.num)]
        return {"module": m["id"], "defaults": m["routes"], "routes": self.state.data["routes"].get(m["id"], []),
                "sources": dests, "destinations": dests + (["daisy"] if self.fd is not None else []),
                "presets": sorted(self.state.data["presets"].get(m["id"], {})), "types": list(ROUTE_TYPES),
                "net": self.net_info()}

    def set_routes(self, routes):
        if not isinstance(routes, list) or len(routes) > ROUTES_MAX:
            return 400, {"error": "a list of at most %d routes" % ROUTES_MAX}
        clean = [valid_route(r) for r in routes]
        if None in clean:
            return 400, {"error": "bad route: %s" % json.dumps(routes[clean.index(None)])[:120]}
        self.state.data["routes"][self.module()["id"]] = clean
        self.state.save()
        self.refresh_daisy_ports()
        return 200, {"ok": True}

    def preset(self, op, name, to=None):
        if not isinstance(name, str) or not 0 < len(name) <= 32:
            return 400, {"error": "name: 1-32 characters"}
        mid = self.module()["id"]
        ps = self.state.data["presets"].setdefault(mid, {})
        if op == "save":
            if len(ps) >= 32 and name not in ps:
                return 400, {"error": "32 presets at most"}
            ps[name] = list(self.state.data["routes"].get(mid, []))
        elif op == "load":
            if name not in ps:
                return 400, {"error": "no preset %s" % name}
            self.state.data["routes"][mid] = list(ps[name])
            self.refresh_daisy_ports()
        elif op == "delete":
            ps.pop(name, None)
        elif op == "rename":
            if name not in ps:
                return 400, {"error": "no preset %s" % name}
            if not isinstance(to, str) or not 0 < len(to) <= 32:
                return 400, {"error": "name: 1-32 characters"}
            if to != name and to in ps:
                return 400, {"error": "there is a preset called %s already" % to}
            # its place in the order kept: the list is by name anyway
            self.state.data["presets"][mid] = {(to if k == name else k): v for k, v in ps.items()}
        self.state.save()
        return 200, {"ok": True}
