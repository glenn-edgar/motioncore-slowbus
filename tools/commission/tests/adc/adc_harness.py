"""adc_harness.py -- shared helpers for the SAMD21 ADC-mode serial tests.

ADC mode is a free-running 8-channel sampler (tumbling-window min/max/avg/rms)
plus a DAC and an interlock (DNF over per-watch ADC streams -> drives D6). These
tests use the DAC as a controllable self-test source:

    A0/DAC  --(jumper A0->A1)-->  A1 (AIN4)

so they REQUIRE that one jumper. The DAC out = level/1023 * 3.3V; the ADC reads
~ level*4 counts. Run on the Pi (ssh robot) where the register_dongle is attached.

Stream selectors in the DSL: A1 (instantaneous), A1.<stat>.<window> with
stat avg/min/max/rms and window fast/mid/slow (100/1000/10000 samples ~ 0.8/8/80 s
tumbling at the measured ~125 Hz/channel rate). A windowed
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
    "OVERRUN":   0x10,   # r: u8 saturating count of 16 kHz slots a conversion overran (~0)
    "WIN_SEL":   0x11,   # downsample window 0=khz1 / 1=hz100 / 2=hz10
    "AVG":       0x18,   # u16 window average (block 0x12..0x1B = seq,min,max,avg,rms)
    "LATEST":    0x1E,   # u16 most recent raw 16 kHz sample (instantaneous)
    "ILSTAT":    0x1C,   # interlock il_parse status (0 = armed)
    "ILSTATE":   0x1D,   # bit0 tripped, bit1 cond-ok, bit2 armed
    "DAC_T1_TYPE": 0x20, "DAC_T1_AMP": 0x21, "DAC_T1_FREQ": 0x23,  # tone 0
    "DAC_APPLY":   0x25,
    "DAC_T2_TYPE": 0x26, "DAC_T2_AMP": 0x27, "DAC_T2_FREQ": 0x29,  # tone 1
    "DAC_OFF_LO":  0x2B, "DAC_OFF_HI": 0x2C,                       # DC offset (u16)
    # shared interlock trip freeze-frame (latched at trip, cleared on reset)
    "SNAP_VALID": 0x30, "SNAP_NOUT": 0x32, "SNAP_SEL": 0x33,
    "SNAP_OUT_PHYS": 0x37, "SNAP_OUT_VAL": 0x38, "SNAP_OUT_LIVE": 0x39,
}
# tone types
T_OFF, T_CONST, T_SINE, T_SQUARE = 0, 1, 2, 3
ADC_IL_INT_BIT = 0x08
WIN = {"khz1": 0, "hz100": 1, "hz10": 2}
# Window fill times at 16 kHz: khz1 (16 samples) 1 ms, hz100 (160) 10 ms, hz10
# (1600) 100 ms. A clean read after a DAC step wants a couple of fresh windows;
# the reg-read overhead dominates the short ones, so just give comfortable margins.
WIN_SETTLE = {0: 0.1, 1: 0.1, 2: 0.4}      # seconds to settle the tumbling window


def find_port():
    ds = libcomm.enumerate_dongles()
    if not ds:
        sys.exit("no register_dongle found")
    return ds[0]["port"]


def commission(unit, settle=9.0, erase=True):
    files = unit.files()
    dg = Dongle(find_port())
    dg.offline()
    if erase:
        dg.file_format()          # discrete clean-slate: wipe the whole store first
    for name, blob in files.items():
        dg.file_put(name, blob)
    dg.close()
    time.sleep(settle)
    return Dongle(find_port()), files


def dac_const(dg):
    # Pure-DC source for the self-test: both DDS tones off, level comes from the
    # DC offset register (the firmware then holds DAC=offset with no ISR).
    dg.reg_write(R["DAC_T1_TYPE"], T_OFF)
    dg.reg_write(R["DAC_T2_TYPE"], T_OFF)


def set_dac(dg, level):
    dg.reg_write(R["DAC_OFF_LO"], level & 0xFF)
    dg.reg_write(R["DAC_OFF_HI"], (level >> 8) & 0xFF)
    dg.reg_write(R["DAC_APPLY"], 1)


def set_tone(dg, tone, ttype, amp=0, freq=0):
    """Configure one DDS tone (0 or 1). Does NOT apply — call dac_apply()."""
    tp = R["DAC_T1_TYPE"] if tone == 0 else R["DAC_T2_TYPE"]
    am = R["DAC_T1_AMP"]  if tone == 0 else R["DAC_T2_AMP"]
    fr = R["DAC_T1_FREQ"] if tone == 0 else R["DAC_T2_FREQ"]
    dg.reg_write(tp, ttype)
    dg.reg_write(am, amp & 0xFF);  dg.reg_write(am + 1, (amp >> 8) & 0xFF)
    dg.reg_write(fr, freq & 0xFF); dg.reg_write(fr + 1, (freq >> 8) & 0xFF)


def set_offset(dg, level):
    dg.reg_write(R["DAC_OFF_LO"], level & 0xFF)
    dg.reg_write(R["DAC_OFF_HI"], (level >> 8) & 0xFF)


def dac_apply(dg):
    dg.reg_write(R["DAC_APPLY"], 1)


def read_avg(dg, win=0):
    dg.reg_write(R["WIN_SEL"], win)
    time.sleep(0.02)
    return dg.reg_read(R["AVG"]) | (dg.reg_read(R["AVG"] + 1) << 8)


def read_stats(dg, win=0):
    """All four stats of the selected window: min, max, avg, rms (AC-rms), in counts."""
    dg.reg_write(R["WIN_SEL"], win)
    time.sleep(0.02)
    def u16(base):
        return dg.reg_read(base) | (dg.reg_read(base + 1) << 8)
    return {"min": u16(0x14), "max": u16(0x16), "avg": u16(0x18), "rms": u16(0x1A)}


def latest(dg):
    return dg.reg_read(R["LATEST"]) | (dg.reg_read(R["LATEST"] + 1) << 8)


def overrun(dg):
    return dg.reg_read(R["OVERRUN"])


def clear_trip(dg):
    dg.reg_write(R["INT_FLAGS"], ADC_IL_INT_BIT)   # un-latch so tripped follows live


def ilstate(dg):
    st = dg.reg_read(R["ILSTATE"])
    return {"tripped": st & 1, "cond_ok": (st >> 1) & 1, "armed": (st >> 2) & 1}


def snapshot(dg):
    """Interlock trip freeze-frame outputs -> {valid, outputs:[{phys,val,live}]}."""
    if not dg.reg_read(R["SNAP_VALID"]):
        return {"valid": 0, "outputs": []}
    outs = []
    for i in range(dg.reg_read(R["SNAP_NOUT"])):
        dg.reg_write(R["SNAP_SEL"], i)
        outs.append({"phys": dg.reg_read(R["SNAP_OUT_PHYS"]),
                     "val":  dg.reg_read(R["SNAP_OUT_VAL"]),
                     "live": dg.reg_read(R["SNAP_OUT_LIVE"])})
    return {"valid": 1, "outputs": outs}


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
