"""Puts the building blocks together and runs them."""
from .common import *
from .util import *
from .daisy import *
from .midi import *
from .samples import *
from .state import *
from .ir import *
from .hdmi import *
from .web import *
from .router import *
from .netmidi import *
from .clock import Clock
from .listen import Listen
from .ble import Ble
from .devices import Devices
from .jam import Jam


class Bridge(DaisyMixin, PortsMixin, RouterMixin, NetMixin, SamplesMixin, WebMixin):
    """The building blocks together; one per T95"""

    def __init__(self, args):
        self.args = args
        self.cache = StateCache()
        self.asm = LineAssembler()
        self.clients = set()
        self.fd = None
        self.port = None
        self.serial = None
        self.connected_at = None
        self.last_line = None
        self.last_ask = 0.0
        self.lines = 0
        self.connects = 0
        self.client_resyncs = 0
        self.rejected = 0
        self.lost = asyncio.Event()
        self.sout = bytearray()          # waiting to go to the Daisy
        self.serial_dropped = 0
        self.ctl = 0                     # Daisy control protocol version (P line)
        self.p_line = None
        self.firmware = None             # what the Daisy calls its firmware (VF line, report 17), e.g. "13.17"
        self.daisy_ports = set()         # port numbers announced to the Daisy (routed to it)
        self.ports = {}                  # num -> MidiPort
        self.pings = {}                  # id -> send time
        self.ping_seq = 0
        self.rtts = collections.deque(maxlen=PING_KEEP)
        self.pings_lost = 0
        self.midi_paths = dict(x.split("=", 1) if "=" in x else (x, os.path.basename(x))
                               for x in (args.midi or []))
        with open(args.ui, "rb") as f:
            self.page = f.read()
        self.selector = b"<!doctype html><title>no selector</title><a href=/apcbox>ApcBox</a>"
        try:
            with open(args.selector, "rb") as f:
                self.selector = f.read()
        except (OSError, TypeError):
            log.warning("no selector page at %s", args.selector)
        # small.css, small.js: the frame every page shares on the 480x320 touch screen (?small).
        # They sit beside the start page.
        self.shared = {}
        for name in ("small.css", "small.js"):
            try:
                with open(os.path.join(os.path.dirname(os.path.abspath(args.selector)), name), "rb") as f:
                    self.shared[name] = f.read()
            except (OSError, TypeError):
                log.warning("no %s beside the start page: pages opened with ?small stay empty", name)
        self.pages = {}
        for name, path in (("clock", args.clock_page), ("apcbox", args.apcbox_page), ("settings", args.settings_page),
                           ("jam", args.jam_page)):
            try:
                with open(path, "rb") as f:
                    self.pages[name] = f.read()
            except (OSError, TypeError):
                log.warning("no %s page at %s", name, path)
                self.pages[name] = b"<!doctype html><title>missing page</title><a href=/>modules</a>"
        self.hub = b"<!doctype html><title>no hub page</title><a href=/>modules</a>"
        try:
            with open(args.hub, "rb") as f:
                self.hub = f.read()
        except (OSError, TypeError):
            log.warning("no MIDI hub page at %s", args.hub)
        self.state = State(args.state)
        self.ir = IrReader(self)
        self.hdmi = HdmiAudio(self)
        self.listen = Listen(self)
        self.ble = Ble(self)
        self.devices = Devices(self)
        self.library = args.library or os.path.join(os.path.dirname(args.state), "samples")
        self.sdcard = args.sdcard
        self.sout_lo = bytearray()       # sample data: goes only when no MIDI is waiting
        self.xfer_q = collections.deque()
        self.xfer_cur = None
        self.xfer_done = collections.deque(maxlen=10)
        self.xfer_seq = 0
        self.xfer_wake = asyncio.Event()
        self.xfer_replies = asyncio.Queue()
        self.jam = Jam(self)
        self.net_init()
        self.clock = Clock(self, self.state.data.get("clock") or {})

    def status(self):
        t = now()
        return {
            "daisy": self.fd is not None,
            "port": self.port,
            "serial": self.serial,
            "data_age_s": None if self.last_line is None else round(t - self.last_line, 3),
            "connected_s": None if self.connected_at is None else round(t - self.connected_at, 1),
            "lines": self.lines,
            "bad_lines": self.asm.bad,
            "connects": self.connects,
            "clients": len(self.clients),
            "client_resyncs": self.client_resyncs + sum(c.resyncs for c in self.clients),
            "rejected_clients": self.rejected,
            "control": self.ctl,
            "firmware": self.firmware,
            "serial_out_bytes": len(self.sout),
            "serial_dropped": self.serial_dropped,
            "controllers": [p.info(self.kinds()) for p in sorted(self.ports.values(), key=lambda p: p.num)],
            "ping_ms": self.ping_stats(),
            "ir": self.ir.info(),
            "samples": self.samples_info(),
            "hdmi_audio": self.hdmi.info(),
            "ble_midi": self.ble.info(),
            "devices": self.devices.info(),
            "net": self.net_info(),
            "clock": self.clock.info(),
            "listen": self.listen.info(),
            "jam": self.jam.info(),
        }


async def main_async(args):
    b = Bridge(args)
    server = await asyncio.start_server(b.handle_http, args.host, args.http_port, limit=16384)
    log.info("serving http://%s:%d/", args.host, args.http_port)
    servers = [server.serve_forever()]
    tls = None
    if args.https_port:
        try:
            ctx = tls_context(os.path.dirname(args.state))
            tls = await asyncio.start_server(b.handle_http, args.host, args.https_port, ssl=ctx, limit=16384)
            servers.append(tls.serve_forever())
            log.info("serving https://%s:%d/", args.host, args.https_port)
        except (OSError, ssl.SSLError) as e:
            log.warning("no HTTPS (the page's record button needs it on phones): %s", e)
    import signal
    b.clock.apply_link()
    b.clock.start_thread()
    me = asyncio.current_task()
    asyncio.get_running_loop().add_signal_handler(signal.SIGTERM, me.cancel)  # finally blocks run (BY to peers)
    try:
        await asyncio.gather(*servers, b.serial_task(), b.midi_task(), b.control_task(),
                             b.ir.task(), b.xfer_task(), b.hdmi.task(), b.net_task(), b.ble.task(), b.devices.task(), b.jam.task())
    finally:
        # Stop taking connections, and don't wait for the open ones. `async with server` did:
        # from Python 3.12 on Server.wait_closed() waits until every browser has left, and the
        # page on the box's own screen never does, so a stop sat there until systemd's 90 s
        # were up and it killed the bridge (rig, 2026-11-24: every restart that night).
        # asyncio.run cancels the browsers' handlers once this returns; their finally blocks run.
        b.jam.stop_now()      # a take that is running: arecord closes its file
        server.close()
        if tls:
            tls.close()


def main(argv=None):
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # t95/bridge or /opt/apcbox-bridge
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--http-port", type=int, default=8040)
    ap.add_argument("--https-port", type=int, default=8443, help="0 = no HTTPS")
    ap.add_argument("--flush-ms", type=float, default=40,
                    help="browsers on the network get their live lines in one write per this many ms (0 = each line at once)")
    ap.add_argument("--flush-local", action="store_true", help="pace browsers on the box itself too (tests)")
    ap.add_argument("--serial", help="USB serial number of the Daisy to use (several connected)")
    ap.add_argument("--port", help="tty path instead of finding the Daisy by VID/PID")
    ap.add_argument("--midi", action="append", metavar="PATH=NAME",
                    help="controller device path instead of scanning /sys (repeatable)")
    ap.add_argument("--no-midi", action="store_true", help="don't touch MIDI controllers")
    ap.add_argument("--ui", default=os.path.join(here, "apcbox-ui.html")
                    if os.path.exists(os.path.join(here, "apcbox-ui.html"))
                    else os.path.join(here, "..", "..", "daisy", "ui", "apcbox-ui.html"))
    ap.add_argument("--selector", default=next((p for p in (os.path.join(here, "selector.html"),
                    os.path.join(here, "..", "ui", "selector.html")) if os.path.exists(p)), None))
    ap.add_argument("--hub", default=next((p for p in (os.path.join(here, "midihub.html"),
                    os.path.join(here, "..", "ui", "midihub.html")) if os.path.exists(p)), None))
    ap.add_argument("--apcbox-page", default=next((p for p in (os.path.join(here, "apcbox.html"),
                    os.path.join(here, "..", "ui", "apcbox.html")) if os.path.exists(p)), None),
                    help="the T95's ApcBox page (/apcbox); --ui is the classic monitor page (/classic)")
    ap.add_argument("--settings-page", default=next((p for p in (os.path.join(here, "settings.html"),
                                                                    os.path.join(here, "..", "ui", "settings.html"))
                                                           if os.path.exists(p)), ""),
                    help="the Settings page (network, Bluetooth, system)")
    ap.add_argument("--helper-socket", default="/run/ghettorack/helper.sock",
                    help="where the settings helper (root, t95/helper) listens")
    ap.add_argument("--clock-page", default=next((p for p in (os.path.join(here, "clock.html"),
                    os.path.join(here, "..", "ui", "clock.html")) if os.path.exists(p)), None))
    ap.add_argument("--jam-page", default=next((p for p in (os.path.join(here, "jam.html"),
                    os.path.join(here, "..", "ui", "jam.html")) if os.path.exists(p)), None),
                    help="the jam recorder's page (/jam)")
    ap.add_argument("--jams", help="where recorded takes go (default: jams, next to the state file)")
    ap.add_argument("--jam-min-free-mb", type=int, default=1024, help="recording stops when the T95 has less than this free")
    ap.add_argument("--jam-split-s", type=int, default=1800, help="a take is written as files of this many seconds")
    ap.add_argument("--state", default=os.path.join(os.environ.get("STATE_DIRECTORY", "/var/lib/apcbox-bridge"),
                    "state.json"), help="where the last module and IR bindings are kept")
    ap.add_argument("--ir", metavar="PATH", help="IR input event device instead of finding it under /sys")
    ap.add_argument("--ir-lirc", metavar="PATH", help="IR lirc device instead of finding it under /sys")
    ap.add_argument("--library", help="sample library folder (default: next to the state file)")
    ap.add_argument("--alsaloop", default="/usr/bin/alsaloop", help="for the HDMI audio setting")
    ap.add_argument("--arecord", default="/usr/bin/arecord", help="for listening in a browser (/audio.pcm)")
    ap.add_argument("--ble", action="store_true", help="Bluetooth MIDI: paired devices as controller ports (BlueZ over D-Bus)")
    ap.add_argument("--asound", default="/proc/asound", help="where the sound cards are listed by name")
    ap.add_argument("--daisy-card", default="ApcBox", help="the Daisy's USB audio card name")
    ap.add_argument("--hdmi-card", default="HDMI", help="the T95's HDMI audio card name")
    ap.add_argument("--loop-latency-us", type=int, default=40000, help="alsaloop -t: how much sound the pass-through keeps queued")
    ap.add_argument("--loop-sync", type=int, default=1, choices=range(6),
                    help="alsaloop -S: how it follows the Daisy's clock. 1 (simple) lets the queue creep up to the whole "
                         "buffer over hours (153 ms seen on a Pi); 4 (samplerate) holds it")
    ap.add_argument("--sdcard", default="/media/sdcard", help="where the T95's SD card is mounted (udev rule)")
    ap.add_argument("--sysblock", default="/sys/block", help="where the block devices are listed (is a card in the SD slot)")
    ap.add_argument("--sdcard-any", action="store_true", help="use --sdcard even if it isn't a mount point (tests)")
    ap.add_argument("--rtp-port", type=int, default=5004, help="network MIDI control port (data is +1); 0 = off")
    ap.add_argument("--net-name", help="the name other devices see (default GhettoRack)")
    ap.add_argument("--net-peer", action="append", metavar="HOST:PORT=NAME",
                    help="a network MIDI device to invite without Bonjour (repeatable)")
    ap.add_argument("--no-browse", action="store_true", help="don't look for network MIDI devices with avahi")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(levelname)s %(message)s", stream=sys.stderr)
    try:
        asyncio.run(main_async(args))
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass
