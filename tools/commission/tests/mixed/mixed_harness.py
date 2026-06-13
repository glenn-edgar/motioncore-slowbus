"""mixed_harness.py -- shared helpers for the SAMD21 MIXED-mode serial tests.

Also worked EXAMPLES of commissioning + exercising a MIXED config-chip over the
wire. They run on the Pi (``ssh robot``) and drive the register_dongle through
``libcomm.Dongle`` -- the exact register interface the Pico uses over I2C.

MIXED mode runs ONE interlock that mixes ADC thresholds and (debounced) GPIO
levels in a single `&& || ~` expression, sampled at ~100 Hz. The DAC bench tool
is available in MIXED too, so -- with the A0->A1 jumper -- the DAC is a
controllable source for the ADC half, exactly like the ADC suite. The GPIO half
is exercised jumperless via the input's commissioned pull (in:up -> 1, in:down ->
0); re-commission to toggle it.

Flow of every test:
    1. build a MIXED unit with slave_dsl (pins + interlock)
    2. commission() -> offline, write idnt/ilcf, disconnect (reboots, arms)
    3. drive the DAC (A1 source) / set GPIO via pull, read the MIXED bank, assert
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
    "INT_FLAGS": 0x04,   # W1C; bit1 (0x02) = MIXED interlock trip
    "CH_SEL":    0x10,   # w: input index 0..input_count-1 to inspect
    "CH_ROLE":   0x11,   # r: 0=unused 1=GPIO 2=ADC
    "ADC_VAL":   0x12,   # r: u16 16x value of the selected input (if ADC)
    "GPIO_RAW":  0x14,   # r: raw-level bitmap (bit i = input i if GPIO)
    "GPIO_DEB":  0x15,   # r: debounced bitmap
    "ILSTATE":   0x16,   # r: bit0 tripped, bit1 cond-ok, bit2 armed
    "ILSTAT":    0x17,   # r: il_parse status (0 = OK/armed)
    # DAC bench tool (same regs as ADC mode; available in MIXED as the A1 source)
    "DAC_T1_TYPE": 0x20, "DAC_T2_TYPE": 0x26,
    "DAC_OFF_LO":  0x2B, "DAC_OFF_HI": 0x2C, "DAC_APPLY": 0x25,
}
MIXED_INT_BIT = 0x02
T_OFF = 0


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


def dac_dc(dg, level):
    """Drive the DAC as a steady DC source (both tones off, output = offset)."""
    dg.reg_write(R["DAC_T1_TYPE"], T_OFF)
    dg.reg_write(R["DAC_T2_TYPE"], T_OFF)
    dg.reg_write(R["DAC_OFF_LO"], level & 0xFF)
    dg.reg_write(R["DAC_OFF_HI"], (level >> 8) & 0xFF)
    dg.reg_write(R["DAC_APPLY"], 1)


def ilstate(dg):
    s = dg.reg_read(R["ILSTATE"])
    return {"tripped": s & 1, "cond_ok": (s >> 1) & 1, "armed": (s >> 2) & 1}


def clear_trip(dg):
    dg.reg_write(R["INT_FLAGS"], MIXED_INT_BIT)   # un-latch so tripped follows live


def adc_val(dg, idx):
    dg.reg_write(R["CH_SEL"], idx)
    time.sleep(0.02)
    return dg.reg_read(R["ADC_VAL"]) | (dg.reg_read(R["ADC_VAL"] + 1) << 8)


def gpio_deb(dg):
    return dg.reg_read(R["GPIO_DEB"])


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
