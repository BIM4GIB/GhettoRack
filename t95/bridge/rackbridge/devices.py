"""What Settings > Devices lists: the sound cards and the SD card in the T95's slot.

Read-only: `/proc/asound`, the mount table, `/sys/block`. Nothing here changes the
box. The Daisy's own card is not in it: the firmware doesn't report it.
"""
import glob
import re

from .common import *
from .util import *

SD_COUNT_MAX = 20000        # files looked at when counting the samples on a card
SD_COUNT_S = 30.0           # how long that count is good for
DEV_POLL_S = 3.0


def sound_cards(root):
    """[{num, id, name, play, rec, midi}] from <root>/cards and <root>/card<N>/"""
    out = []
    try:
        with open(os.path.join(root, "cards")) as f:
            text = f.read()
    except OSError:
        return out
    for m in re.finditer(r"^\s*(\d+) \[(.+?)\s*\]: \S+ - (.*)$", text, re.M):
        num = int(m.group(1))
        try:
            ents = os.listdir(os.path.join(root, "card%d" % num))
        except OSError:
            ents = []
        out.append({"num": num, "id": m.group(2), "name": m.group(3).strip(),
                    "play": any(re.fullmatch(r"pcm\d+p", e) for e in ents),
                    "rec": any(re.fullmatch(r"pcm\d+c", e) for e in ents),
                    "midi": any(re.fullmatch(r"midi\d+", e) for e in ents)})
    return out


def mount_of(path, mounts="/proc/mounts"):
    """(device, filesystem, read-only) of what is mounted at path, or None"""
    found = None
    try:
        with open(mounts) as f:
            for line in f:
                p = line.split()
                if len(p) >= 4 and p[1].replace("\\040", " ") == path:
                    found = (p[0], p[2], "ro" in p[3].split(","))     # the last one is the one on top
    except OSError:
        pass
    return found


def system_disks(mounts="/proc/mounts"):
    """the card or chip the box itself runs from (a Raspberry Pi boots from its SD slot)"""
    out = set()
    try:
        with open(mounts) as f:
            for line in f:
                p = line.split()
                m = re.match(r"^/dev/(mmcblk\d+)", p[0]) if len(p) >= 2 and p[1] in ("/", "/boot", "/boot/firmware") else None
                if m:
                    out.add(m.group(1))
    except OSError:
        pass
    return out


def sd_in_slot(sysblock, mounts="/proc/mounts"):
    """True if a card sits in an SD slot, mounted or not. Not the one the box runs from:
    that is no card for samples, and "not readable" would be wrong for it"""
    own = system_disks(mounts)
    for p in glob.glob(os.path.join(sysblock, "mmcblk*", "device", "type")):
        if os.path.basename(os.path.dirname(os.path.dirname(p))) in own:
            continue
        try:
            with open(p) as f:
                if f.read().strip() == "SD":
                    return True
        except OSError:
            pass
    return False


def count_samples(root, limit=SD_COUNT_MAX):
    """(audio files, True if the count stopped at the limit)"""
    n = seen = 0
    for _, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        for f in files:
            seen += 1
            if not f.startswith(".") and f.lower().endswith(AUDIO_EXT):
                n += 1
        if seen >= limit:
            return n, True
    return n, False


class Devices:
    def __init__(self, bridge):
        self.b = bridge
        self.cards = []
        self.sd = {"state": "none"}
        self.counted = None          # (device, when, samples, more)

    def read(self):
        """Runs in a thread: a card full of files takes a while to walk"""
        a = self.b.args
        cards = sound_cards(a.asound)
        path = self.b.sdcard
        if not self.b.sd_ready():
            sd = {"state": "unreadable" if sd_in_slot(a.sysblock) else "none"}
        else:
            dev, fs, ro = mount_of(path) or ("", "", False)
            sd = {"state": "ready", "path": path, "fs": fs, "read_only": ro}
            try:
                v = os.statvfs(path)
                sd["bytes"], sd["free"] = v.f_frsize * v.f_blocks, v.f_frsize * v.f_bavail
            except OSError:
                pass
            c = self.counted
            if c is None or c[0] != dev or now() - c[1] > SD_COUNT_S:
                try:
                    n, more = count_samples(path)
                except OSError:
                    n, more = None, False
                c = self.counted = (dev, now(), n, more)
            sd["samples"], sd["samples_more"] = c[2], c[3]
        return cards, sd

    def info(self):
        a, hd = self.b.args, self.b.hdmi
        cards = []
        for c in self.cards:
            use = ("daisy" if c["id"] == a.daisy_card else "tv" if c["id"] == hd.card()
                   else "midi" if c["midi"] and not (c["play"] or c["rec"]) else "")
            cards.append(dict(c, use=use, busy=hd.running() and use in ("daisy", "tv")))
        return {"cards": cards, "sd": self.sd}

    async def task(self):
        loop = asyncio.get_running_loop()
        while True:
            try:
                self.cards, self.sd = await loop.run_in_executor(None, self.read)
            except Exception as e:      # a listing is never worth the bridge
                log.warning("devices: %s", e)
            await asyncio.sleep(DEV_POLL_S)
