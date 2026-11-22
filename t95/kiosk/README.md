# Kiosk: GhettoRack on the TV

At boot the T95 shows the module selector full screen on HDMI: Chromium in
`cage` (a Wayland compositor that runs exactly one app), as a `kiosk`
user, on tty1, restarted if it crashes. Phones and laptops open the same
pages over Wi-Fi at `http://<t95-ip>:8040/`.

```
sudo ~/GhettoRack/t95/bridge/install.sh     # first: the bridge serves the pages
sudo ~/GhettoRack/t95/kiosk/install.sh      # packages, kiosk user, service
sudo ~/GhettoRack/t95/kiosk/install.sh --remove   # back to the text console
journalctl -u kiosk -f
```

The kiosk opens `/?boot=1&tv=1`: the selector counts down (default 5 s,
set on the page) and opens the last module with the TV layout. Any remote
button, key or touch during the countdown stays on the selector.

It takes the HDMI console (tty1); SSH keeps working. `--remove` gives the
console back. Packages stay installed.

## Speed

`python3 t95/kiosk/gpuinfo.py` prints what `chrome://gpu` would: whether
compositing and rasterization run on the Mali (panfrost) or in software.
It uses Chromium's DevTools, which the service opens on 127.0.0.1:9222
only (anyone logged into the T95 could drive the browser through it).

Try Chromium flags without editing the service:

```
echo 'CHROMIUM_FLAGS=--ignore-gpu-blocklist --enable-gpu-rasterization' | sudo tee /etc/default/kiosk
sudo systemctl restart kiosk
sudo rm /etc/default/kiosk; sudo systemctl restart kiosk     # back to plain
```

Seen on the T95 (2026-11-22): panfrost loaded, `/dev/dri/renderD128`
present, HDMI at 1920x1080, pages very slow to draw, slowest at first.

**Not tried on the T95 yet:** whether cage and Chromium get GPU
acceleration from the H616's Mali (panfrost) on this kernel, how fast the
pages are, and the TV's resolution. If Chromium is too slow, try
`--disable-gpu` in `kiosk.service` first.
