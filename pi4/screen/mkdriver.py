#!/usr/bin/env python3
"""Make st7796sr.c from the Raspberry Pi kernel's drivers/gpu/drm/tiny/ili9486.c:
same shift-register bus code, the ST7796U's start-up commands (daisy/tft.h)."""
import sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()


def rep(a, b, every=False):
    global s
    assert a in s, a
    s = s.replace(a, b) if every else s.replace(a, b, 1)


i0 = s.index("\tmipi_dbi_command(dbi, ILI9486_ITFCTR1);")
i1 = s.index(" out_enable:")
init = (
    "\tmipi_dbi_command(dbi, MIPI_DCS_SOFT_RESET);\n"
    "\tmsleep(150);\n"
    "\tmipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);\n"
    "\tmsleep(150);\n"
    "\t/* the sequence ApcBox uses on this screen (daisy/tft.h) */\n"
    "\tmipi_dbi_command(dbi, ST7796_CSCON, 0xc3);\t/* unlock extended commands */\n"
    "\tmipi_dbi_command(dbi, ST7796_CSCON, 0x96);\n"
    "\tmipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, 0x55);\n"
    "\tmipi_dbi_command(dbi, ST7796_DIC, 0x01);\t/* 1-dot inversion */\n"
    "\tmipi_dbi_command(dbi, MIPI_DCS_ENTER_INVERT_MODE);\t/* IPS panel */\n"
    "\tmipi_dbi_command(dbi, ST7796_CSCON, 0x3c);\t/* lock again */\n"
    "\tmipi_dbi_command(dbi, ST7796_CSCON, 0x69);\n"
    "\tmipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);\n"
    "\tmsleep(20);\n\n"
)
s = s[:i0] + init + s[i1:]

d0 = s.index("#define ILI9486_ITFCTR1")
d1 = s.index("#define ILI9486_MADCTL_BGR")
s = s[:d0] + "#define ST7796_DIC              0xb4\n#define ST7796_CSCON            0xf0\n" + s[d1:]

rep(
    "\tcase 90:\n\t\taddr_mode = ILI9486_MADCTL_MY;\n\t\tbreak;\n"
    "\tcase 180:\n\t\taddr_mode = ILI9486_MADCTL_MV;\n\t\tbreak;\n"
    "\tcase 270:\n\t\taddr_mode = ILI9486_MADCTL_MX;\n\t\tbreak;\n"
    "\tdefault:\n\t\taddr_mode = ILI9486_MADCTL_MV | ILI9486_MADCTL_MY |\n"
    "\t\t\tILI9486_MADCTL_MX;\n\t\tbreak;",
    "\tcase 90:\n\t\taddr_mode = ILI9486_MADCTL_MX;\n\t\tbreak;\n"
    "\tcase 180:\n\t\taddr_mode = ILI9486_MADCTL_MV | ILI9486_MADCTL_MY |\n"
    "\t\t\tILI9486_MADCTL_MX;\n\t\tbreak;\n"
    "\tcase 270:\n\t\taddr_mode = ILI9486_MADCTL_MY;\n\t\tbreak;\n"
    "\tdefault:\t/* landscape, as ApcBox draws it on the Daisy (0x28) */\n"
    "\t\taddr_mode = ILI9486_MADCTL_MV;\n\t\tbreak;",
)
rep('\t{ .compatible = "waveshare,rpi-lcd-35" },\n\t{ .compatible = "ozzmaker,piscreen" },',
    '\t{ .compatible = "ghettorack,sunfounder35-st7796" },')
rep('\t{ "rpi-lcd-35", 0 },\n\t{ "piscreen", 0 },', '\t{ "sunfounder35-st7796", 0 },')
rep('"Ilitek ILI9486"', '"ST7796U, 16-bit shift register"')
# The kernel's mode has a 1 kHz pixel clock, which works out as a refresh rate of
# 0.0065 Hz. A Wayland compositor and Chromium pace their frames by that rate:
# under 2 frames a second on the Pi, with the SPI link nearly idle. Say 30 Hz;
# the flush itself still takes as long as it takes.
rep("\tDRM_SIMPLE_MODE(480, 320, 73, 49),\n",
    "\t.type = DRM_MODE_TYPE_DRIVER,\n"
    "\t.clock = 480 * 320 * 30 / 1000,\n"
    "\t.hdisplay = 480, .hsync_start = 480, .hsync_end = 480, .htotal = 480,\n"
    "\t.vdisplay = 320, .vsync_start = 320, .vsync_end = 320, .vtotal = 320,\n"
    "\t.width_mm = 73, .height_mm = 49,\n")
rep("Ilitek ILI9486 DRM driver", "ST7796U behind a 16-bit shift register, DRM driver")
rep("ILI9486_MADCTL", "ST7796_MADCTL", every=True)
rep("ili9486", "st7796sr", every=True)
rep(
    " * DRM driver for Ilitek ILI9486 panels\n",
    ' * DRM driver for the SunFounder 3.5" IPS screen for the Raspberry Pi: a Sitronix\n'
    " * ST7796U behind an SPI to 16-bit shift register board (GhettoRack).\n"
    " *\n"
    " * This is drivers/gpu/drm/tiny/ili9486.c of the Raspberry Pi kernel (rpi-6.18.y)\n"
    " * with the ST7796U's start-up commands and its own names; made by mkdriver.py.\n",
)
open(dst, "w").write(s)
