#!/usr/bin/env python3
"""Fill the ApcBox microSD card from your Ableton library (or any folder of
samples).

    python3 sdprep.py                      # dry run: shows what it would do
    python3 sdprep.py --go                 # converts and copies to /Volumes/APCBOX
    python3 sdprep.py --go --src ~/Samples --src "/Volumes/Big/Splice"

What ends up on the card:
    kits/<name>/1 kick.wav ... 8 perc.wav    drum kits for the SCENE buttons
    samples/<folder>/*.wav                   the browser (SYSTEM view, DEVICE <>)

Everything is converted to 48 kHz 16-bit mono WAV (the Daisy's format, so
it loads fast). Uses macOS's own afconvert/afinfo; ffmpeg works too.

Limits it plans around (they're the firmware's):
    59 SD kits, 47 sample folders, 400 samples per folder, 32 s per sample.
"""
import argparse, os, re, shutil, subprocess, sys, concurrent.futures as cf
from collections import defaultdict

HOME = os.path.expanduser("~")
DEFAULT_SOURCES = [
    f"{HOME}/Music/Ableton/Factory Packs",
    f"{HOME}/Music/Ableton/User Library/Samples",
    # Core Library inside the app (Live 10 / 11 / 12, Suite / Standard / Intro)
    *[f"/Applications/{a}/Contents/App-Resources/Core Library"
      for a in (os.listdir("/Applications") if os.path.isdir("/Applications") else [])
      if a.startswith("Ableton Live")],
]
AUDIO_EXT = (".wav", ".aif", ".aiff", ".flac", ".caf")
MAX_KITS, MAX_FOLDERS, MAX_FILES, MAX_SECONDS = 59, 47, 400, 32.0
ONESHOT_SECONDS = 3.0

ROLES = ["kick", "snare", "closed hat", "open hat", "clap", "tom", "rim", "perc"]
# word patterns, checked on the file name first, then the folder path
ROLE_RX = [
    ("open hat", r"open\s*hat|open\s*hh|\bohh?\b|hat\s*open|hh\s*open|\bopen\b.*\bhat"),
    ("closed hat", r"closed\s*hat|\bchh?\b|hi-?hat|\bhh\b|\bhat\b|hihat"),
    ("kick", r"\bkick|\bbd\b|\bkik\b|bassdrum|bass drum"),
    ("snare", r"snare|\bsd\b|\bsnr\b"),
    ("clap", r"\bclap|\bcp\b|\bclp\b"),
    ("rim", r"\brim|\brs\b|sidestick|side stick|\bstick"),
    ("tom", r"\btom|\btm\b|floor tom"),
    ("perc", r"perc|cowbell|conga|bongo|shaker|tamb|clave|block|cymbal|ride|crash|\bcb\b|agogo|guiro|triangle"),
]
OTHER_RX = [  # browser folders for things that aren't drum hits
    ("bass", r"\bbass\b|\bsub\b|808 bass|reese"),
    ("keys", r"piano|keys|rhodes|wurli|organ|\bep\b"),
    ("synth", r"synth|lead|pad|pluck|chord|stab|arp"),
    ("vocals", r"vox|vocal|voice|choir|chant"),
    ("fx", r"\bfx\b|sweep|riser|impact|noise|foley|texture|atmos|ambien|whoosh|hit"),
]


def role_of(path):
    name = os.path.basename(path).lower()
    folder = os.path.dirname(path).lower()
    for text in (name, folder):
        for role, rx in ROLE_RX:
            if re.search(rx, text):
                return role
    return None


def other_of(path):
    text = path.lower()
    for kind, rx in OTHER_RX:
        if re.search(rx, text):
            return kind
    return "other"


def tool(*names):
    for n in names:
        if shutil.which(n):
            return n
    return None


AFINFO, AFCONVERT = tool("afinfo"), tool("afconvert")
FFPROBE, FFMPEG = tool("ffprobe"), tool("ffmpeg")


def duration(path):
    try:
        if AFINFO:
            out = subprocess.run([AFINFO, path], capture_output=True, text=True, timeout=20).stdout
            m = re.search(r"estimated duration:\s*([\d.]+)", out)
            return float(m.group(1)) if m else None
        if FFPROBE:
            out = subprocess.run([FFPROBE, "-v", "error", "-show_entries", "format=duration",
                                  "-of", "csv=p=0", path], capture_output=True, text=True,
                                 timeout=20).stdout
            return float(out.strip())
    except (ValueError, subprocess.SubprocessError, OSError):
        pass
    return None


def convert(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if AFCONVERT:
        cmd = [AFCONVERT, "-f", "WAVE", "-d", "LEI16@48000", "-c", "1", src, dst]
    else:
        cmd = [FFMPEG, "-v", "error", "-y", "-i", src, "-ac", "1", "-ar", "48000",
               "-c:a", "pcm_s16le", dst]
    r = subprocess.run(cmd, capture_output=True, timeout=120)
    return r.returncode == 0 and os.path.exists(dst)


def clean(name, n=40):
    base = os.path.splitext(name)[0]
    base = re.sub(r"[^A-Za-z0-9 _#+.-]", "", base).strip(" .")
    return (base or "sample")[:n]


def pack_of(path, roots):
    for r in roots:
        if path.startswith(r + os.sep):
            rest = path[len(r) + 1:].split(os.sep)
            return rest[0] if len(rest) > 1 else os.path.basename(r)
    return "misc"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", action="append", help="sample folder (repeatable); default: Ableton's")
    ap.add_argument("--card", default="/Volumes/APCBOX", help="mounted card (default /Volumes/APCBOX)")
    ap.add_argument("--go", action="store_true", help="actually convert and copy")
    ap.add_argument("--jobs", type=int, default=8)
    a = ap.parse_args()

    if not (AFCONVERT or FFMPEG) or not (AFINFO or FFPROBE):
        sys.exit("needs afconvert/afinfo (macOS) or ffmpeg/ffprobe")
    roots = [os.path.realpath(os.path.expanduser(s)) for s in (a.src or DEFAULT_SOURCES)]
    roots = [r for r in roots if os.path.isdir(r)]
    if not roots:
        sys.exit("no sample folders found; point me at one with --src")
    print("reading:", *roots, sep="\n  ")

    files = []
    for r in roots:
        for dp, dn, fn in os.walk(r):
            dn[:] = [d for d in dn if not d.startswith(".")]
            files += [os.path.join(dp, f) for f in fn
                      if f.lower().endswith(AUDIO_EXT) and not f.startswith(".")]
    print(f"{len(files)} audio files, measuring lengths...")
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        lens = dict(zip(files, ex.map(duration, files)))

    hits, loops, others = defaultdict(list), [], defaultdict(list)
    too_long = 0
    for f, d in lens.items():
        if d is None:
            continue
        if d > MAX_SECONDS:
            too_long += 1
            continue
        role = role_of(f)
        if d <= ONESHOT_SECONDS and role:
            hits[role].append(f)
        elif d > ONESHOT_SECONDS and re.search(r"loop|beat|groove|break|bpm", f.lower()):
            loops.append(f)
        else:
            others[other_of(f)].append(f)

    # ---- kits: per pack, the n-th of each role, while the pack has enough ----
    by_pack = defaultdict(lambda: defaultdict(list))
    for role, lst in hits.items():
        for f in sorted(lst):
            by_pack[pack_of(f, roots)][role].append(f)
    kits = []
    for pack, roles in sorted(by_pack.items(), key=lambda kv: -len(kv[1])):
        if len(roles) < 5:  # want at least kick, snare, hat + 2 more
            continue
        depth = min(len(v) for k, v in roles.items() if k in ("kick", "snare", "closed hat")) \
            if all(k in roles for k in ("kick", "snare", "closed hat")) else 0
        for i in range(min(depth, 3)):
            kit = {role: roles[role][i % len(roles[role])] for role in ROLES if role in roles}
            kits.append((clean(pack, 34) + (f" {i + 1}" if depth > 1 else ""), kit))
    kits = kits[:MAX_KITS]

    # ---- browser folders: drum hits first, then loops, then the rest ----
    plan = []  # (folder name, [files])
    def add(base, lst):
        lst = sorted(set(lst))
        for i in range(0, len(lst), MAX_FILES):
            if len(plan) >= MAX_FOLDERS:
                return
            part = f" {i // MAX_FILES + 1}" if len(lst) > MAX_FILES else ""
            plan.append((f"{base}{part}", lst[i:i + MAX_FILES]))
    for role in ROLES:
        add(role, hits.get(role, []))
    add("loops", loops)
    for kind in ("bass", "synth", "keys", "vocals", "fx", "other"):
        add(kind, others.get(kind, []))

    n_samples = sum(len(l) for _, l in plan)
    print(f"\nplan for {a.card}:")
    print(f"  {len(kits)} kits:", ", ".join(k for k, _ in kits[:12]) + (" ..." if len(kits) > 12 else ""))
    for name, lst in plan:
        print(f"  samples/{name:<14} {len(lst):4} files")
    left = sum(len(v) for v in hits.values()) + len(loops) + sum(len(v) for v in others.values()) - n_samples
    print(f"  ({left} more didn't fit the folder limits, {too_long} longer than 32 s skipped)")
    if not a.go:
        print("\ndry run. Add --go to convert and copy.")
        return

    if not os.path.isdir(a.card):
        sys.exit(f"{a.card} isn't mounted")
    jobs = []
    for kit_name, kit in kits:
        for i, role in enumerate(ROLES):
            if role in kit:
                jobs.append((kit[role], os.path.join(a.card, "kits", kit_name, f"{i + 1} {role}.wav")))
    for folder, lst in plan:
        used = set()
        for f in lst:
            n = clean(os.path.basename(f))
            k, base = 2, n
            while n.lower() in used:
                n, k = f"{base} {k}", k + 1
            used.add(n.lower())
            jobs.append((f, os.path.join(a.card, "samples", folder, n + ".wav")))
    print(f"\nconverting {len(jobs)} files...")
    done = failed = 0
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for ok in ex.map(lambda j: os.path.exists(j[1]) or convert(*j), jobs):
            done += 1
            failed += 0 if ok else 1
            if done % 200 == 0:
                print(f"  {done}/{len(jobs)}")
    # macOS litters FAT cards with ._ files and a Spotlight index
    open(os.path.join(a.card, ".metadata_never_index"), "a").close()
    if shutil.which("dot_clean"):
        subprocess.run(["dot_clean", "-m", a.card])
    for dp, dn, fn in os.walk(a.card):
        for f in fn:
            if f.startswith("._"):
                os.remove(os.path.join(dp, f))
    print(f"done: {done - failed} written, {failed} failed. Eject the card, put it in the Daisy.")


if __name__ == "__main__":
    main()
