"""gpio_harness.py -- shared helpers for the SAMD21 GPIO serial tests.

These tests are also worked EXAMPLES of how to commission and exercise a GPIO
config-chip over the wire. They run on the Pi (``ssh robot``) where the
register_dongle (USB-CDC) is attached, and drive it through ``libcomm.Dongle``
-- the USB->register bridge -- so they hit the exact register interface the Pico
uses over I2C.

Flow of every test:
    1. build a GPIO unit with slave_dsl (pins + optional interlock)
    2. commission() -> offline, write idnt/gpmp/ilcf, disconnect (reboots the
       chip, which applies the pin map and arms the interlock)
    3. read back the PIO mode-bank registers and assert

The board is location-fixed, so DIR/PULL/OD are commission-static (read-only at
runtime); only output VALUES (OLAT/GPIO) move. Tests that need a specific input
level either use the pin's own pull (in:up/down) or prompt for a manual jumper.
"""

import os
import sys
import time

# libcomm.py + slave_dsl.py live in tools/commission/ (two levels up).
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

import libcomm                       # noqa: E402
from libcomm import Dongle           # noqa: E402
import slave_dsl                     # noqa: E402

# PIO/GPIO mode-bank registers (see samd21_commands.c REG_PIO_*).
R = {
    "MODE":    0x02,   # control bank: current mode (1 = GPIO)
    "IODIR":   0x10,   # 1 = input, 0 = output            (read-only at runtime)
    "GPPU":    0x11,   # pull-up enable                   (read-only at runtime)
    "IPOL":    0x12,   # input read polarity invert
    "GPIO":    0x13,   # live pin reads
    "OLAT":    0x14,   # output values (push-pull level / open-drain low-or-Hi-Z)
    "ILSTAT":  0x15,   # interlock il_parse status (0 = OK)
    "ILSTATE": 0x16,   # bit0 tripped, bit1 cond-ok, bit2 armed
    "GPPD":    0x17,   # pull-down enable                 (read-only at runtime)
    "OD":      0x18,   # open-drain output mask           (read-only)
}
GPIO_PINS = slave_dsl.GPIO_PINS          # ('D0','D1','D2','D3','D7','D8','D9','D10') = bit 0..7


def bit(pin):
    """Bit index of a GPIO channel in the register bitmaps."""
    return GPIO_PINS.index(pin.upper())


def find_port():
    ds = libcomm.enumerate_dongles()
    if not ds:
        sys.exit("no register_dongle found (is the XIAO enumerated as 2886:802f?)")
    return ds[0]["port"]


def commission(unit, settle=9.0, erase=True):
    """Write a slave_dsl.Unit's files and reboot into the new config.

    Returns (Dongle, files). The Dongle is freshly opened after the reboot
    (the USB port may renumber, so we re-discover it).
    """
    files = unit.files()
    dg = Dongle(find_port())
    dg.offline()                              # commissioning gate: writes need offline
    if erase:
        dg.file_format()                      # discrete clean-slate: wipe the whole store first
    for name, blob in files.items():
        dg.file_put(name, blob)
    dg.close()                                # disconnect -> reboot -> apply config
    time.sleep(settle)
    return Dongle(find_port()), files


def _fmt(v):
    return ("0x%02X" % v) if isinstance(v, int) else repr(v)


class Checker:
    """Tiny assert/report helper so each test prints a readable pass/fail table."""

    def __init__(self):
        self.fails = []
        self.n = 0

    def eq(self, name, got, exp):
        self.n += 1
        ok = got == exp
        print("  %-24s = %-6s expect %-6s %s"
              % (name, _fmt(got), _fmt(exp), "OK" if ok else "FAIL"))
        if not ok:
            self.fails.append(name)
        return ok

    def done(self, title):
        bad = len(self.fails)
        status = "PASS" if not bad else ("FAIL " + ",".join(self.fails))
        print("%s: %s  (%d/%d)" % (title, status, self.n - bad, self.n))
        return not bad
