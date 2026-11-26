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
    hotspot_set psk auto  the password of the box's own network, and whether
                          it starts by itself when no known Wi-Fi connects
    hotspot on|off      the box's own network, for where there is no Wi-Fi:
                          named like the box, fixed address, no internet.
                          Started by hand it stops again if nobody joins it
    hostname name       the box's name on the network
    bt on|off           Bluetooth on the UWE5622 (needs the attach tool): the
                          chip attaches once per boot and is never detached;
                          on / off is the radio's block switch
    restart_bridge, reboot, poweroff
    card_status         the SD card in the T95's slot, and a copy under way
    card_copy take      one take of the jam recorder onto the card, as one WAV
                          in its folder jams. The card is made writable for
                          the copy and read-only again after it; only a FAT or
                          exFAT card in the SD slot, only from the takes folder
    card_eject          unmount the card for the whole box (not during a copy)
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
import stat
import struct
import subprocess
import tempfile
import threading
import time

log = logging.getLogger("helper")
RISKY = {"join", "forget", "hotspot", "hotspot_set", "hostname", "bt", "bt_pair", "bt_forget", "restart_bridge", "reboot", "poweroff"}
HOST_RE = re.compile(r"^[a-z0-9]([a-z0-9-]{0,30}[a-z0-9])?$")
PIN_RE = re.compile(r"^[0-9]{4,8}$")
ATTACH = "/usr/local/bin/hciattach_opi"
BT_UNIT = "ghettorack-bluetooth.service"
BT_ADDR_RE = re.compile(r"^[0-9A-F]{2}(:[0-9A-F]{2}){5}$")
BT_MIDI_UUID = "03b80e5a-ede8-4b33-a751-6ce34ec4c700"      # the Bluetooth LE MIDI service
TAKE_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9 _.-]{0,47}")     # as the bridge names takes (rackbridge/jam.py)
WAV_MAX = 0xFFFFFFFF - 44
BT_WHY = {"AuthenticationFailed": "it refused (if it was paired with the box before, forget the box on the device first)", "AuthenticationCanceled": "it gave up",
          "AuthenticationTimeout": "it didn't answer", "AuthenticationRejected": "it said no", "ConnectionAttemptFailed": "it didn't answer",
          "NotReady": "Bluetooth isn't ready yet", "InProgress": "it is busy, try again"}


class Helper:
    def __init__(self, a):
        self.a = a
        self.file = os.path.join(a.netplan_dir, "40-ghettorack-wifi.yaml")
        self.pin_file = os.path.join(a.state_dir, "pin")
        self.lock = threading.Lock()
        self.trial = None           # {id, ssid, psk, phase: connecting|connected, deadline}
        self.last = None            # how the last try ended: {ssid, ok, why}
        self.ctl_n = 0
        self.hot_file = os.path.join(a.state_dir, "hotspot")
        self.hot = None             # {id, auto, since, seen, idle, scan, proc, clients}
        self.down_since = None      # since when the Wi-Fi has had no network
        self.fails, self.locked_until = 0, 0.0
        self.bt_cache = None        # (when, the paired devices)
        self.card = None            # the last copy to the SD card: {take, state, bytes, total, file, error}

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
        """drop the tried network and let the ones from before connect again (or the hotspot, if that was up)"""
        self.ctl("REMOVE_NETWORK " + nid)
        if self.hot:
            self.ctl("SELECT_NETWORK " + self.hot["id"])
            return
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
            if self.hot:                                                 # joined from the hotspot: that one is done
                self.hot_clear_locked()
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

    # ---- the box's own network ----
    # Seen by hand on the rig (2026-11-23): the same running wpa_supplicant puts wlan0 into AP
    # mode (mode=2 network), a phone saw it and passed the WPA2 handshake. Addresses for the
    # phones come from dnsmasq (DHCP only, no DNS, no route: a phone keeps its own internet).
    def hot_cfg(self):
        try:
            c = json.load(open(self.hot_file))
            return {"psk": c.get("psk", ""), "auto": bool(c.get("auto", False))}
        except (OSError, ValueError, AttributeError):
            return {"psk": "", "auto": False}        # off until the hotspot is seen working on the rig

    def cmd_hotspot_set(self, d):
        c = self.hot_cfg()
        if "psk" in d:
            psk = d["psk"]
            if not isinstance(psk, str) or not 8 <= len(psk) <= 63 or any(ord(ch) < 32 or ord(ch) > 126 for ch in psk):
                return {"error": "a hotspot password is 8 to 63 characters"}
            c["psk"] = psk
        if "auto" in d:
            if not isinstance(d["auto"], bool):
                return {"error": "auto: true or false"}
            c["auto"] = d["auto"]
        tmp = self.hot_file + ".tmp"
        with open(os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
            json.dump(c, f)
        os.replace(tmp, self.hot_file)
        return {"ok": True}

    def hot_why_not(self):
        if not self.hot_cfg()["psk"]:
            return "set a hotspot password first: the box never opens a network without one"
        if not os.path.exists(self.a.dnsmasq):
            return "the hotspot needs dnsmasq on the box: sudo apt install dnsmasq-base"
        return None

    def hot_start_locked(self, auto):
        """None = it is coming up, else why not"""
        why = self.hot_why_not()
        if why:
            return why
        name = socket.gethostname()
        nid = self.ctl("ADD_NETWORK")
        if not nid.isdigit():
            return "the Wi-Fi isn't answering"
        key = hashlib.pbkdf2_hmac("sha1", self.hot_cfg()["psk"].encode(), name.encode(), 4096, 32).hex()
        for k, v in (("ssid", name.encode().hex()), ("mode", "2"), ("frequency", str(self.a.hot_freq)), ("key_mgmt", "WPA-PSK"),
                     ("proto", "RSN"), ("pairwise", "CCMP"), ("psk", key)):
            if self.ctl("SET_NETWORK %s %s %s" % (nid, k, v)) != "OK":
                self.ctl("REMOVE_NETWORK " + nid)
                return "the Wi-Fi didn't take the hotspot (%s)" % k
        if self.ctl("SELECT_NETWORK " + nid) != "OK":
            self.ctl("REMOVE_NETWORK " + nid)
            self.ctl("ENABLE_NETWORK all")
            return "the Wi-Fi didn't start the hotspot"
        now = time.monotonic()
        self.hot = {"id": nid, "auto": auto, "since": now, "seen": False, "idle": now, "scan": None, "proc": None, "clients": 0}
        log.info("hotspot up (%s)", "by itself: no known Wi-Fi" if auto else "from Settings")
        return None

    def hot_clear_locked(self):
        """take the hotspot's own pieces away; what the Wi-Fi does next is the caller's"""
        h, self.hot = self.hot, None
        self.down_since = None
        self.ctl("REMOVE_NETWORK " + h["id"])
        if h["proc"] and h["proc"].poll() is None:
            h["proc"].terminate()
        self.run(self.a.ip, "addr", "del", self.a.hot_addr + "/24", "dev", self.a.iface)

    def hot_stop_locked(self, why):
        self.hot_clear_locked()
        self.ctl("ENABLE_NETWORK all")
        self.ctl("REASSOCIATE")
        log.info("hotspot down (%s)", why)

    def hot_clients(self):
        n, r = 0, self.ctl("STA-FIRST")
        while r and not r.startswith("FAIL") and n < 32:
            n += 1
            r = self.ctl("STA-NEXT " + r.splitlines()[0])
        return n

    def hot_keep_locked(self, now):
        """every few seconds while it is up: address, address server, who is on it, the way back"""
        h = self.hot
        # the address: put back if the network service took it off when the link came up
        if (self.a.hot_addr + "/") not in self.run(self.a.ip, "-4", "-o", "addr", "show", "dev", self.a.iface)[1]:
            self.run(self.a.ip, "addr", "add", self.a.hot_addr + "/24", "dev", self.a.iface)
        if h["proc"] is None or h["proc"].poll() is not None:
            base = self.a.hot_addr.rsplit(".", 1)[0]
            try:
                h["proc"] = subprocess.Popen(
                    [self.a.dnsmasq, "--keep-in-foreground", "--conf-file=/dev/null", "--port=0", "--interface=" + self.a.iface,
                     "--bind-interfaces", "--dhcp-range=%s.10,%s.99,1h" % (base, base), "--dhcp-option=3", "--dhcp-option=6",
                     "--dhcp-leasefile=" + os.path.join(os.path.dirname(self.a.socket), "hotspot.leases")],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            except OSError:
                h["proc"] = None
        h["clients"] = self.hot_clients()
        if h["clients"]:
            h["seen"], h["idle"], h["scan"] = True, now, None
            return
        if not h["auto"] and not h["seen"] and now - h["since"] > self.a.hot_trial_s:
            self.hot_stop_locked("nobody joined it: back to the Wi-Fi")
            self.last = {"ssid": socket.gethostname(), "ok": False, "why": "nobody joined the hotspot"}
            return
        # Nobody on it: look now and then whether a known network is back. Whether this driver
        # scans while it is an access point is not known (not seen on the rig yet). If it
        # doesn't, a hotspot that came up by itself steps aside for a moment instead: the
        # Wi-Fi gets auto_s to find a network, else the hotspot returns.
        if not h["auto"]:                                     # started by hand: it stays until turned off
            return
        if h["scan"] is None and now - h["idle"] > self.a.rescan_s:
            h["idle"] = now
            if self.ctl("SCAN") == "OK":
                h["scan"] = now
            else:
                self.hot_stop_locked("looking for a known Wi-Fi")
        elif h["scan"] is not None and now - h["scan"] > self.a.scan_s:
            h["scan"] = None
            known = {name for i, name, _ in self.nets() if i != h["id"]}
            seen = {l.split("\t")[4] for l in self.ctl("SCAN_RESULTS").splitlines()[1:] if len(l.split("\t")) > 4}
            if known & seen or not seen:
                self.hot_stop_locked("a known Wi-Fi is back" if known & seen else "looking for a known Wi-Fi")

    def keeper(self):
        """the hotspot's housekeeping, and starting it when the box has had no Wi-Fi for a while"""
        # A hotspot left over from a helper that was restarted: nothing looks after it and
        # its address server went with that helper. Take it down; if it is still needed it
        # comes back by itself.
        with self.lock:
            left = [i for i, _, _ in self.nets() if self.ctl("GET_NETWORK %s mode" % i) == "2"]
            for i in left:
                self.ctl("REMOVE_NETWORK " + i)
            if left:
                self.ctl("ENABLE_NETWORK all")
                self.ctl("REASSOCIATE")
                self.run(self.a.ip, "addr", "del", self.a.hot_addr + "/24", "dev", self.a.iface)
                log.info("took down a hotspot left over from before")
        while True:
            time.sleep(self.a.keep_s)
            try:
                with self.lock:
                    now = time.monotonic()
                    if self.trial:
                        self.down_since = None
                    elif self.hot:
                        self.hot_keep_locked(now)
                    else:
                        st = dict(l.split("=", 1) for l in self.ctl("STATUS").splitlines() if "=" in l)
                        if not st or st.get("wpa_state") == "COMPLETED" or not self.hot_cfg()["auto"] or self.hot_why_not():
                            self.down_since = None
                        elif self.down_since is None:
                            self.down_since = now
                        elif now - self.down_since > self.a.auto_s:
                            self.down_since = None
                            self.hot_start_locked(True)
            except Exception as e:                             # must not die: it is the way back
                log.error("keeper: %s", type(e).__name__)

    def cmd_hotspot(self, d):
        on = d.get("on")
        if not isinstance(on, bool):
            return {"error": "on: true or false"}
        with self.lock:
            if self.trial:
                return {"error": "a network is being tried: wait for it"}
            if not on:
                if self.hot:
                    threading.Timer(self.a.select_s, self.hot_off_later).start()
                return {"ok": True}
            if self.hot:
                return {"ok": True}
            why = self.hot_why_not()
            if why:
                return {"error": why}
        threading.Timer(self.a.select_s, self.hot_on_later).start()      # the answer first: the box leaves the Wi-Fi for it
        return {"ok": True, "ssid": socket.gethostname(), "addr": self.a.hot_addr, "trial_s": self.a.hot_trial_s}

    def hot_on_later(self):
        with self.lock:
            if not self.hot and not self.trial:
                why = self.hot_start_locked(False)
                if why:
                    self.last = {"ssid": socket.gethostname(), "ok": False, "why": why}

    def hot_off_later(self):
        with self.lock:
            if self.hot and not self.trial:
                self.hot_stop_locked("turned off in Settings")

    def hot_status(self):
        c, h = self.hot_cfg(), self.hot
        return {"set": bool(c["psk"]), "auto": c["auto"], "tool": os.path.exists(self.a.dnsmasq), "on": bool(h),
                "by": ("itself" if h["auto"] else "hand") if h else "", "clients": h["clients"] if h else 0,
                "ssid": socket.gethostname(), "addr": self.a.hot_addr}

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

    # Bluetooth devices. bluetoothctl, one command per call; what it prints goes nowhere
    # but through the patterns here. BlueZ forgets a device it only saw in a scan about
    # half a minute after the scan ends, so pairing keeps a scan going until it is done.
    def btctl(self, *args, timeout=8):
        return self.run(self.a.bluetoothctl, *args, timeout=timeout)[1]

    def bt_info(self, addr):
        out = self.btctl("info", addr, timeout=4)
        if "not available" in out or "Device " not in out:
            return None

        def f(k):
            m = re.search(r"^\s*%s: (.*)$" % k, out, re.M)
            return m.group(1).strip() if m else ""
        m = re.search(r"RSSI: .*\((-?\d+)\)", out)
        name = f("Name") or f("Alias")
        if not name or name.replace("-", ":").upper() == addr:
            name = ""
        return {"addr": addr, "name": "".join(c for c in name if c.isprintable())[:40], "midi": BT_MIDI_UUID in out.lower(),
                "paired": f("Paired") == "yes", "connected": f("Connected") == "yes", "rssi": int(m.group(1)) if m else None}

    def bt_addrs(self, *which):
        return [a for a in re.findall(r"^Device (\S+)", self.btctl("devices", *which), re.M) if BT_ADDR_RE.match(a)]

    def bt_devices(self):
        """The paired ones, for the status (asked every few seconds: kept for a moment)"""
        now = time.monotonic()
        if self.bt_cache and now - self.bt_cache[0] < 4:
            return self.bt_cache[1]
        out = [i for i in (self.bt_info(a) for a in self.bt_addrs("Paired")[:16]) if i]
        self.bt_cache = (now, out)
        return out

    def cmd_bt_scan(self, d):
        if not self.bt()["on"]:
            return {"error": "Bluetooth is off"}
        scan = None
        try:                                    # read while it still scans: BlueZ drops the signal figure when it stops
            scan = subprocess.Popen([self.a.bluetoothctl, "--timeout", str(int(self.a.bt_scan_s) + 20), "scan", "on"],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError:
            pass
        try:
            time.sleep(self.a.bt_scan_s)
            seen = [i for i in (self.bt_info(a) for a in self.bt_addrs()[:48]) if i]
        finally:
            if scan is not None:
                scan.terminate()
                try:
                    scan.wait(3)
                except subprocess.TimeoutExpired:
                    scan.kill()
        seen = [i for i in seen if i["name"] or i["midi"] or i["paired"]]         # the nameless ones are beacons and the like
        seen.sort(key=lambda i: (not i["midi"], not i["paired"], -(i["rssi"] if i["rssi"] is not None else -200)))
        return {"ok": True, "devices": seen[:16]}

    def cmd_bt_pair(self, d):
        """One bluetoothctl session, in the order that works with this radio (rig, 2026-11-24, an iPad):
        connect while a scan runs (the radio only finds a private address then), stop the scan (the link
        doesn't settle under one), then pair, and say yes to the code: the device shows the same one and
        its owner confirms it there. Afterwards the device may be listed under another address (its real one)."""
        addr = d.get("addr")
        if not isinstance(addr, str) or not BT_ADDR_RE.match(addr):
            return {"error": "addr: a device from the scan"}
        if not self.bt()["on"]:
            return {"error": "Bluetooth is off"}
        before = set(self.bt_addrs("Paired"))
        if addr in before:
            return {"ok": True, "device": self.bt_info(addr)}
        log = tempfile.TemporaryFile()
        try:
            ctl = subprocess.Popen([self.a.bluetoothctl], stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
        except OSError:
            return {"error": "bluetoothctl isn't installed"}

        def say(line):
            try:
                ctl.stdin.write(line.encode() + b"\n")
                ctl.stdin.flush()
            except OSError:
                pass

        def wait(what, seconds, each=None):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                r = what()
                if r:
                    return r
                if each:
                    each()
                time.sleep(self.a.bt_step_s)
            return what()
        new, why = None, "no answer from it"
        try:
            say("scan on")
            if not wait(lambda: self.bt_info(addr), self.a.bt_find_s):
                return {"error": "it isn't in reach any more: put it in pairing mode and scan again"}
            say("connect " + addr)
            up = wait(lambda: (self.bt_info(addr) or {}).get("connected"), self.a.bt_connect_s)
            say("scan off")
            time.sleep(min(0.5, self.a.bt_step_s))
            if up:
                say("pair " + addr)

                def paired():
                    now = self.bt_addrs("Paired")
                    return addr if addr in now else next((a for a in now if a not in before), None)

                def failed():
                    log.seek(0)
                    return re.search(r"Failed to pair: org\.bluez\.Error\.(\w+)", log.read().decode(errors="replace"))
                new = wait(lambda: paired() or failed(), self.a.bt_pair_s, lambda: say("yes"))
                if new and not isinstance(new, str):
                    new, why = None, BT_WHY.get(new.group(1), new.group(1))
                if new:
                    say("trust " + new)
                    time.sleep(min(0.5, self.a.bt_step_s))
        finally:
            say("quit")
            try:
                ctl.wait(3)
            except subprocess.TimeoutExpired:
                ctl.kill()
            log.close()
            self.bt_cache = None
        if not new:
            return {"error": "it didn't pair: " + why}
        return {"ok": True, "device": self.bt_info(new) or {"addr": new, "name": "", "midi": False, "paired": True, "connected": False, "rssi": None}}

    def cmd_bt_forget(self, d):
        addr = d.get("addr")
        if not isinstance(addr, str) or not BT_ADDR_RE.match(addr):
            return {"error": "addr: a paired device"}
        self.btctl("remove", addr)
        self.bt_cache = None
        if addr in self.bt_addrs("Paired"):
            return {"error": "it is still there"}
        return {"ok": True}

    # ---- the SD card in the T95's slot: takes of the jam recorder copied onto it, eject ----
    # The card is mounted read-only (udev rule, t95/bridge/99-apcbox.rules), so pulling it is
    # always safe. It is writable only while a copy runs here.
    def card_mount(self):
        """(device, filesystem, read-only) of what is mounted at the card's place, or None"""
        try:
            with open(self.a.mounts) as f:
                for line in f:
                    p = line.split()
                    if len(p) >= 4 and p[1] == self.a.card_mount:
                        return p[0], p[2], "ro" in p[3].split(",")
        except OSError:
            pass
        return None

    def card_why_not(self, m):
        """None if what is mounted there is a FAT / exFAT card in the SD slot"""
        if m is None:
            return "no SD card in the box"
        disk = re.match(r"^(mmcblk\d+)(p\d+)?$", os.path.basename(m[0]))
        kind = ""
        try:
            if disk:
                with open(os.path.join(self.a.sysblock, disk.group(1), "device", "type")) as f:
                    kind = f.read().strip()
        except OSError:
            pass
        if kind != "SD" or m[1] not in ("vfat", "exfat"):
            return "what is mounted there isn't a FAT or exFAT card in the SD slot"
        return None

    def card_remount(self, how):
        return self.run(self.a.mount, "-o", "remount," + how, self.a.card_mount, timeout=30)[0] == 0

    @staticmethod
    def wav_sound(fd):
        """(offset, bytes) of the sound in an open WAV file, by what the file holds (not what its header says)"""
        size = os.fstat(fd).st_size
        head = os.pread(fd, 4096, 0)
        if head[:4] != b"RIFF" or head[8:12] != b"WAVE":
            return None
        pos = 12
        while pos + 8 <= len(head):
            cid, n = head[pos:pos + 4], struct.unpack("<I", head[pos + 4:pos + 8])[0]
            if cid == b"data":
                return pos + 8, max(0, (size - pos - 8) // 4 * 4)
            pos += 8 + n + (n & 1)
        return None

    def take_open(self, take):
        """The take's parts, open: [(fd, offset, bytes)]. Plain files in the takes folder only:
        the bridge has no rights and writes there, so nothing is followed out of it."""
        d = os.path.join(self.a.jams_dir, take)
        st = os.lstat(d)
        if not stat.S_ISDIR(st.st_mode):
            raise OSError("not a folder")
        names = []
        for n in os.listdir(d):
            m = re.fullmatch(r"part(?:-(\d+))?\.wav", n)
            if m:
                names.append((int(m.group(1) or 0), n))
        out = []
        try:
            for _, n in sorted(names):
                fd = os.open(os.path.join(d, n), os.O_RDONLY | os.O_NOFOLLOW)
                out.append((fd, 0, 0))
                if not stat.S_ISREG(os.fstat(fd).st_mode):
                    raise OSError("not a file")
                w = self.wav_sound(fd)
                out[-1] = (fd, w[0], w[1]) if w else (fd, 0, 0)
        except OSError:
            for fd, _, _ in out:
                os.close(fd)
            raise
        return out

    def cmd_card_status(self, d):
        m = self.card_mount()
        why = self.card_why_not(m)
        card = {"mounted": m is not None, "ok": why is None, "why": why}
        if m:
            card.update(fs=m[1], read_only=m[2])
            try:
                v = os.statvfs(self.a.card_mount)
                card.update(bytes=v.f_frsize * v.f_blocks, free=v.f_frsize * v.f_bavail)
            except OSError:
                pass
        with self.lock:
            return {"ok": True, "card": card, "copy": dict(self.card) if self.card else None}

    def cmd_card_copy(self, d):
        take = d.get("take")
        if not isinstance(take, str) or not TAKE_RE.fullmatch(take) or ".." in take or take.endswith((" ", ".")):
            return {"error": "no such take"}
        why = self.card_why_not(self.card_mount())
        if why:
            return {"error": why}
        with self.lock:
            if self.card and self.card["state"] == "copying":
                return {"error": "a copy is running: %s" % self.card["take"]}
        try:
            parts = self.take_open(take)
        except OSError:
            return {"error": "no such take"}
        data = sum(p[2] for p in parts)
        dst = os.path.join(self.a.card_mount, "jams", take + ".wav")
        why = None
        try:
            if not data:
                why = "nothing in that take"
            elif data > WAV_MAX:
                why = "over 4 GB: too long for one WAV file. Download it from the page instead"
            elif os.path.exists(dst) and os.path.getsize(dst) == 44 + data:
                why = "already on the card (jams/%s.wav)" % take
            elif os.path.exists(dst):
                why = "another file of that name is on the card"
            elif os.statvfs(self.a.card_mount).f_frsize * os.statvfs(self.a.card_mount).f_bavail < 44 + data + (16 << 20):
                why = "not enough room on the card"
        except OSError as e:
            why = "the card can't be read (%s)" % type(e).__name__
        if why:
            for fd, _, _ in parts:
                os.close(fd)
            return {"error": why}
        with self.lock:
            self.card = {"take": take, "state": "copying", "bytes": 0, "total": 44 + data, "file": "jams/%s.wav" % take, "error": None}
        threading.Thread(target=self.card_copy_run, args=(parts, data, dst), daemon=True).start()
        return {"ok": True, "copy": dict(self.card)}

    def card_copy_run(self, parts, data, dst):
        tmp = dst + ".part"
        err = None
        try:
            if not self.card_remount("rw"):
                raise OSError("the card can't be made writable")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(tmp, "wb") as f:
                f.write(b"RIFF" + struct.pack("<I", 36 + data) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, 48000, 192000, 4, 16)
                        + b"data" + struct.pack("<I", data))
                done = 44
                for fd, off, n in parts:
                    pos = 0
                    while pos < n:
                        chunk = os.pread(fd, min(1 << 20, n - pos), off + pos)
                        if not chunk:
                            raise OSError("the take got shorter")
                        f.write(chunk)
                        pos += len(chunk)
                        done += len(chunk)
                        with self.lock:
                            self.card["bytes"] = done
                f.flush()
                os.fsync(f.fileno())
            os.rename(tmp, dst)
        except OSError as e:
            err = str(e) if e.args and isinstance(e.args[0], str) else (e.strerror or type(e).__name__)
            try:
                os.unlink(tmp)
            except OSError:
                pass
        finally:
            for fd, _, _ in parts:
                os.close(fd)
            try:
                os.sync()
            except OSError:
                pass
            # read-only again, whatever happened. A card that was pulled has no mount left to change.
            back = any(self.card_remount("ro") or time.sleep(1) for _ in range(5)) if self.card_mount() else True
            m = self.card_mount()
            if not back and m and not m[2]:
                err = (err + "; " if err else "") + "the card is still writable: eject it before pulling it"
        with self.lock:
            self.card["state"] = "failed" if err else "done"
            self.card["error"] = err
        log.info("card: copy of a take %s", "failed (%s)" % err if err else "done")

    def cmd_card_eject(self, d):
        with self.lock:
            if self.card and self.card["state"] == "copying":
                return {"error": "a copy to the card is running"}
        if self.card_mount() is None:
            return {"ok": True, "was": "not mounted"}
        try:
            os.sync()
        except OSError:
            pass
        # Through systemd, which mounted it (systemd-mount in the udev rule). This helper has a mount
        # namespace of its own (ProtectHome): an umount run here took the card away from the helper
        # only and left it mounted for everything else on the box (rig, 2026-11-24).
        rc, out = self.run(self.a.systemctl, "stop", self.a.card_unit, timeout=30)
        for _ in range(20):                      # the unmount reaches this namespace a moment later
            if self.card_mount() is None:
                return {"ok": True}
            time.sleep(0.1)
        return {"error": "the card is in use (a sample being read?): try again in a moment" if "busy" in out else "the card can't be unmounted"}

    def cmd_status(self, d):
        ssid, state, ip = self.wifi_now()
        with self.lock:
            t = self.trial
            left = max(0, int(t["deadline"] - time.monotonic()) + 1) if t else 0
            trial = {"ssid": t["ssid"], "phase": t["phase"], "left_s": left} if t else None
            last = self.last
            hot = self.hot_status()
        bt = self.bt()
        return {"ok": True, "wifi": {"ssid": ssid, "state": state, "ip": ip, "iface": self.a.iface},
                "saved": sorted(self.saved()), "hostname": socket.gethostname(),
                "trial": trial, "last": last, "hotspot": hot, "pin_set": self.pin_set_p(), "bt": bt,
                "bt_devices": self.bt_devices() if bt["powered"] else []}

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
    threading.Thread(target=h.keeper, daemon=True).start()
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
    ap.add_argument("--hot-addr", default="10.42.0.1", help="the box's address on its own network")
    ap.add_argument("--hot-freq", type=int, default=2437, help="channel 6: 2.4 GHz reaches every phone")
    ap.add_argument("--auto-s", type=float, default=30, help="no Wi-Fi for this long: the hotspot starts by itself")
    ap.add_argument("--hot-trial-s", type=float, default=120, help="started by hand and nobody joins: back to the Wi-Fi")
    ap.add_argument("--rescan-s", type=float, default=120)
    ap.add_argument("--keep-s", type=float, default=2)
    ap.add_argument("--wpa-ctrl", default="/run/wpa_supplicant", help="directory of wpa_supplicant's control sockets")
    ap.add_argument("--scan-s", type=float, default=3)
    ap.add_argument("--lock-s", type=float, default=60)
    ap.add_argument("--attach", default=ATTACH)
    ap.add_argument("--rfkill-dir", default="/sys/class/rfkill")
    ap.add_argument("--bt-class-dir", default="/sys/class/bluetooth")
    ap.add_argument("--bt-wait-s", type=float, default=20)
    ap.add_argument("--bt-scan-s", type=float, default=8, help="how long a Bluetooth scan listens")
    ap.add_argument("--bt-find-s", type=float, default=12, help="pairing: how long to look for the device first")
    ap.add_argument("--bt-connect-s", type=float, default=20.0, help="how long a device gets to connect before pairing")
    ap.add_argument("--bt-pair-s", type=float, default=45.0, help="how long a pairing may take (its owner has to confirm on the device)")
    ap.add_argument("--card-mount", default="/media/sdcard", help="where the udev rule mounts the SD card")
    ap.add_argument("--jams-dir", default="/var/lib/apcbox-bridge/jams", help="the jam recorder's takes (the bridge's folder)")
    ap.add_argument("--card-unit", default="media-sdcard.mount", help="the mount unit systemd-mount makes for --card-mount")
    ap.add_argument("--mounts", default="/proc/self/mounts")
    ap.add_argument("--sysblock", default="/sys/block")
    ap.add_argument("--bt-step-s", type=float, default=1.0, help="how often a pairing looks whether the device has moved on")
    for tool, path in (("wpa-cli", "/usr/sbin/wpa_cli"), ("netplan", "/usr/sbin/netplan"), ("hostnamectl", "/usr/bin/hostnamectl"),
                       ("systemctl", "/usr/bin/systemctl"), ("bluetoothctl", "/usr/bin/bluetoothctl"),
                       ("ip", "/usr/sbin/ip"), ("dnsmasq", "/usr/sbin/dnsmasq"),
                       ("mount", "/usr/bin/mount")):
        ap.add_argument("--" + tool, default=path)
    a = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    serve(a)


if __name__ == "__main__":
    main()
