"""Sound on the TV: the Daisy's USB audio passed to HDMI with alsaloop."""
from .common import *
from .util import *


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

    def cards(self):
        root = self.b.args.asound
        return (os.path.exists(os.path.join(root, self.b.args.daisy_card)),
                os.path.exists(os.path.join(root, self.b.args.hdmi_card)))

    def info(self):
        daisy, hdmi = self.cards()
        return {"on": self.b.state.data["hdmi_audio"], "running": self.running(),
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
                       "-P", "plughw:CARD=" + self.b.args.hdmi_card, "-t", "40000", "-S", "1"]
                try:
                    self.used_shared = self.b.listen.shared
                    self.started_at = now()
                    self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                                 stderr=subprocess.DEVNULL, env=self.b.listen.env())
                    self.starts += 1
                    log.info("HDMI audio on (%s)", " ".join(cmd))
                except OSError as e:
                    self.last_error = str(e)
                    self.proc = None
            elif (not want or not daisy) and self.running():
                self.stop()
                log.info("HDMI audio off")
            await asyncio.sleep(2)
