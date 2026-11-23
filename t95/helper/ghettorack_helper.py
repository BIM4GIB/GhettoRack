#!/usr/bin/env python3
"""GhettoRack settings helper: the few things on the T95 that need root.

The bridge (the web server) runs without rights on purpose. It asks this
helper over a local socket, and this helper does only what is on its list:

    status              Wi-Fi, address, name, Bluetooth, trial in progress
    scan                Wi-Fi networks in range
    join ssid psk       add a network and try it; it is kept only if
    confirm               "confirm" arrives within the trial time, else the
                          box goes back to how it was (no lockout)
    forget ssid         remove a network this helper added
    hostname name       the box's name on the network
    bt on|off           Bluetooth on the UWE5622 (needs the attach tool)
    restart_bridge, reboot, poweroff
    pin_set old new     the PIN for everything that changes something
    pin_check pin

One JSON object per line in, one per line out: {"cmd": ..., "pin": ...}.
Everything that changes the box needs the PIN; five wrong ones lock for a
minute. Passwords are written only to the box's own network config (mode
600) and never logged.

Wi-Fi: this box runs Netplan + systemd-networkd + wpa_supplicant. Networks
joined here go into one extra Netplan file (JSON, which is YAML) next to the
ones from the install; Netplan merges them. Nothing else is rewritten.

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
        self.trial = None           # (deadline, previous file content or None, ssid)
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

    def apply(self):
        """None = applied, else what netplan said"""
        rc, out = self.run(self.a.netplan, "generate")
        if rc:
            return "netplan refused it: " + out.strip()[-200:]
        rc, out = self.run(self.a.netplan, "apply", timeout=60)
        return None if rc == 0 else "netplan apply failed: " + out.strip()[-200:]

    def wifi_now(self):
        st = dict(l.split("=", 1) for l in self.wpa("status")[1].splitlines() if "=" in l)
        return st.get("ssid", ""), st.get("wpa_state", ""), st.get("ip_address", "")

    def cmd_scan(self, d):
        self.wpa("scan")
        time.sleep(self.a.scan_s)
        rc, out = self.wpa("scan_results")
        if rc:
            return {"error": "the Wi-Fi isn't answering (wpa_cli)"}
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
            before = self.raw_file()
            aps = self.saved()
            aps[ssid] = {"password": psk} if psk else {}
            self.write_saved(aps)
            why = self.apply()
            if why:
                self.restore(before)
                self.apply()
                return {"error": why}
            self.trial = (time.monotonic() + self.a.trial_s, before, ssid)
        threading.Thread(target=self.trial_watch, daemon=True).start()
        log.info("trying a Wi-Fi network for %d s", self.a.trial_s)     # no name, no password
        return {"ok": True, "trial_s": self.a.trial_s}

    def trial_watch(self):
        while True:
            time.sleep(0.2)
            with self.lock:
                if not self.trial:
                    return
                if time.monotonic() < self.trial[0]:
                    continue
                _, before, _ = self.trial
                self.trial = None
                self.restore(before)
                why = self.apply()
            log.warning("nobody confirmed the new Wi-Fi: back to how it was%s", " (%s)" % why if why else "")
            return

    def cmd_confirm(self, d):
        with self.lock:
            if not self.trial:
                return {"ok": True, "kept": False}
            self.trial = None
        log.info("new Wi-Fi confirmed: kept")
        return {"ok": True, "kept": True}

    def cmd_forget(self, d):
        ssid = d.get("ssid")
        with self.lock:
            aps = self.saved()
            if ssid not in aps:
                return {"error": "that network was set when the box was installed (or isn't saved): it can't be removed here"}
            before = self.raw_file()
            del aps[ssid]
            self.write_saved(aps)
            why = self.apply()
            if why:
                self.restore(before)
                self.apply()
                return {"error": why}
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

    def bt(self):
        tool = os.path.exists(self.a.attach)
        active = self.run(self.a.systemctl, "is-active", BT_UNIT)[1].strip() == "active"
        show = self.run(self.a.bluetoothctl, "show", timeout=6)[1] if active else ""
        return {"tool": tool, "on": active, "powered": "Powered: yes" in show,
                "controller": "Controller " in show}

    def cmd_bt(self, d):
        on = d.get("on")
        if not isinstance(on, bool):
            return {"error": "on: true or false"}
        if on and not os.path.exists(self.a.attach):
            return {"error": "the attach tool isn't on the box (%s): see t95/helper/README.md" % self.a.attach}
        rc, out = self.run(self.a.systemctl, "enable" if on else "disable", "--now", BT_UNIT, timeout=40)
        return {"ok": True} if rc == 0 else {"error": "systemctl: " + out.strip()[-160:]}

    def cmd_status(self, d):
        ssid, state, ip = self.wifi_now()
        with self.lock:
            left = max(0, int(self.trial[0] - time.monotonic())) if self.trial else 0
            trying = self.trial[2] if self.trial else ""
        return {"ok": True, "wifi": {"ssid": ssid, "state": state, "ip": ip, "iface": self.a.iface},
                "saved": sorted(self.saved()), "hostname": socket.gethostname(),
                "trial_s": left, "trying": trying, "pin_set": self.pin_set_p(), "bt": self.bt()}

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
    ap.add_argument("--trial-s", type=float, default=60)
    ap.add_argument("--scan-s", type=float, default=3)
    ap.add_argument("--lock-s", type=float, default=60)
    ap.add_argument("--attach", default=ATTACH)
    for tool, path in (("wpa-cli", "/usr/sbin/wpa_cli"), ("netplan", "/usr/sbin/netplan"), ("hostnamectl", "/usr/bin/hostnamectl"),
                       ("systemctl", "/usr/bin/systemctl"), ("bluetoothctl", "/usr/bin/bluetoothctl")):
        ap.add_argument("--" + tool, default=path)
    a = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    serve(a)


if __name__ == "__main__":
    main()
