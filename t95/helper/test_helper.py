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
  "netplan generate") [ -f "$FAKEDIR/netplan-broken" ] && { echo "bad yaml"; exit 1; } ;;
  "systemctl is-active ghettorack-bluetooth.service") [ -f "$FAKEDIR/bt-on" ] && echo active || echo inactive ;;
  "systemctl enable --now ghettorack-bluetooth.service") touch "$FAKEDIR/bt-on" ;;
  "systemctl disable --now ghettorack-bluetooth.service") rm -f "$FAKEDIR/bt-on" ;;
  "bluetoothctl show") printf 'Controller XX:XX:XX:XX:XX:XX (public)\\n\\tPowered: yes\\n' ;;
esac
exit 0
"""


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
        for t in ("wpa_cli", "netplan", "hostnamectl", "systemctl", "bluetoothctl"):
            p = os.path.join(cls.bin, t)
            open(p, "w").write(FAKE)
            os.chmod(p, 0o755)
            tools[t] = p
        cls.log = os.path.join(cls.dir, "calls.log")
        cls.sock = os.path.join(cls.dir, "h.sock")
        cls.attach = os.path.join(cls.dir, "hciattach_opi")
        env = dict(os.environ, FAKELOG=cls.log, FAKEDIR=cls.dir)
        cls.proc = subprocess.Popen(
            [sys.executable, os.path.join(HERE, "ghettorack_helper.py"), "--socket", cls.sock, "--state-dir", os.path.join(cls.dir, "state"),
             "--netplan-dir", cls.netplan, "--trial-s", "1.5", "--scan-s", "0.05", "--lock-s", "1.2", "--attach", cls.attach,
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

    def calls(self):
        return open(self.log).read().splitlines() if os.path.exists(self.log) else []

    def ours(self):
        return os.path.join(self.netplan, "40-ghettorack-wifi.yaml")

    def test_1_status_scan_need_no_pin(self):
        st = self.ask(cmd="status")
        self.assertEqual(st["wifi"], {"ssid": "Home Net", "state": "COMPLETED", "ip": "198.51.100.7", "iface": "wlan0"})
        self.assertEqual((st["saved"], st["trial_s"], st["pin_set"]), ([], 0, False))
        self.assertEqual(st["bt"], {"tool": False, "on": False, "powered": False, "controller": False})
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
        # nobody confirms: it goes back
        r = self.join('Studio "5G": x', "pass word 123")
        self.assertEqual(r, {"ok": True, "trial_s": 1.5})
        self.assertEqual(stat.S_IMODE(os.stat(self.ours()).st_mode), 0o600)
        doc = json.load(open(self.ours()))
        self.assertEqual(doc["network"]["wifis"]["wlan0"]["access-points"], {'Studio "5G": x': {"password": "pass word 123"}})
        st = self.ask(cmd="status")
        self.assertTrue(0 < st["trial_s"] <= 2 and st["trying"] == 'Studio "5G": x')
        self.assertIn("being tried", self.join("Other", "12345678")["error"])
        time.sleep(2.0)
        self.assertFalse(os.path.exists(self.ours()))
        self.assertEqual(self.ask(cmd="status")["trial_s"], 0)
        self.assertGreaterEqual(self.calls().count("netplan apply"), 2)                               # there and back
        # confirmed: it stays; an open network; a second one keeps the first
        self.assertTrue(self.join("Studio", "pass word 123")["ok"])
        self.assertEqual(self.ask(cmd="confirm"), {"ok": True, "kept": True})
        time.sleep(1.9)
        self.assertTrue(os.path.exists(self.ours()))
        self.assertTrue(self.join("Cafe Open", "")["ok"])
        self.ask(cmd="confirm")
        self.assertEqual(json.load(open(self.ours()))["network"]["wifis"]["wlan0"]["access-points"],
                         {"Studio": {"password": "pass word 123"}, "Cafe Open": {}})
        self.assertEqual(self.ask(cmd="status")["saved"], ["Cafe Open", "Studio"])
        self.assertTrue([n for n in self.ask(cmd="scan")["networks"] if n["ssid"] == "Cafe Open"][0]["saved"])
        # netplan says no: nothing changes
        open(os.path.join(self.dir, "netplan-broken"), "w").close()
        before = open(self.ours()).read()
        self.assertIn("netplan refused", self.join("Third", "12345678")["error"])
        os.unlink(os.path.join(self.dir, "netplan-broken"))
        self.assertEqual(open(self.ours()).read(), before)
        self.assertEqual(self.ask(cmd="status")["trial_s"], 0)
        # forget: only what was added here
        self.assertIn("can't be removed", self.ask(cmd="forget", pin=P, ssid="Home Net")["error"])
        self.assertEqual(self.ask(cmd="forget", pin=P, ssid="Studio"), {"ok": True})
        self.assertEqual(self.ask(cmd="forget", pin=P, ssid="Cafe Open"), {"ok": True})
        self.assertFalse(os.path.exists(self.ours()))
        self.assertEqual(open(os.path.join(self.netplan, "30-t95-wifi.yaml")).read(), "# from the install: never touched\n")
        # passwords never reach the log or a command line
        self.assertNotIn("pass word 123", open(os.path.join(self.dir, "helper.log")).read())
        self.assertNotIn("pass word 123", "\n".join(self.calls()))

    def test_4_name_bluetooth_power(self):
        P = "4711"
        for bad in ("Has Caps", "-lead", "a_b", "", "x" * 40, "a;reboot"):
            self.assertIn("lower-case", self.ask(cmd="hostname", pin=P, name=bad)["error"], bad)
        self.assertEqual(self.ask(cmd="hostname", pin=P, name="ghetto-rack"), {"ok": True})
        self.assertIn("hostnamectl set-hostname ghetto-rack", self.calls())
        self.assertIn("attach tool", self.ask(cmd="bt", pin=P, on=True)["error"])                     # not on the box
        open(self.attach, "w").close()
        self.assertIn("true or false", self.ask(cmd="bt", pin=P, on="yes")["error"])
        self.assertEqual(self.ask(cmd="bt", pin=P, on=True), {"ok": True})
        self.assertEqual(self.ask(cmd="status")["bt"], {"tool": True, "on": True, "powered": True, "controller": True})
        self.assertEqual(self.ask(cmd="bt", pin=P, on=False), {"ok": True})
        self.assertFalse(self.ask(cmd="status")["bt"]["on"])
        self.assertEqual(self.ask(cmd="restart_bridge", pin=P), {"ok": True})
        self.assertEqual(self.ask(cmd="reboot", pin=P), {"ok": True})
        self.assertNotIn("systemctl reboot", self.calls())                                            # after the answer is out
        time.sleep(1.4)
        self.assertIn("systemctl reboot", self.calls())
        self.assertIn("systemctl restart apcbox-bridge.service", self.calls())
        self.assertEqual(self.ask(cmd="poweroff")["error"], "wrong PIN")
        self.assertNotIn("systemctl poweroff", self.calls())

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
