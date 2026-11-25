#!/usr/bin/env python3
"""The ITCM copy, checked on the desk: does what ItcmLoad (main.cpp) will copy
out of the flash image equal what the linker expects at those addresses?
A build that fails this does not boot.   python3 daisy/check_itcm.py
(run after `make -C daisy`; needs arm-none-eabi-nm and -objcopy)"""
import os, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
elf, binf = os.path.join(here, "build/ApcBox.elf"), os.path.join(here, "build/ApcBox.bin")
nm = subprocess.run(["arm-none-eabi-nm", elf], capture_output=True, text=True, check=True).stdout
sym = {p[2]: int(p[0], 16) for p in (l.split() for l in nm.splitlines()) if len(p) == 3}
s, e, si = sym["_sitcm"], sym["_eitcm"], sym["_siitcm"]
with tempfile.TemporaryDirectory() as d:
    out = os.path.join(d, "itcm.bin")
    subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".itcm", elf, out], check=True)
    want = open(out, "rb").read()          # the section by address, from its start
flash = open(binf, "rb").read()
src = si - 0x90040000                      # the app's image starts at 0x90040000
ok = e > s and flash[src:src + (e - s)] == want[s:e]
bad = [n for n in ("memset", "memcpy") if n in sym and sym[n] < 0x10000]   # the reset code calls these first
print("ITCM copy: %d bytes, %s%s" % (e - s, "ok" if ok else "WRONG: this build will not boot",
                                     "; in ITCM but needed before the copy: " + ", ".join(bad) if bad else ""))
sys.exit(0 if ok and not bad else 1)
