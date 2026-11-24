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
  "bluetoothctl devices"|"bluetoothctl devices Paired") [ -f "$FAKEDIR/btdev" ] && awk -F'|' -v w="$2" '(w == "" || $4 == "yes") { print "Device " $1 " " $2 }' "$FAKEDIR/btdev" ;;
  "bluetoothctl info "*) [ -f "$FAKEDIR/btdev" ] || { echo "Device $2 not available"; exit 1; }
     awk -F'|' -v a="$2" '$1 == a { f = 1; print "Device " a " (random)"; if ($2 != "") print "\\tName: " $2; print "\\tPaired: " $4; print "\\tConnected: " $5
       if ($3 == "yes") print "\\tUUID: Vendor specific           (03b80e5a-ede8-4b33-a751-6ce34ec4c700)"; print "\\tRSSI: 0xffffffc4 (" $6 ")" }
       END { if (!f) print "Device " a " not available" }' "$FAKEDIR/btdev" ;;
  "bluetoothctl ") want=""                      # a session: commands on its input, as the helper pairs
     set_field() { awk -F'|' -v OFS='|' -v a="$1" -v n="$2" -v v="$3" '$1 == a { $n = v } { print }' "$FAKEDIR/btdev" > "$FAKEDIR/btdev.new" && mv "$FAKEDIR/btdev.new" "$FAKEDIR/btdev"; }
     while read c a; do echo "bluetoothctl> $c $a" >> "$FAKELOG"; case "$c" in
       connect) [ -f "$FAKEDIR/bt-away" ] || set_field "$a" 5 yes ;;
       pair) if [ -f "$FAKEDIR/bt-refuse" ]; then echo "Failed to pair: org.bluez.Error.AuthenticationFailed"; else want="$a"; echo "[agent] Confirm passkey 123456 (yes/no):"; fi ;;
       yes) [ -n "$want" ] && { set_field "$want" 4 yes; [ -f "$FAKEDIR/bt-newaddr" ] && set_field "$want" 1 "$(cat "$FAKEDIR/bt-newaddr")"; echo "Pairing successful"; want=""; } ;;
       quit) exit 0 ;;
     esac; done ;;
  "bluetoothctl remove "*) awk -F'|' -v a="$2" '$1 != a' "$FAKEDIR/btdev" > "$FAKEDIR/btdev.new" && mv "$FAKEDIR/btdev.new" "$FAKEDIR/btdev" ;;
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
             "--rfkill-dir", os.path.join(cls.dir, "rfkill"), "--bt-class-dir", os.path.join(cls.dir, "btclass"), "--bt-wait-s", "1", "--bt-scan-s", "0.1", "--bt-find-s", "1", "--bt-connect-s", "1", "--bt-pair-s", "3", "--bt-step-s", "0.1",
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

    def test_4b_bluetooth_devices(self):
        P = "4711"
        self.assertEqual(self.ask(cmd="bt", pin=P, on=True), {"ok": True})
        ad = [":".join(["%02X" % (0xC0 + i)] * 6) for i in range(4)]       # made here: no address-shaped text in the repo
        dev = os.path.join(self.dir, "btdev")
        with open(dev, "w") as f:
            f.write("%s|TV in the next room|no|no|no|-80\n%s||no|no|no|-50\n%s|WIDI Jack|yes|no|no|-61\n%s|Old Keys|yes|yes|no|-90\n" % tuple(ad))
        self.addCleanup(lambda: os.path.exists(dev) and os.unlink(dev))
        out = self.ask(cmd="bt_scan")                                       # no PIN to look; MIDI first, the nameless one left out
        self.assertEqual([(x["name"], x["midi"], x["paired"], x["rssi"]) for x in out["devices"]],
                         [("Old Keys", True, True, -90), ("WIDI Jack", True, False, -61), ("TV in the next room", False, False, -80)])
        time.sleep(4.1)
        self.assertEqual([x["name"] for x in self.ask(cmd="status")["bt_devices"]], ["Old Keys"])
        self.assertTrue(self.ask(cmd="bt_pair", addr=ad[2]).get("pin"))     # changing the box needs the PIN
        self.assertIn("addr", self.ask(cmd="bt_pair", pin=P, addr="WIDI Jack; reboot")["error"])
        self.assertIn("in reach", self.ask(cmd="bt_pair", pin=P, addr=":".join(["EE"] * 6))["error"])
        open(os.path.join(self.dir, "bt-away"), "w").close()                # seen in the scan, doesn't connect
        self.assertEqual(self.ask(cmd="bt_pair", pin=P, addr=ad[2])["error"], "it didn't pair: no answer from it")
        os.unlink(os.path.join(self.dir, "bt-away"))
        open(os.path.join(self.dir, "bt-refuse"), "w").close()
        self.assertIn("it didn't pair: it refused", self.ask(cmd="bt_pair", pin=P, addr=ad[2])["error"])
        os.unlink(os.path.join(self.dir, "bt-refuse"))
        # the order that works on the T95: connect under a scan, scan off, pair, yes to the code
        real = ":".join(["%02X" % 0xA5] * 6)                                # a phone or tablet shows its real address once paired
        with open(os.path.join(self.dir, "bt-newaddr"), "w") as f:
            f.write(real)
        n = len(self.calls())
        out = self.ask(cmd="bt_pair", pin=P, addr=ad[2])
        self.assertEqual((out["ok"], out["device"]["addr"], out["device"]["name"], out["device"]["paired"], out["device"]["connected"]),
                         (True, real, "WIDI Jack", True, True))
        said = [c.split("> ", 1)[1].strip() for c in self.calls()[n:] if c.startswith("bluetoothctl> ")]
        said = [c for i, c in enumerate(said) if c != "yes" or said[i - 1] != "yes"]
        self.assertEqual(said, ["scan on", "connect " + ad[2], "scan off", "pair " + ad[2], "yes", "trust " + real, "quit"])
        ad[2] = real
        self.assertEqual(sorted(x["name"] for x in self.ask(cmd="status")["bt_devices"]), ["Old Keys", "WIDI Jack"])
        self.assertTrue(self.ask(cmd="bt_forget", addr=ad[3]).get("pin"))
        self.assertEqual(self.ask(cmd="bt_forget", pin=P, addr=ad[3]), {"ok": True})
        self.assertEqual([x["name"] for x in self.ask(cmd="status")["bt_devices"]], ["WIDI Jack"])
        self.ask(cmd="bt", pin=P, on=False)
        self.assertEqual(self.ask(cmd="bt_scan")["error"], "Bluetooth is off")
        self.assertEqual(self.ask(cmd="status")["bt_devices"], [])

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
        self.assertEqual(hot(), {"set": False, "auto": False, "tool": True, "on": False, "by": "", "clients": 0, "ssid": name, "addr": "10.42.0.1"})
        # no password: no hotspot, by hand or by itself (never an open one)
        self.assertEqual(self.ask(cmd="hotspot_set", pin=P, auto=True), {"ok": True})
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


CARD_FAKE = """#!/bin/sh
# mount / systemctl for the card tests: the "mounts" file is what the helper reads
echo "$(basename "$0") $*" >> "$FAKELOG"
M="$FAKEDIR/mounts"
case "$(basename "$0") $*" in
  "mount -o remount,rw "*) [ -f "$FAKEDIR/rw-fails" ] && exit 32; sed -i.bak 's/ ro,/ rw,/' "$M" ;;
  "mount -o remount,ro "*) sed -i.bak 's/ rw,/ ro,/' "$M" ;;
  "systemctl stop media-sdcard.mount") [ -f "$FAKEDIR/busy" ] && { echo "umount: /media/sdcard: target is busy." >&2; exit 1; }; : > "$M" ;;
esac
exit 0
"""


def wav(data, says=None):
    import struct
    n = len(data) if says is None else says
    return (b"RIFF" + struct.pack("<I", 36 + n) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, 48000, 192000, 4, 16)
            + b"data" + struct.pack("<I", n) + data)


class CardTest(unittest.TestCase):
    """Takes copied to the SD card and eject: a folder as the card, made-up mount / umount"""

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.card = os.path.join(cls.dir, "card")
        cls.jams = os.path.join(cls.dir, "jams")
        cls.mounts = os.path.join(cls.dir, "mounts")
        cls.log = os.path.join(cls.dir, "calls.log")
        cls.sock = os.path.join(cls.dir, "h.sock")
        os.makedirs(cls.card)
        os.makedirs(os.path.join(cls.dir, "sysblock", "mmcblk0", "device"))
        os.makedirs(os.path.join(cls.dir, "sysblock", "mmcblk2", "device"))
        open(os.path.join(cls.dir, "sysblock", "mmcblk0", "device", "type"), "w").write("SD\n")
        open(os.path.join(cls.dir, "sysblock", "mmcblk2", "device", "type"), "w").write("MMC\n")      # the eMMC
        tools = {}
        for t in ("mount", "systemctl"):
            tools[t] = os.path.join(cls.dir, t)
            open(tools[t], "w").write(CARD_FAKE)
            os.chmod(tools[t], 0o755)
        cls.a = os.urandom(40000)                 # a take in two parts; the second one's header is wrong (cut off)
        cls.b = os.urandom(20000)
        d = os.path.join(cls.jams, "Friday jam 1")
        os.makedirs(d)
        open(os.path.join(d, "part.wav"), "wb").write(wav(cls.a))
        open(os.path.join(d, "part-01.wav"), "wb").write(wav(cls.b + b"\x07\x07", says=0x7fffff00))
        open(os.path.join(d, "arecord.log"), "w").write("not a part\n")
        cls.proc = subprocess.Popen(
            [sys.executable, os.path.join(HERE, "ghettorack_helper.py"), "--socket", cls.sock, "--state-dir", os.path.join(cls.dir, "state"),
             "--netplan-dir", cls.dir, "--group", "no-such-group", "--wpa-ctrl", os.path.join(cls.dir, "wpa"), "--auto-s", "9999",
             "--card-mount", cls.card, "--jams-dir", cls.jams, "--mounts", cls.mounts, "--sysblock", os.path.join(cls.dir, "sysblock"),
             "--mount", tools["mount"], "--systemctl", tools["systemctl"], "--wpa-cli", "/bin/false", "--bluetoothctl", "/bin/false",
             "--ip", "/bin/false"],
            env=dict(os.environ, FAKELOG=cls.log, FAKEDIR=cls.dir), stdout=subprocess.DEVNULL,
            stderr=open(os.path.join(cls.dir, "helper.log"), "w"))
        for _ in range(100):
            if os.path.exists(cls.sock):
                break
            time.sleep(0.05)

    @classmethod
    def tearDownClass(cls):
        cls.proc.kill()

    ask = HelperTest.ask

    def mount(self, dev="/dev/mmcblk0p1", fs="exfat", how="ro"):
        open(self.mounts, "w").write("/dev/root / ext4 rw,relatime 0 0\n%s %s %s %s,nosuid,nodev 0 0\n" % (dev, self.card, fs, how))

    def setUp(self):
        self.mount()
        for f in ("busy", "rw-fails"):
            try:
                os.remove(os.path.join(self.dir, f))
            except OSError:
                pass
        open(self.log, "w").close()

    def calls(self):
        return open(self.log).read().split("\n")[:-1]

    def copied(self, timeout=5):
        end = time.time() + timeout
        while time.time() < end:
            c = self.ask(cmd="card_status")["copy"]
            if c and c["state"] != "copying":
                return c
            time.sleep(0.05)
        self.fail("the copy never ended")

    def test_k1_status(self):
        s = self.ask(cmd="card_status")
        self.assertEqual((s["card"]["mounted"], s["card"]["ok"], s["card"]["fs"], s["card"]["read_only"]), (True, True, "exfat", True))
        self.assertGreater(s["card"]["free"], 0)
        self.assertIsNone(s["copy"])
        open(self.mounts, "w").write("/dev/root / ext4 rw 0 0\n")
        s = self.ask(cmd="card_status")
        self.assertEqual((s["card"]["mounted"], s["card"]["ok"]), (False, False))
        self.assertIn("no SD card", self.ask(cmd="card_copy", take="Friday jam 1")["error"])

    def test_k2_a_take_goes_onto_the_card_as_one_wav(self):
        r = self.ask(cmd="card_copy", take="Friday jam 1")
        self.assertTrue(r.get("ok"), r)
        self.assertEqual(r["copy"]["total"], 44 + 60000)
        c = self.copied()
        self.assertEqual((c["state"], c["bytes"], c["error"], c["file"]), ("done", 44 + 60000, None, "jams/Friday jam 1.wav"))
        got = open(os.path.join(self.card, "jams", "Friday jam 1.wav"), "rb").read()
        self.assertEqual(got, wav(self.a + self.b))                 # both parts' sound under one true header; the torn frame left out
        self.assertEqual(os.listdir(os.path.join(self.card, "jams")), ["Friday jam 1.wav"])    # no half file left
        # writable for the copy only
        self.assertEqual(self.calls(), ["mount -o remount,rw " + self.card, "mount -o remount,ro " + self.card])
        self.assertTrue(self.ask(cmd="card_status")["card"]["read_only"])
        # again: said, and the card is not touched
        open(self.log, "w").close()
        self.assertIn("already on the card", self.ask(cmd="card_copy", take="Friday jam 1")["error"])
        open(os.path.join(self.card, "jams", "Friday jam 1.wav"), "ab").write(b"x")
        self.assertIn("another file of that name", self.ask(cmd="card_copy", take="Friday jam 1")["error"])
        self.assertEqual(self.calls(), [])
        os.remove(os.path.join(self.card, "jams", "Friday jam 1.wav"))

    def test_k3_only_takes_only_onto_a_card(self):
        for bad in ("", "..", "../jams/Friday jam 1", "Friday jam 1/..", "/etc", "nope", ".hidden", "x" * 49, 7, None):
            self.assertEqual(self.ask(cmd="card_copy", take=bad).get("error"), "no such take", bad)
        # the bridge has no rights and writes the takes folder: nothing is followed out of it
        secret = os.path.join(self.dir, "secret")
        open(secret, "wb").write(wav(b"\x01\x02\x03\x04" * 100))
        d = os.path.join(self.jams, "linked part")
        os.makedirs(d)
        os.symlink(secret, os.path.join(d, "part.wav"))
        self.assertEqual(self.ask(cmd="card_copy", take="linked part").get("error"), "no such take")
        os.symlink(os.path.join(self.jams, "Friday jam 1"), os.path.join(self.jams, "linked take"))
        self.assertEqual(self.ask(cmd="card_copy", take="linked take").get("error"), "no such take")
        os.makedirs(os.path.join(self.jams, "empty"))
        self.assertIn("nothing in that take", self.ask(cmd="card_copy", take="empty")["error"])
        # not the eMMC, not a Linux card (the recovery card), not some other disk
        for dev, fs in (("/dev/mmcblk2p1", "exfat"), ("/dev/mmcblk0p1", "ext4"), ("/dev/sda1", "vfat"), ("/dev/mmcblk0p1/../sda1", "vfat")):
            self.mount(dev, fs)
            r = self.ask(cmd="card_copy", take="Friday jam 1")
            self.assertIn("isn't a FAT or exFAT card in the SD slot", r.get("error", ""), (dev, fs))
            self.assertFalse(self.ask(cmd="card_status")["card"]["ok"])
        self.assertEqual(self.calls(), [])
        j = os.path.join(self.card, "jams")
        self.assertEqual(os.listdir(j) if os.path.isdir(j) else [], [])       # nothing reached the card

    def test_k4_a_card_that_stays_read_only(self):
        open(os.path.join(self.dir, "rw-fails"), "w").close()
        self.assertTrue(self.ask(cmd="card_copy", take="Friday jam 1").get("ok"))
        c = self.copied()
        self.assertEqual(c["state"], "failed")
        self.assertIn("can't be made writable", c["error"])
        self.assertFalse(os.path.exists(os.path.join(self.card, "jams", "Friday jam 1.wav")))
        self.assertTrue(self.ask(cmd="card_status")["card"]["read_only"])

    def test_k5_eject(self):
        open(os.path.join(self.dir, "busy"), "w").close()
        self.assertIn("in use", self.ask(cmd="card_eject")["error"])
        self.assertTrue(self.ask(cmd="card_status")["card"]["mounted"])
        os.remove(os.path.join(self.dir, "busy"))
        self.assertEqual(self.ask(cmd="card_eject"), {"ok": True})
        self.assertEqual(self.calls()[-1], "systemctl stop media-sdcard.mount")      # systemd's unmount: for the whole box
        self.assertFalse(self.ask(cmd="card_status")["card"]["mounted"])
        self.assertEqual(self.ask(cmd="card_eject").get("was"), "not mounted")     # twice is fine


if __name__ == "__main__":
    unittest.main(verbosity=1)
