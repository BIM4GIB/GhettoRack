#!/usr/bin/env python3
"""GhettoRack settings helper: the few things on the T95 that need root.

The bridge (the web server) runs without rights on purpose. It asks this
helper over a local socket, and this helper does only what is on its list:

    status              Wi-Fi, address, name, Bluetooth, trial in progress
    scan                Wi-Fi networks in range
    join ssid psk       try a network; it is saved only if the box gets
    confirm               onto it and "confirm" arrives within the trial
                          time, else the box goes back to the network it
                          had (no lockout). A wrong password is back within
                          seconds
    forget ssid         remove a network this helper added
    hostname name       the box's name on the network
    bt on|off           Bluetooth on the UWE5622 (needs the attach tool): the
                          chip attaches once per boot and is never detached;
                          on / off is the radio's block switch
    restart_bridge, reboot, poweroff
    pin_set old new     the PIN for everything that changes something
    pin_check pin

One JSON object per line in, one per line out: {"cmd": ..., "pin": ...}.
Everything that changes the box needs the PIN; five wrong ones lock for a
minute. Passwords are written only to the box's own network config (mode
600) and never logged.

Wi-Fi: this box runs Netplan + systemd-networkd + wpa_supplicant. A network
is tried in the running wpa_supplicant only (over its control socket, so the
password is never on a command line): the saved config isn't touched and
nothing restarts, so a failed try costs seconds. Once confirmed it goes into
one extra Netplan file (JSON, which is YAML) next to the ones from the
install; Netplan merges them at the next start. "netplan apply" is never run
here: on the rig it took the Wi-Fi down for most of a minute, twice per
failed try (2026-11-23). Nothing else is rewritten.

Stdlib only. Paths and the trial time can be changed for tests.
"""
import argparse
import grp
import hashlib
import hmac
import json
import logging
import os
import re
import secrets
import socket
import socketserver
import subprocess
import threading
import time

log = logging.getLogger("helper")
RISKY = {"join", "forget", "hostname", "bt", "restart_bridge", "reboot", "poweroff"}
HOST_RE = re.compile(r"^[a-z0-9]([a-z0-9-]{0,30}[a-z0-9])?$")
PIN_RE = re.compile(r"^[0-9]{4,8}$")
ATTACH = "/usr/local/bin/hciattach_opi"
BT_UNIT = "ghettorack-bluetooth.service"


class Helper:
    def __init__(self, a):
        self.a = a
        self.file = os.path.join(a.netplan_dir, "40-ghettorack-wifi.yaml")
        self.pin_file = os.path.join(a.state_dir, "pin")
        self.lock = threading.Lock()
        self.trial = None           # {id, ssid, psk, phase: connecting|connected, deadline}
        self.last = None            # how the last try ended: {ssid, ok, why}
        self.ctl_n = 0
        self.fails, self.locked_until = 0, 0.0

    # ---- running things ----
    def run(self, *cmd, timeout=20):
        """(return code, output). Never raises; never logs arguments (they can hold passwords)."""
        try:
            r = subprocess.run(list(cmd), capture_output=True, timeout=timeout)
            return r.returncode, (r.stdout + r.stderr).decode(errors="replace")
        except (OSError, subprocess.TimeoutExpired) as e:
            return 127, type(e).__name__

    def wpa(self, *args):
        return self.run(self.a.wpa_cli, "-i", self.a.iface, *args)

    def ctl(self, cmd, timeout=5):
        """One command straight to wpa_supplicant's control socket; the reply, or "" if
        it doesn't answer. Passwords go this way: never on a command line."""
        self.ctl_n += 1
        mine = os.path.join(os.path.dirname(self.a.socket), "wpa-%d-%d" % (os.getpid(), self.ctl_n))
        s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            s.bind(mine)
            s.settimeout(timeout)
            s.connect(os.path.join(self.a.wpa_ctrl, self.a.iface))
            s.send(cmd.encode())
            while True:
                r = s.recv(16384).decode(errors="replace")
                if not r.startswith("<"):                    # "<3>CTRL-EVENT..." is an event, not our reply
                    return r.strip()
        except OSError:
            return ""
        finally:
            s.close()
            try:
                os.unlink(mine)
            except OSError:
                pass

    # ---- the PIN ----
    def pin_set_p(self):
        return os.path.exists(self.pin_file)

    @staticmethod
    def pin_hash(pin, salt):
        return hashlib.pbkdf2_hmac("sha256", pin.encode(), salt, 200_000).hex()

    def pin_ok(self, pin):
        """None = fine, else why not"""
        if not self.pin_set_p():
            return "set a PIN first"
        if time.monotonic() < self.locked_until:
            return "too many wrong PINs: wait a minute"
        try:
            salt, want = open(self.pin_file).read().strip().split(":")
        except (OSError, ValueError):
            return "the PIN file is damaged: delete %s on the box" % self.pin_file
        if isinstance(pin, str) and hmac.compare_digest(self.pin_hash(pin, bytes.fromhex(salt)), want):
            self.fails = 0
            return None
        self.fails += 1
        if self.fails >= 5:
            self.fails, self.locked_until = 0, time.monotonic() + self.a.lock_s
        return "wrong PIN"

    def cmd_pin_set(self, d):
        new = d.get("new")
        if not isinstance(new, str) or not PIN_RE.match(new):
            return {"error": "the PIN is 4 to 8 digits"}
        if self.pin_set_p():
            why = self.pin_ok(d.get("old"))
            if why:
                return {"error": why if why != "wrong PIN" else "the old PIN is wrong"}
        salt = secrets.token_bytes(16)
        tmp = self.pin_file + ".tmp"
        with open(os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
            f.write(salt.hex() + ":" + self.pin_hash(new, salt) + "\n")
        os.replace(tmp, self.pin_file)
        log.info("PIN set")
        return {"ok": True}

    def cmd_pin_check(self, d):
        why = self.pin_ok(d.get("pin"))
        return {"error": why} if why else {"ok": True}

    # ---- Wi-Fi ----
    def saved(self):
        """{ssid: {...}} of the networks this helper added"""
        try:
            return json.load(open(self.file))["network"]["wifis"][self.a.iface]["access-points"]
        except (OSError, ValueError, KeyError, TypeError):
            return {}

    def write_saved(self, aps):
        if not aps:
            try:
                os.unlink(self.file)
            except OSError:
                pass
            return
        doc = {"network": {"version": 2, "wifis": {self.a.iface: {"access-points": aps}}}}
        tmp = self.file + ".tmp"
        with open(os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
            json.dump(doc, f, indent=1)
            f.write("\n")
        os.replace(tmp, self.file)

    def raw_file(self):
        try:
            return open(self.file).read()
        except OSError:
            return None

    def restore(self, content):
        if content is None:
            try:
                os.unlink(self.file)
            except OSError:
                pass
        else:
            with open(os.open(self.file, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
                f.write(content)

    def generate(self):
        """None = Netplan takes the files as they are, else what it said. Changes nothing that runs."""
        rc, out = self.run(self.a.netplan, "generate")
        return None if rc == 0 else "netplan refused it: " + out.strip()[-200:]

    def nets(self):
        """[(id, ssid, flags)] of the running wpa_supplicant"""
        out = []
        for l in self.ctl("LIST_NETWORKS").splitlines()[1:]:
            f = l.split("\t")
            if len(f) >= 2 and f[0].isdigit():
                out.append((f[0], f[1], f[3] if len(f) > 3 else ""))
        return out

    def wifi_now(self):
        st = dict(l.split("=", 1) for l in self.wpa("status")[1].splitlines() if "=" in l)
        return st.get("ssid", ""), st.get("wpa_state", ""), st.get("ip_address", "")

    def cmd_scan(self, d):
        rc, out = self.wpa("scan")
        if rc:
            return {"error": "the Wi-Fi isn't answering (wpa_cli: %s)" % (out.strip().splitlines() or ["no output"])[-1][:120]}
        for _ in range(4):                                   # a scan of both bands can take several seconds
            time.sleep(self.a.scan_s)
            rc, out = self.wpa("scan_results")
            if rc:
                return {"error": "the Wi-Fi isn't answering (wpa_cli: %s)" % (out.strip().splitlines() or ["no output"])[-1][:120]}
            if len(out.splitlines()) > 1:
                break
        cur, saved, best = self.wifi_now()[0], self.saved(), {}
        for l in out.splitlines()[1:]:
            f = l.split("\t")
            if len(f) < 5 or not f[4].strip() or "\\x00" in f[4]:
                continue                                    # hidden networks
            try:
                pct = max(0, min(100, 2 * (int(f[2]) + 100)))
            except ValueError:
                continue
            ssid = f[4]
            if ssid not in best or pct > best[ssid]["signal"]:
                best[ssid] = {"ssid": ssid, "signal": pct, "secure": "WPA" in f[3] or "WEP" in f[3] or "SAE" in f[3],
                              "saved": ssid in saved, "current": ssid == cur}
        return {"ok": True, "networks": sorted(best.values(), key=lambda n: (-n["current"], -n["signal"]))[:24]}

    def cmd_join(self, d):
        ssid, psk = d.get("ssid"), d.get("psk", "")
        if not isinstance(ssid, str) or not 0 < len(ssid.encode()) <= 32 or any(ord(c) < 32 for c in ssid):
            return {"error": "that network name can't be used"}
        if not isinstance(psk, str) or (psk and not 8 <= len(psk) <= 63) or any(ord(c) < 32 or ord(c) > 126 for c in psk):
            return {"error": "a Wi-Fi password is 8 to 63 characters (none for an open network)"}
        with self.lock:
            if self.trial:
                return {"error": "another network is being tried: wait for it"}
            nid = self.ctl("ADD_NETWORK")
            if not nid.isdigit():
                return {"error": "the Wi-Fi isn't answering"}
            # name and key as hex: no quoting to get wrong. The key is the network's own derived one.
            sets = [("ssid", ssid.encode().hex())]
            sets.append(("psk", hashlib.pbkdf2_hmac("sha1", psk.encode(), ssid.encode(), 4096, 32).hex()) if psk
                        else ("key_mgmt", "NONE"))
            for k, v in sets:
                if self.ctl("SET_NETWORK %s %s %s" % (nid, k, v)) != "OK":
                    self.ctl("REMOVE_NETWORK " + nid)
                    return {"error": "the Wi-Fi didn't take that network"}
            self.last = None
            self.trial = {"id": nid, "ssid": ssid, "psk": psk, "phase": "connecting", "selected": False,
                          "deadline": time.monotonic() + self.a.select_s + self.a.connect_s}
        threading.Thread(target=self.trial_watch, daemon=True).start()
        log.info("trying a Wi-Fi network")                              # no name, no password
        return {"ok": True, "trial_s": self.a.trial_s, "connect_s": self.a.connect_s}

    def back_locked(self, nid):
        """drop the tried network and let the ones from before connect again"""
        self.ctl("REMOVE_NETWORK " + nid)
        self.ctl("ENABLE_NETWORK all")
        self.ctl("REASSOCIATE")

    def fail_locked(self, why):
        t, self.trial = self.trial, None
        self.back_locked(t["id"])
        self.last = {"ssid": t["ssid"], "ok": False, "why": why}
        log.warning("the new Wi-Fi wasn't kept (%s): back to the one before", why)

    def trial_watch(self):
        # The box has one radio: trying a network means leaving the current one. Wait a moment
        # first, so the answer to "join" still reaches a page that came in over that Wi-Fi
        # (on the rig the answer was lost and the page said "the box isn't answering", 2026-11-23).
        time.sleep(self.a.select_s)
        with self.lock:
            t = self.trial
            if not t:
                return
            if self.ctl("SELECT_NETWORK " + t["id"]) != "OK":            # the others wait until it is settled
                self.fail_locked("the Wi-Fi didn't take that network")
                return
            t["selected"] = True
        while True:
            time.sleep(self.a.watch_s)
            with self.lock:
                t = self.trial
                if not t:
                    return
                late = time.monotonic() >= t["deadline"]
                if t["phase"] == "connected":
                    if late:
                        self.fail_locked("nobody confirmed it: this device has to reach the box on the new network")
                        return
                    continue
                st = dict(l.split("=", 1) for l in self.ctl("STATUS").splitlines() if "=" in l)
                if st.get("wpa_state") == "COMPLETED" and st.get("id") == t["id"]:
                    t["phase"], t["deadline"] = "connected", time.monotonic() + self.a.trial_s
                elif any(i == t["id"] and "TEMP-DISABLED" in fl for i, _, fl in self.nets()):
                    self.fail_locked("wrong password")
                    return
                elif late:
                    self.fail_locked("no connection: wrong password, or the network is too weak here")
                    return

    def cmd_confirm(self, d):
        with self.lock:
            t = self.trial
            if not t or t["phase"] != "connected":
                return {"ok": True, "kept": False}
            before = self.raw_file()
            aps = self.saved()
            aps[t["ssid"]] = {"password": t["psk"]} if t["psk"] else {}
            self.write_saved(aps)
            why = self.generate()
            if why:
                self.restore(before)
                self.fail_locked("it connected but couldn't be saved")
                return {"error": why}
            self.ctl("ENABLE_NETWORK all")                               # the older ones stay as a way back
            self.trial, self.last = None, {"ssid": t["ssid"], "ok": True, "why": ""}
        log.info("new Wi-Fi confirmed: kept")
        return {"ok": True, "kept": True}

    def cmd_forget(self, d):
        ssid = d.get("ssid")
        with self.lock:
            if self.trial:
                return {"error": "a network is being tried: wait for it"}
            aps = self.saved()
            if ssid not in aps:
                return {"error": "only networks added on this page can be removed here (that one came with the install, or isn't saved)"}
            cur, state, _ = self.wifi_now()
            if cur == ssid and state == "COMPLETED":
                return {"error": "the box is on that network right now: join another one first"}
            before = self.raw_file()
            del aps[ssid]
            self.write_saved(aps)
            why = self.generate()
            if why:
                self.restore(before)
                return {"error": why}
            for i, name, _ in self.nets():                               # and out of the running Wi-Fi
                if name == ssid:
                    self.ctl("REMOVE_NETWORK " + i)
        return {"ok": True}

    # ---- the rest ----
    def cmd_hostname(self, d):
        name = d.get("name")
        if not isinstance(name, str) or not HOST_RE.match(name):
            return {"error": "a name is lower-case letters, digits and - (up to 32)"}
        rc, out = self.run(self.a.hostnamectl, "set-hostname", name)
        if rc:
            return {"error": "hostnamectl: " + out.strip()[-160:]}
        self.run(self.a.systemctl, "try-restart", "avahi-daemon.service")
        return {"ok": True}

    def later(self, *cmd):
        """Run after the answer is out"""
        threading.Timer(1.0, lambda: self.run(*cmd, timeout=60)).start()

    def cmd_restart_bridge(self, d):
        self.later(self.a.systemctl, "restart", "apcbox-bridge.service")
        return {"ok": True}

    def cmd_reboot(self, d):
        self.later(self.a.systemctl, "reboot")
        return {"ok": True}

    def cmd_poweroff(self, d):
        self.later(self.a.systemctl, "poweroff")
        return {"ok": True}

    # Bluetooth. The chip's attach tool must never be stopped once it has attached:
    # on this driver a killed one hangs in the kernel and nothing can attach again
    # until a reboot (rig, 2026-11-23). So it attaches once per boot, and on / off
    # is the radio's block switch plus the Bluetooth service.
    def bt_radios(self):
        out = []
        try:
            for n in sorted(os.listdir(self.a.rfkill_dir)):
                p = os.path.join(self.a.rfkill_dir, n)
                try:
                    if open(os.path.join(p, "type")).read().strip() == "bluetooth":
                        out.append(os.path.join(p, "soft"))
                except OSError:
                    pass
        except OSError:
            pass
        return out

    def bt_attached(self):
        try:
            return any(n.startswith("hci") for n in os.listdir(self.a.bt_class_dir))
        except OSError:
            return False

    def bt_block(self, block):
        for p in self.bt_radios():
            try:
                with open(p, "w") as f:
                    f.write("1" if block else "0")
            except OSError:
                pass

    def bt(self):
        tool, attached = os.path.exists(self.a.attach), self.bt_attached()
        soft = []
        for p in self.bt_radios():
            try:
                soft.append(open(p).read().strip())
            except OSError:
                pass
        on = attached and bool(soft) and all(x == "0" for x in soft)
        show = self.run(self.a.bluetoothctl, "show", timeout=6)[1] if on else ""
        return {"tool": tool, "attached": attached, "on": on, "powered": "Powered: yes" in show}

    def cmd_bt(self, d):
        on = d.get("on")
        if not isinstance(on, bool):
            return {"error": "on: true or false"}
        if not on:
            self.bt_block(True)
            self.run(self.a.systemctl, "stop", "bluetooth.service", timeout=20)
            self.run(self.a.systemctl, "disable", BT_UNIT, timeout=20)     # not at the next boot; the chip stays attached till then
            return {"ok": True}
        if not os.path.exists(self.a.attach):
            return {"error": "the attach tool isn't on the box (%s): see t95/helper/README.md" % self.a.attach}
        self.run(self.a.systemctl, "enable", BT_UNIT, timeout=20)
        if not self.bt_attached():
            self.run(self.a.systemctl, "start", "--no-block", BT_UNIT, timeout=20)
            end = time.monotonic() + self.a.bt_wait_s
            while not self.bt_attached() and time.monotonic() < end:
                time.sleep(0.25)
            if not self.bt_attached():
                return {"error": "the chip didn't attach. Reboot the box (System) and it starts by itself; "
                                 "if it still doesn't: journalctl -u " + BT_UNIT}
        self.bt_block(False)
        self.run(self.a.systemctl, "start", "--no-block", "bluetooth.service", timeout=20)
        return {"ok": True}

    def cmd_status(self, d):
        ssid, state, ip = self.wifi_now()
        with self.lock:
            t = self.trial
            left = max(0, int(t["deadline"] - time.monotonic()) + 1) if t else 0
            trial = {"ssid": t["ssid"], "phase": t["phase"], "left_s": left} if t else None
            last = self.last
        return {"ok": True, "wifi": {"ssid": ssid, "state": state, "ip": ip, "iface": self.a.iface},
                "saved": sorted(self.saved()), "hostname": socket.gethostname(),
                "trial": trial, "last": last, "pin_set": self.pin_set_p(), "bt": self.bt()}

    def handle(self, d):
        cmd = d.get("cmd") if isinstance(d, dict) else None
        fn = getattr(self, "cmd_" + cmd, None) if isinstance(cmd, str) and re.match(r"^[a-z_]+$", cmd) else None
        if fn is None:
            return {"error": "unknown command"}
        if cmd in RISKY:
            why = self.pin_ok(d.get("pin"))
            if why:
                return {"error": why, "pin": True}
        try:
            return fn(d)
        except Exception as e:                                 # a bug here must not take the helper down
            log.error("%s failed: %s", cmd, type(e).__name__)
            return {"error": "the helper tripped over that (%s)" % type(e).__name__}


def serve(a):
    h = Helper(a)
    os.makedirs(a.state_dir, mode=0o700, exist_ok=True)
    try:
        os.unlink(a.socket)
    except OSError:
        pass

    class Conn(socketserver.StreamRequestHandler):
        def handle(self):
            self.connection.settimeout(90)
            try:
                line = self.rfile.readline(8192)
                try:
                    out = h.handle(json.loads(line.decode()))
                except ValueError:
                    out = {"error": "JSON expected"}
                self.wfile.write(json.dumps(out).encode() + b"\n")
            except OSError:
                pass

    class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
        daemon_threads = True

    old = os.umask(0o117)
    srv = Server(a.socket, Conn)
    os.umask(old)
    try:                                                         # only the bridge's group may talk to it
        os.chown(a.socket, 0, grp.getgrnam(a.group).gr_gid)
        os.chmod(a.socket, 0o660)
    except (KeyError, PermissionError):
        os.chmod(a.socket, 0o600)
    log.info("listening on %s", a.socket)
    srv.serve_forever()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--socket", default="/run/ghettorack/helper.sock")
    ap.add_argument("--group", default="ghettorack")
    ap.add_argument("--state-dir", default="/var/lib/ghettorack")
    ap.add_argument("--netplan-dir", default="/etc/netplan")
    ap.add_argument("--iface", default="wlan0")
    ap.add_argument("--trial-s", type=float, default=60, help="time to confirm once the box is on the new network")
    ap.add_argument("--connect-s", type=float, default=25, help="time the box gets to connect to it")
    ap.add_argument("--watch-s", type=float, default=0.5)
    ap.add_argument("--select-s", type=float, default=1.0, help="pause between answering 'join' and leaving the current network")
    ap.add_argument("--wpa-ctrl", default="/run/wpa_supplicant", help="directory of wpa_supplicant's control sockets")
    ap.add_argument("--scan-s", type=float, default=3)
    ap.add_argument("--lock-s", type=float, default=60)
    ap.add_argument("--attach", default=ATTACH)
    ap.add_argument("--rfkill-dir", default="/sys/class/rfkill")
    ap.add_argument("--bt-class-dir", default="/sys/class/bluetooth")
    ap.add_argument("--bt-wait-s", type=float, default=20)
    for tool, path in (("wpa-cli", "/usr/sbin/wpa_cli"), ("netplan", "/usr/sbin/netplan"), ("hostnamectl", "/usr/bin/hostnamectl"),
                       ("systemctl", "/usr/bin/systemctl"), ("bluetoothctl", "/usr/bin/bluetoothctl")):
        ap.add_argument("--" + tool, default=path)
    a = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    serve(a)


if __name__ == "__main__":
    main()
