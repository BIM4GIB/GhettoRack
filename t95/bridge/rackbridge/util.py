"""Small helpers: clock, raw tty, SSE encoding."""
from .common import *


def now():
    return time.monotonic()


def open_raw(path):
    """Open a tty raw, echo off, before reading anything"""
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        open_raw_fd(fd)
    except termios.error:
        os.close(fd)
        raise
    return fd


def open_raw_fd(fd):
    """Raw, echo off (cfmakeraw)"""
    a = termios.tcgetattr(fd)
    # cfmakeraw
    a[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP
              | termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
    a[1] &= ~termios.OPOST
    a[2] &= ~(termios.CSIZE | termios.PARENB)
    a[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
    a[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
    a[6][termios.VMIN] = 1
    a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)


def sse_data(line):
    return ("data: " + line + "\n\n").encode()


def sse_event(name, obj):
    return ("event: " + name + "\ndata: " + json.dumps(obj, separators=(",", ":")) + "\n\n").encode()
