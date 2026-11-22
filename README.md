# GhettoRack

A DIY groovebox rig: an Akai APC40 mk1 driving a Daisy Seed that does the sound
(16 tracks, samples and 303 voices, sequencer, mixer, FX, sets), with an old
T95 TV box turned Linux companion for the screen, controllers, files and network.

| Folder | What |
|---|---|
| [`daisy/`](daisy/) | ApcBox firmware for the Daisy Seed, Mac build, monitor UI, tests |
| [`t95/`](t95/) | The T95 companion box: findings and plan |
| [`protocol/`](protocol/serial.md) | What the Daisy's USB presents, and the serial report lines |
| [`tools/`](tools/) | SD card filler, fresh-Seed setup, flash script |
| [`docs/`](docs/) | Wiring guide with diagrams (open `wiring-guide.html` in a browser) |
| `lib/` | libDaisy and DaisySP, pinned |

Related: [uhntiss](https://github.com/BIM4GIB/uhntiss), home of the Pocket 303.

## Get it on a Mac

```
cd ~/dev
git clone --recursive https://github.com/BIM4GIB/GhettoRack.git
cd GhettoRack
make -C lib/libDaisy && make -C lib/DaisySP && make -C lib/DaisySP/DaisySP-LGPL
make -C daisy
```

Flash a Seed that already runs ApcBox: `tools/flash-seed.sh` (pick its port).
A brand-new Seed: `tools/seed2-setup.sh` (walks you through it).
Without building anything, both use `daisy/release/ApcBox.bin`.

Agents: start with [AGENTS.md](AGENTS.md).
