"""counter_harness.py -- shared helpers for the SAMD21 COUNTER-mode serial tests.

Worked examples of commissioning + exercising a COUNTER config-chip. The chip's
counters are commissioned via a `cntr` file (which pads count, their pull + edge,
and a bank-global update rate); the Pico reads ALL counters in one transaction
(READ / READ_CLR -> stream the 36-byte shadow). Undeclared pads are free for the
bench tools -- the DAC on A0 (= D0/CH0) is a square-wave stimulus when CH0 is free
(jumper A0 -> a counter pad; A1 = AIN4 = D1, so the ADC-suite A0->A1 jumper is
already A0->D1).

NOTE: the assertions here are deliberately frequency-INDEPENDENT (edges present
vs. absent, clear semantics, enabled-only), because this board's timer clock runs
~9x slow + jittery (a GCLK0/DFLL issue, see notes) -- so the DAC's absolute output
frequency is currently unreliable. The counting LOGIC is fully exercised; precise
rate checks wait on the clock fix.
"""

import os
import sys
import time

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

import libcomm                       # noqa: E402
from libcomm import Dongle           # noqa: E402
import slave_dsl                     # noqa: E402

R = {
    "MODE":     0x02,
    "READ":     0x10,   # w: snapshot all counters -> shadow
    "READ_CLR": 0x11,   # w: snapshot all + zero all
    "DATA":     0x12,   # r: stream 36-byte shadow (9 x u32 LE)
    "ENABLE":   0x13,   # r: u16 enable bitmap (bit ch = channel is a counter)
    "CLEAR":    0x1A,   # w: 0xFF -> zero all
    "DAC_T1_TYPE": 0x20, "DAC_T1_AMP": 0x21, "DAC_T1_FREQ": 0x23,
    "DAC_APPLY":   0x25, "DAC_T2_TYPE": 0x26,
    "DAC_OFF_LO":  0x2B, "DAC_OFF_HI": 0x2C,
}
T_OFF, T_SQUARE = 0, 3


def find_port():
    ds = libcomm.enumerate_dongles()
    if not ds:
        sys.exit("no register_dongle found")
    return ds[0]["port"]


def commission(unit, settle=9.0):
    files = unit.files()
    dg = Dongle(find_port())
    dg.offline()
    for name, blob in files.items():
        dg.file_put(name, blob)
    dg.close()
    time.sleep(settle)
    return Dongle(find_port()), files


def _w16(dg, base, v):
    dg.reg_write(base, v & 0xFF)
    dg.reg_write(base + 1, (v >> 8) & 0xFF)


def dac_dc(dg, level):
    """Steady DC on A0 (both DAC tones off, output = offset)."""
    dg.reg_write(R["DAC_T1_TYPE"], T_OFF)
    dg.reg_write(R["DAC_T2_TYPE"], T_OFF)
    _w16(dg, R["DAC_OFF_LO"], level)
    dg.reg_write(R["DAC_APPLY"], 1)


def dac_square(dg, freq=200, amp=511, offset=512):
    """Square wave on A0 (full-swing crosses the digital threshold)."""
    dg.reg_write(R["DAC_T1_TYPE"], T_SQUARE)
    _w16(dg, R["DAC_T1_AMP"], amp)
    _w16(dg, R["DAC_T1_FREQ"], freq)
    dg.reg_write(R["DAC_T2_TYPE"], T_OFF)
    _w16(dg, R["DAC_OFF_LO"], offset)
    dg.reg_write(R["DAC_APPLY"], 1)


def read_all(dg, clear=False):
    """Snapshot + stream all 9 counters -> list of 9 u32."""
    dg.reg_write(R["READ_CLR"] if clear else R["READ"], 1)
    b = [dg.reg_read(R["DATA"]) for _ in range(36)]
    return [b[i*4] | (b[i*4+1] << 8) | (b[i*4+2] << 16) | (b[i*4+3] << 24) for i in range(9)]


def enable_bitmap(dg):
    return dg.reg_read(R["ENABLE"]) | (dg.reg_read(R["ENABLE"] + 1) << 8)


class Checker:
    def __init__(self):
        self.fails = []
        self.n = 0

    def eq(self, name, got, exp):
        self.n += 1
        ok = got == exp
        print("    %-30s = %-5s expect %-5s %s" % (name, got, exp, "OK" if ok else "FAIL"))
        if not ok:
            self.fails.append(name)
        return ok

    def done(self, title):
        bad = len(self.fails)
        print("  %s: %s  (%d/%d)" % (title, "PASS" if not bad else "FAIL", self.n - bad, self.n))
        return not bad
