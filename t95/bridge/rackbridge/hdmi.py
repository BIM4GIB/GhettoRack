"""Sound out of the box: the Daisy's USB audio passed to one of the box's own
outputs with alsaloop (the TV over HDMI, or a headphone socket)."""
from .common import *
from .util import *
from .devices import sound_cards


class HdmiAudio:
    """The Daisy's USB audio (capture card ApcBox) passed on to the TV over
    HDMI with alsaloop, while the setting is on and both cards exist.
    alsaloop exits when the Daisy goes away; it's started again when it's
    back. Adds ~40 ms plus the TV's own delay: for listening, not playing."""

    def __init__(self, bridge):
        self.b = bridge
        self.proc = None
        self.starts = 0
        self.last_error = None
        self.started_at = 0.0
        self.used_shared = False
        self.playing_on = None

    def outputs(self):
        """The cards sound can go out on: every one that plays, but the Daisy's own"""
        a = self.b.args
        try:
            outs = [c["id"] for c in sound_cards(a.asound) if c["play"] and c["id"] != a.daisy_card]
        except OSError:
            outs = []
        if a.hdmi_card not in outs and os.path.exists(os.path.join(a.asound, a.hdmi_card)):
            outs.insert(0, a.hdmi_card)       # the card named on the command line counts whenever it is there
        return outs

    def card(self):
        """Where the sound goes: the card picked in Settings; else the HDMI one (--hdmi-card, the
        T95's); else the first there is. A Pi has no card of that name: its first is the headphone
        socket, and before this nothing played there with the setting on."""
        outs, pick = self.outputs(), self.b.state.data.get("sound_out")
        return pick if pick in outs else self.b.args.hdmi_card if self.b.args.hdmi_card in outs else outs[0] if outs else None

    def cards(self):
        root = self.b.args.asound
        return (os.path.exists(os.path.join(root, self.b.args.daisy_card)), self.card() is not None)

    def info(self):
        daisy, hdmi = self.cards()
        return {"on": self.b.state.data["hdmi_audio"], "running": self.running(), "card": self.card(), "cards": self.outputs(),
                "daisy_card": daisy, "hdmi_card": hdmi, "starts": self.starts, "error": self.last_error}

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def stop(self):
        if self.running():
            self.proc.terminate()
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None

    async def task(self):
        while True:
            want = self.b.state.data["hdmi_audio"]
            daisy, hdmi = self.cards()
            card = self.card()
            if self.running() and card != self.playing_on:      # another output was picked
                self.stop()
            if want and daisy and hdmi and not self.running():
                if self.proc is not None and self.proc.returncode not in (None, 0, -15):
                    self.last_error = "alsaloop exited %s" % self.proc.returncode
                    # The shared capture device (listen.py) died at once with the Daisy still there:
                    # it doesn't work on this box. Open the card directly, as before.
                    if self.used_shared and now() - self.started_at < 4 and self.b.listen.shared:
                        self.b.listen.shared = False
                        log.warning("HDMI audio: the shared capture device doesn't work here; opening the card directly "
                                    "(listening in a browser then needs the TV sound off)")
                cmd = [self.b.args.alsaloop, "-C", self.b.listen.device(),
                       "-P", "plughw:CARD=" + card, "-t", str(self.b.args.loop_latency_us), "-S", str(self.b.args.loop_sync)]
                try:
                    self.used_shared = self.b.listen.shared
                    self.started_at = now()
                    self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                                 stderr=subprocess.DEVNULL, env=self.b.listen.env())
                    self.starts += 1
                    self.playing_on = card
                    log.info("HDMI audio on (%s)", " ".join(cmd))
                except OSError as e:
                    self.last_error = str(e)
                    self.proc = None
            elif (not want or not daisy) and self.running():
                self.stop()
                log.info("HDMI audio off")
            await asyncio.sleep(2)
