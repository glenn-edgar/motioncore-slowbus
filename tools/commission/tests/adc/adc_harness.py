"""adc_harness.py -- shared helpers for the SAMD21 ADC-mode serial tests.

ADC mode is a free-running 8-channel sampler (tumbling-window min/max/avg/rms)
plus a DAC and an interlock (DNF over per-watch ADC streams -> drives D6). These
tests use the DAC as a controllable self-test source:

    A0/DAC  --(jumper A0->A1)-->  A1 (AIN4)

so they REQUIRE that one jumper. The DAC out = level/1023 * 3.3V; the ADC reads
~ level*4 counts. Run on the Pi (ssh robot) where the register_dongle is attached.

Stream selectors in the DSL: A1 (instantaneous), A1.<stat>.<window> with
stat avg/min/max/rms and window fast/mid/slow (10/1/0.1 Hz tumbling). A windowed
stat only refreshes when its window fills, so allow settle time after a DAC step.
"""

import os
import sys
import time

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

import libcomm                       # noqa: E402
from libcomm import Dongle           # noqa: E402
import slave_dsl                     # noqa: E402

R = {
    "MODE":      0x02,
    "INT_FLAGS": 0x04,   # W1C; bit3 = ADC interlock trip
    "CH_SEL":    0x10,   # ADC channel 0..7 (A1=0, A2=1, A3=2, D6=3, ...)
    "WIN_SEL":   0x11,   # window 0 fast / 1 mid / 2 slow
    "AVG":       0x18,   # u16 window average (block 0x12..0x1B = seq,min,max,avg,rms)
    "ILSTAT":    0x1C,   # interlock il_parse status (0 = armed)
    "ILSTATE":   0x1D,   # bit0 tripped, bit1 cond-ok, bit2 armed
    "DAC_MODE":  0x20,   # 0 constant
    "DAC_LO":    0x21, "DAC_HI": 0x22,
    "DAC_APPLY": 0x25,
}
ADC_IL_INT_BIT = 0x08
WIN = {"fast": 0, "mid": 1, "slow": 2}
WIN_SETTLE = {0: 0.4, 1: 3.0, 2: 25.0}   # seconds to flush the tumbling window


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


def dac_const(dg):
    dg.reg_write(R["DAC_MODE"], 0)           # constant


def set_dac(dg, level):
    dg.reg_write(R["DAC_LO"], level & 0xFF)
    dg.reg_write(R["DAC_HI"], (level >> 8) & 0xFF)
    dg.reg_write(R["DAC_APPLY"], 1)


def read_avg(dg, ch=0, win=0):
    dg.reg_write(R["CH_SEL"], ch)
    dg.reg_write(R["WIN_SEL"], win)
    time.sleep(0.02)
    return dg.reg_read(R["AVG"]) | (dg.reg_read(R["AVG"] + 1) << 8)


def clear_trip(dg):
    dg.reg_write(R["INT_FLAGS"], ADC_IL_INT_BIT)   # un-latch so tripped follows live


def ilstate(dg):
    st = dg.reg_read(R["ILSTATE"])
    return {"tripped": st & 1, "cond_ok": (st >> 1) & 1, "armed": (st >> 2) & 1}


def volts_to_count(v):
    return slave_dsl._volts_to_count(v, slave_dsl.VREF_DEFAULT)


class Checker:
    def __init__(self):
        self.fails = []
        self.n = 0

    def eq(self, name, got, exp):
        self.n += 1
        ok = got == exp
        print("    %-28s = %-4s expect %-4s %s" % (name, got, exp, "OK" if ok else "FAIL"))
        if not ok:
            self.fails.append(name)
        return ok

    def done(self, title):
        bad = len(self.fails)
        print("  %s: %s  (%d/%d)" % (title, "PASS" if not bad else "FAIL", self.n - bad, self.n))
        return not bad
