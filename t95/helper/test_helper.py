#!/usr/bin/env python3
"""Tests for the settings helper, against fake wpa_cli / netplan / systemctl
(no root, no real network).   python3 t95/helper/test_helper.py"""
import json
import os
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import warnings

warnings.simplefilter("ignore", ResourceWarning)

HERE = os.path.dirname(os.path.abspath(__file__))
FAKE = """#!/bin/sh
echo "$(basename "$0") $*" >> "$FAKELOG"
case "$(basename "$0") $*" in
  "wpa_cli -i wlan0 status") printf 'bssid=aa\\nssid=Home Net\\nwpa_state=COMPLETED\\nip_address=198.51.100.7\\n' ;;
  "wpa_cli -i wlan0 scan_results") printf 'bssid / frequency / signal level / flags / ssid\\n'
     printf 'a\\t2412\\t-48\\t[WPA2-PSK-CCMP][ESS]\\tHome Net\\n'
     printf 'b\\t5180\\t-70\\t[WPA2-PSK-CCMP][ESS]\\tHome Net\\n'
     printf 'c\\t2437\\t-81\\t[ESS]\\tCafe Open\\n'
     printf 'd\\t2437\\t-60\\t[WPA2-PSK-CCMP][ESS]\\t\\n' ;;
  "ip addr add "*) touch "$FAKEDIR/hot-addr" ;;
  "ip addr del "*) rm -f "$FAKEDIR/hot-addr" ;;
  "ip -4 -o addr show dev wlan0") [ -f "$FAKEDIR/hot-addr" ] && echo "3: wlan0 inet 10.42.0.1/24 scope global wlan0" ;;
  "dnsmasq "*) echo $$ > "$FAKEDIR/dnsmasq.pid"; exec sleep 600 ;;
  "netplan generate") [ -f "$FAKEDIR/netplan-broken" ] && { echo "bad yaml"; exit 1; } ;;
  "systemctl start --no-block ghettorack-bluetooth.service") [ -f "$FAKEDIR/chip-dead" ] || mkdir -p "$FAKEDIR/btclass/hci0" ;;
  "bluetoothctl show") printf 'Controller XX:XX:XX:XX:XX:XX (public)\\n\\tPowered: yes\\n' ;;
esac
exit 0
"""


class FakeWpa(threading.Thread):
    """wpa_supplicant's control socket, as far as the helper uses it. mode: what a
    tried network does: "ok" connects, "wrong" gets temp-disabled, "never" keeps scanning."""

    def __init__(self, ctrl_dir, iface="wlan0"):
        super().__init__(daemon=True)
        os.makedirs(ctrl_dir, exist_ok=True)
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.s.bind(os.path.join(ctrl_dir, iface))
        self.mode, self.log = "ok", []
        self.nets = {"0": {"ssid": "Home Net", "off": False}}
        self.cur, self.next = "0", 1
        self.clients, self.air, self.ap_scan = [], None, True     # phones on the hotspot; networks in the air (None: any); scans as an AP
        self.start()

    def flags(self, i):
        n = self.nets[i]
        if i == self.cur:
            return "[CURRENT]"
        if n.get("wrong"):
            return "[TEMP-DISABLED]"
        return "[DISABLED]" if n["off"] else ""

    def settle(self):
        """connect to what is allowed (and in the air), as the real one would"""
        for i, n in self.nets.items():
            if not n["off"] and not n.get("wrong") and not n.get("never") and (n.get("mode") == "2" or self.air is None or n["ssid"] in self.air):
                self.cur = i
                return
        self.cur = None

    def answer(self, c):
        w = c.split(" ")
        if c == "ADD_NETWORK":
            i, self.next = str(self.next), self.next + 1
            self.nets[i] = {"ssid": "", "off": True}
            return i
        if w[0] == "SET_NETWORK" and w[1] in self.nets:
            if w[2] == "ssid":
                self.nets[w[1]]["ssid"] = bytes.fromhex(w[3]).decode()
            self.nets[w[1]][w[2]] = w[3] if w[2] != "ssid" else self.nets[w[1]]["ssid"]
            return "OK"
        if w[0] == "SELECT_NETWORK" and w[1] in self.nets:
            for i, n in self.nets.items():
                n["off"] = i != w[1]
            ap = self.nets[w[1]].get("mode") == "2"
            self.nets[w[1]]["wrong"], self.nets[w[1]]["never"] = self.mode == "wrong" and not ap, self.mode == "never" and not ap
            self.settle()
            return "OK"
        if w[0] == "REMOVE_NETWORK" and w[1] in self.nets:
            del self.nets[w[1]]
            self.settle()
            return "OK"
        if c == "ENABLE_NETWORK all":
            for n in self.nets.values():
                n["off"] = False
            if self.cur is None:
                self.settle()
            return "OK"
        if c == "REASSOCIATE":
            return "OK"
        if c == "STATUS":
            if self.cur is None:
                return "wpa_state=SCANNING"
            return "id=%s\nssid=%s\nwpa_state=COMPLETED" % (self.cur, self.nets[self.cur]["ssid"])
        if w[0] == "GET_NETWORK" and w[1] in self.nets:
            return self.nets[w[1]].get(w[2], "FAIL")
        if c == "STA-FIRST":
            return self.clients[0] if self.clients else ""
        if w[0] == "STA-NEXT":
            i = self.clients.index(w[1]) + 1 if w[1] in self.clients else len(self.clients)
            return self.clients[i] if i < len(self.clients) else ""
        if c == "SCAN":
            return "OK" if self.ap_scan or not self.ap() else "FAIL-BUSY"
        if c == "SCAN_RESULTS":
            return "bssid / frequency / signal level / flags / ssid\n" + "\n".join("a\t2412\t-50\t[ESS]\t" + n for n in (self.air or ["Home Net"]))
        if c == "LIST_NETWORKS":
            return "network id / ssid / bssid / flags\n" + "\n".join(
                "%s\t%s\tany\t%s" % (i, n["ssid"], self.flags(i)) for i, n in self.nets.items())
        return "FAIL"

    def run(self):
        while True:
            data, addr = self.s.recvfrom(4096)
            c = data.decode()
            self.log.append(c)
            self.s.sendto(self.answer(c).encode(), addr)

    def ap(self):
        """the hotspot network, if that is what the radio is doing"""
        n = self.nets.get(self.cur) if self.cur else None
        return n if n and n.get("mode") == "2" else None

    def names(self):
        return sorted(n["ssid"] for n in self.nets.values())


class HelperTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.bin = os.path.join(cls.dir, "bin")
        cls.netplan = os.path.join(cls.dir, "netplan")
        os.makedirs(cls.bin)
        os.makedirs(cls.netplan)
        open(os.path.join(cls.netplan, "30-t95-wifi.yaml"), "w").write("# from the install: never touched\n")
        tools = {}
        for t in ("wpa_cli", "netplan", "hostnamectl", "systemctl", "bluetoothctl", "ip", "dnsmasq"):
            p = os.path.join(cls.bin, t)
            open(p, "w").write(FAKE)
            os.chmod(p, 0o755)
            tools[t] = p
        for i, (typ, soft) in enumerate((("wlan", "0"), ("bluetooth", "1"))):                          # the radios' block switches
            os.makedirs(os.path.join(cls.dir, "rfkill", "rfkill%d" % i))
            open(os.path.join(cls.dir, "rfkill", "rfkill%d" % i, "type"), "w").write(typ + "\n")
            open(os.path.join(cls.dir, "rfkill", "rfkill%d" % i, "soft"), "w").write(soft)
        cls.log = os.path.join(cls.dir, "calls.log")
        cls.sock = os.path.join(cls.dir, "h.sock")
        cls.attach = os.path.join(cls.dir, "hciattach_opi")
        cls.wpa = FakeWpa(os.path.join(cls.dir, "wpa"))
        cls.wpa.nets["7"] = {"ssid": "left over", "off": False, "mode": "2"}                             # a hotspot from a helper that died
        cls.wpa.next = 8
        env = dict(os.environ, FAKELOG=cls.log, FAKEDIR=cls.dir)
        cls.proc = subprocess.Popen(
            [sys.executable, os.path.join(HERE, "ghettorack_helper.py"), "--socket", cls.sock, "--state-dir", os.path.join(cls.dir, "state"),
             "--netplan-dir", cls.netplan, "--trial-s", "1.5", "--select-s", "0.05", "--connect-s", "1.5", "--watch-s", "0.05",
             "--wpa-ctrl", os.path.join(cls.dir, "wpa"), "--ip", tools["ip"], "--dnsmasq", tools["dnsmasq"],
             "--keep-s", "0.05", "--auto-s", "0.6", "--hot-trial-s", "1.2", "--rescan-s", "0.5", "--scan-s", "0.05", "--lock-s", "1.2", "--attach", cls.attach,
             "--rfkill-dir", os.path.join(cls.dir, "rfkill"), "--bt-class-dir", os.path.join(cls.dir, "btclass"), "--bt-wait-s", "1",
             "--group", "no-such-group", "--wpa-cli", tools["wpa_cli"], "--netplan", tools["netplan"],
             "--hostnamectl", tools["hostnamectl"], "--systemctl", tools["systemctl"], "--bluetoothctl", tools["bluetoothctl"]],
            env=env, stdout=subprocess.DEVNULL, stderr=open(os.path.join(cls.dir, "helper.log"), "w"))
        for _ in range(100):
            if os.path.exists(cls.sock):
                break
            time.sleep(0.05)

    @classmethod
    def tearDownClass(cls):
        cls.proc.kill()

    def ask(self, **d):
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(10)
        s.connect(self.sock)
        s.sendall(json.dumps(d).encode() + b"\n")
        buf = b""
        while not buf.endswith(b"\n"):
            c = s.recv(65536)
            if not c:
                break
            buf += c
        s.close()
        return json.loads(buf.decode())

    def join(self, ssid, key):
        return self.ask(**{"cmd": "join", "pin": "4711", "ssid": ssid, "psk": key})

    def hot_set(self, pin, key, **more):
        return self.ask(**dict({"cmd": "hotspot_set", "pin": pin, "psk": key}, **more))

    def calls(self):
        return open(self.log).read().splitlines() if os.path.exists(self.log) else []

    def ours(self):
        return os.path.join(self.netplan, "40-ghettorack-wifi.yaml")

    def test_1_status_scan_need_no_pin(self):
        self.assertEqual(self.wpa.names(), ["Home Net"])                                               # the left-over hotspot is gone
        st = self.ask(cmd="status")
        self.assertEqual(st["wifi"], {"ssid": "Home Net", "state": "COMPLETED", "ip": "198.51.100.7", "iface": "wlan0"})
        self.assertEqual((st["saved"], st["trial"], st["last"], st["pin_set"]), ([], None, None, False))
        self.assertEqual(st["bt"], {"tool": False, "attached": False, "on": False, "powered": False})
        n = self.ask(cmd="scan")["networks"]
        self.assertEqual([(x["ssid"], x["signal"], x["secure"], x["current"]) for x in n],
                         [("Home Net", 100, True, True), ("Cafe Open", 38, False, False)])    # best of two, hidden one dropped
        self.assertEqual(self.ask(cmd="rm -rf /")["error"], "unknown command")
        self.assertEqual(self.ask(cmd="__init__")["error"], "unknown command")
        self.assertEqual(self.ask(cmd="handle")["error"], "unknown command")

    def test_2_pin(self):
        self.assertEqual(self.ask(cmd="reboot")["error"], "set a PIN first")
        self.assertIn("4 to 8 digits", self.ask(cmd="pin_set", new="12")["error"])
        self.assertIn("4 to 8 digits", self.ask(cmd="pin_set", new="abcd")["error"])
        self.assertEqual(self.ask(cmd="pin_set", new="4711"), {"ok": True})
        pf = os.path.join(self.dir, "state", "pin")
        self.assertEqual(stat.S_IMODE(os.stat(pf).st_mode), 0o600)
        self.assertNotIn("4711", open(pf).read())
        self.assertTrue(self.ask(cmd="status")["pin_set"])
        self.assertEqual(self.ask(cmd="pin_set", new="9999")["error"], "the old PIN is wrong")       # can't be replaced without it
        self.assertEqual(self.ask(cmd="pin_check", pin="4711"), {"ok": True})
        r = self.ask(cmd="hostname", name="box")
        self.assertEqual((r["error"], r["pin"]), ("wrong PIN", True))
        self.assertNotIn("hostnamectl set-hostname box", self.calls())
        for _ in range(5):
            self.ask(cmd="pin_check", pin="0000")
        self.assertIn("wait", self.ask(cmd="pin_check", pin="4711")["error"])                        # locked, even with the right one
        time.sleep(1.4)
        self.assertEqual(self.ask(cmd="pin_check", pin="4711"), {"ok": True})
        self.assertEqual(self.ask(cmd="pin_set", old="4711", new="123456"), {"ok": True})
        self.assertEqual(self.ask(cmd="pin_set", old="123456", new="4711"), {"ok": True})

    def test_3_join_is_a_trial(self):
        P = "4711"
        self.assertIn("8 to 63", self.join("X", "short")["error"])
        self.assertIn("can't be used", self.join("", "longenough")["error"])
        self.assertIn("can't be used", self.join("a\nb", "longenough")["error"])
        self.assertFalse(os.path.exists(self.ours()))
        def trial():
            return self.ask(cmd="status")["trial"]

        def ended(t=3):
            end = time.time() + t
            while trial() and time.time() < end:
                time.sleep(0.05)
            self.assertIsNone(trial())
            return self.ask(cmd="status")["last"]
        name = 'Studio "5G": x'
        # wrong password: back at once (not after the whole trial time), nothing saved, the old network is back
        self.wpa.mode = "wrong"
        t0 = time.time()
        r = self.join(name, "pass word 123")
        self.assertEqual(r, {"ok": True, "trial_s": 1.5, "connect_s": 1.5})
        self.assertEqual(ended(), {"ssid": name, "ok": False, "why": "wrong password"})
        self.assertLess(time.time() - t0, 1.0)
        self.assertEqual((self.wpa.names(), self.wpa.cur), (["Home Net"], "0"))
        self.assertFalse(os.path.exists(self.ours()))
        # it never connects: back after the connect time
        self.wpa.mode = "never"
        self.assertTrue(self.join(name, "pass word 123")["ok"])
        self.assertEqual((trial()["ssid"], trial()["phase"]), (name, "connecting"))
        self.assertIn("being tried", self.join("Other", "12345678")["error"])
        self.assertIn("being tried", self.ask(cmd="forget", pin=P, ssid="Other")["error"])
        self.assertEqual(self.ask(cmd="confirm"), {"ok": True, "kept": False})                        # not on it: nothing to keep
        self.assertIn("no connection", ended()["why"])
        self.assertEqual((self.wpa.names(), self.wpa.cur), (["Home Net"], "0"))
        # it connects but nobody confirms: back after the trial time
        self.wpa.mode = "ok"
        self.assertTrue(self.join(name, "pass word 123")["ok"])
        time.sleep(0.3)
        self.assertEqual((trial()["phase"], self.wpa.nets[self.wpa.cur]["ssid"]), ("connected", name))
        self.assertFalse(os.path.exists(self.ours()))                                                 # saved only once confirmed
        self.assertIn("nobody confirmed", ended()["why"])
        self.assertEqual((self.wpa.names(), self.wpa.cur), (["Home Net"], "0"))
        # confirmed: it stays; an open network; a second one keeps the first
        self.assertTrue(self.join("Studio", "pass word 123")["ok"])
        time.sleep(0.3)
        self.assertEqual(self.ask(cmd="confirm"), {"ok": True, "kept": True})
        self.assertEqual(self.ask(cmd="status")["last"], {"ssid": "Studio", "ok": True, "why": ""})
        self.assertEqual(stat.S_IMODE(os.stat(self.ours()).st_mode), 0o600)
        time.sleep(1.9)
        self.assertTrue(os.path.exists(self.ours()))
        self.assertEqual(self.wpa.names(), ["Home Net", "Studio"])
        self.assertFalse(any(n["off"] for n in self.wpa.nets.values()))                              # the old one stays a way back
        self.assertTrue(self.join("Cafe Open", "")["ok"])
        time.sleep(0.3)
        self.ask(cmd="confirm")
        self.assertEqual(json.load(open(self.ours()))["network"]["wifis"]["wlan0"]["access-points"],
                         {"Studio": {"password": "pass word 123"}, "Cafe Open": {}})
        self.assertEqual(self.ask(cmd="status")["saved"], ["Cafe Open", "Studio"])
        self.assertTrue([n for n in self.ask(cmd="scan")["networks"] if n["ssid"] == "Cafe Open"][0]["saved"])
        # netplan says no at the confirm: not saved, and the box goes back
        open(os.path.join(self.dir, "netplan-broken"), "w").close()
        before = open(self.ours()).read()
        self.assertTrue(self.join("Third", "12345678")["ok"])
        time.sleep(0.3)
        self.assertIn("netplan refused", self.ask(cmd="confirm")["error"])
        os.unlink(os.path.join(self.dir, "netplan-broken"))
        self.assertEqual(open(self.ours()).read(), before)
        self.assertIsNone(trial())
        self.assertNotIn("Third", self.wpa.names())
        # forget: only what was added here, and out of the running Wi-Fi too
        self.assertIn("came with the install", self.ask(cmd="forget", pin=P, ssid="Home Net")["error"])
        self.assertEqual(self.ask(cmd="forget", pin=P, ssid="Studio"), {"ok": True})
        self.assertEqual(self.ask(cmd="forget", pin=P, ssid="Cafe Open"), {"ok": True})
        self.assertFalse(os.path.exists(self.ours()))
        self.assertEqual(self.wpa.names(), ["Home Net"])
        self.assertEqual(open(os.path.join(self.netplan, "30-t95-wifi.yaml")).read(), "# from the install: never touched\n")
        self.assertNotIn("netplan apply", self.calls())                                               # it takes the Wi-Fi down: never
        # passwords never reach the log or a command line
        self.assertNotIn("pass word 123", open(os.path.join(self.dir, "helper.log")).read())
        self.assertNotIn("pass word 123", "\n".join(self.calls()))
        self.assertNotIn("pass word 123", "\n".join(self.wpa.log))

    def test_4_name_bluetooth_power(self):
        P = "4711"
        for bad in ("Has Caps", "-lead", "a_b", "", "x" * 40, "a;reboot"):
            self.assertIn("lower-case", self.ask(cmd="hostname", pin=P, name=bad)["error"], bad)
        self.assertEqual(self.ask(cmd="hostname", pin=P, name="ghetto-rack"), {"ok": True})
        self.assertIn("hostnamectl set-hostname ghetto-rack", self.calls())
        self.assertIn("attach tool", self.ask(cmd="bt", pin=P, on=True)["error"])                     # not on the box
        open(self.attach, "w").close()
        self.assertIn("true or false", self.ask(cmd="bt", pin=P, on="yes")["error"])
        soft = os.path.join(self.dir, "rfkill", "rfkill1", "soft")
        wlan = os.path.join(self.dir, "rfkill", "rfkill0", "soft")
        # the chip doesn't come up: said so, with what to do
        open(os.path.join(self.dir, "chip-dead"), "w").close()
        self.assertIn("didn't attach", self.ask(cmd="bt", pin=P, on=True)["error"])
        self.assertEqual(open(soft).read(), "1")
        os.unlink(os.path.join(self.dir, "chip-dead"))
        # on: attach (once), lift the block, start the stack
        self.assertEqual(self.ask(cmd="bt", pin=P, on=True), {"ok": True})
        self.assertEqual(self.ask(cmd="status")["bt"], {"tool": True, "attached": True, "on": True, "powered": True})
        self.assertEqual((open(soft).read(), open(wlan).read()), ("0", "0"))
        self.assertIn("systemctl enable ghettorack-bluetooth.service", self.calls())
        self.assertIn("systemctl start --no-block bluetooth.service", self.calls())
        # off: the radio is blocked and it won't start at the next boot, but the attach tool is never stopped
        self.assertEqual(self.ask(cmd="bt", pin=P, on=False), {"ok": True})
        self.assertEqual(self.ask(cmd="status")["bt"], {"tool": True, "attached": True, "on": False, "powered": False})
        self.assertEqual((open(soft).read(), open(wlan).read()), ("1", "0"))                          # the Wi-Fi's switch is not touched
        self.assertIn("systemctl disable ghettorack-bluetooth.service", self.calls())
        n = self.calls().count("systemctl start --no-block ghettorack-bluetooth.service")
        # on again in the same boot: no second attach
        self.assertEqual(self.ask(cmd="bt", pin=P, on=True), {"ok": True})
        self.assertEqual(self.calls().count("systemctl start --no-block ghettorack-bluetooth.service"), n)
        self.assertTrue(self.ask(cmd="status")["bt"]["on"])
        self.assertFalse([c for c in self.calls() if "stop ghettorack-bluetooth" in c or "restart ghettorack-bluetooth" in c
                          or "--now ghettorack-bluetooth" in c])
        self.assertEqual(self.ask(cmd="restart_bridge", pin=P), {"ok": True})
        self.assertEqual(self.ask(cmd="reboot", pin=P), {"ok": True})
        self.assertNotIn("systemctl reboot", self.calls())                                            # after the answer is out
        time.sleep(1.4)
        self.assertIn("systemctl reboot", self.calls())
        self.assertIn("systemctl restart apcbox-bridge.service", self.calls())
        self.assertEqual(self.ask(cmd="poweroff")["error"], "wrong PIN")
        self.assertNotIn("systemctl poweroff", self.calls())

    def test_45_hotspot(self):
        P, w = "4711", self.wpa
        key = "rack rack 99"

        def hot():
            return self.ask(cmd="status")["hotspot"]

        def until(f, t=4):
            end = time.time() + t
            while not f() and time.time() < end:
                time.sleep(0.05)
            self.assertTrue(f())
        name = socket.gethostname()
        self.assertEqual(hot(), {"set": False, "auto": True, "tool": True, "on": False, "by": "", "clients": 0, "ssid": name, "addr": "10.42.0.1"})
        # no password: no hotspot, by hand or by itself (never an open one)
        self.assertIn("PIN", self.ask(cmd="hotspot", on=True)["error"])
        self.assertIn("password first", self.ask(cmd="hotspot", pin=P, on=True)["error"])
        w.air = []
        w.settle()
        time.sleep(1.2)
        self.assertIsNone(w.ap())
        w.air = ["Home Net"]
        w.settle()
        self.assertIn("8 to 63", self.hot_set(P, "short")["error"])
        self.assertEqual(self.hot_set(P, key, auto=False), {"ok": True})
        self.assertEqual(stat.S_IMODE(os.stat(os.path.join(self.dir, "state", "hotspot")).st_mode), 0o600)
        self.assertEqual((hot()["set"], hot()["auto"]), (True, False))
        # by hand: WPA2 only, named like the box, an address, an address server; nobody joins: back to the Wi-Fi
        r = self.ask(cmd="hotspot", pin=P, on=True)
        self.assertEqual((r["ok"], r["ssid"], r["addr"]), (True, name, "10.42.0.1"))
        until(lambda: w.ap() and os.path.exists(os.path.join(self.dir, "hot-addr")) and os.path.exists(os.path.join(self.dir, "dnsmasq.pid")))
        ap = w.ap()
        self.assertEqual((ap["ssid"], ap["key_mgmt"], ap["proto"], ap["pairwise"], ap["frequency"]), (name, "WPA-PSK", "RSN", "CCMP", "2437"))
        self.assertEqual(hot()["by"], "hand")
        d = [c for c in self.calls() if c.startswith("dnsmasq ")][-1]
        self.assertIn("--port=0", d)                                                                  # addresses only
        self.assertIn("--dhcp-range=10.42.0.10,10.42.0.99,1h", d)
        until(lambda: not w.ap() and w.cur == "0")
        self.assertEqual((hot()["on"], w.names(), os.path.exists(os.path.join(self.dir, "hot-addr"))), (False, ["Home Net"], False))
        self.assertIn("nobody joined", self.ask(cmd="status")["last"]["why"])
        # by hand and somebody joins: it stays, also after they leave; off in Settings ends it
        self.ask(cmd="hotspot", pin=P, on=True)
        until(lambda: w.ap())
        w.clients = ["XX:XX:XX:XX:XX:01", "XX:XX:XX:XX:XX:02"]
        until(lambda: hot()["clients"] == 2)
        time.sleep(1.5)
        self.assertTrue(w.ap())
        w.clients = []
        time.sleep(1.5)
        self.assertTrue(w.ap())
        # a join from the hotspot that fails brings the hotspot back; one that works ends it
        w.mode = "wrong"
        self.assertTrue(self.join("Venue", "12345678")["ok"])
        until(lambda: self.ask(cmd="status")["last"] is not None)
        self.assertTrue(w.ap())
        self.assertTrue(hot()["on"])
        w.mode, w.air = "ok", ["Home Net", "Venue"]
        self.assertTrue(self.join("Venue", "12345678")["ok"])
        until(lambda: (self.ask(cmd="status")["trial"] or {}).get("phase") == "connected")
        self.assertEqual(self.ask(cmd="confirm"), {"ok": True, "kept": True})
        self.assertEqual((hot()["on"], w.names(), os.path.exists(os.path.join(self.dir, "hot-addr"))), (False, ["Home Net", "Venue"], False))
        self.assertEqual(self.ask(cmd="forget", pin=P, ssid="Venue"), {"ok": True})
        self.assertEqual(self.ask(cmd="hotspot", pin=P, on=True)["ok"], True)
        until(lambda: w.ap())
        self.assertEqual(self.ask(cmd="hotspot", pin=P, on=False), {"ok": True})
        until(lambda: not w.ap() and not hot()["on"])
        # by itself: no known Wi-Fi for a while brings it up; a known one back in the air takes it down
        self.assertEqual(self.ask(cmd="hotspot_set", pin=P, auto=True), {"ok": True})
        time.sleep(1.0)
        self.assertIsNone(w.ap())                                                                     # there is Wi-Fi: no hotspot
        w.air = []
        w.settle()
        until(lambda: w.ap())
        self.assertEqual(hot()["by"], "itself")
        w.clients = ["XX:XX:XX:XX:XX:01"]
        w.air = ["Home Net"]
        time.sleep(1.5)
        self.assertTrue(w.ap())                                                                       # somebody is on it: it stays
        w.clients = []
        until(lambda: not w.ap() and w.cur is not None)
        self.assertFalse(hot()["on"])
        # a driver that can't scan as an access point: the hotspot steps aside, finds no Wi-Fi, returns
        w.ap_scan, w.air = False, []
        w.settle()
        until(lambda: w.ap())
        until(lambda: not w.ap())
        until(lambda: w.ap())
        w.air = ["Home Net"]
        until(lambda: not w.ap() and w.cur is not None)
        w.ap_scan = True
        self.assertEqual(self.ask(cmd="hotspot_set", pin=P, auto=False), {"ok": True})
        w.air = None
        # the password never reaches the log, a command line or the Wi-Fi's control socket
        for text in (open(os.path.join(self.dir, "helper.log")).read(), "\n".join(self.calls()), "\n".join(w.log)):
            self.assertNotIn(key, text)

    def test_5_garbage_on_the_socket(self):
        s = socket.socket(socket.AF_UNIX)
        s.connect(self.sock)
        s.sendall(b"not json\n")
        self.assertEqual(json.loads(s.recv(4096).decode()), {"error": "JSON expected"})
        s.close()
        s = socket.socket(socket.AF_UNIX)
        s.connect(self.sock)
        s.sendall(b"[1, 2]\n")
        self.assertEqual(json.loads(s.recv(4096).decode()), {"error": "unknown command"})
        s.close()
        self.assertEqual(self.ask(cmd="status")["ok"], True)                                          # still up
        self.assertIsNone(self.proc.poll())


if __name__ == "__main__":
    unittest.main(verbosity=1)
