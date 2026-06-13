"""counter_harness.py -- shared helpers for the SAMD21 COUNTER-mode serial tests.

Worked examples of commissioning + exercising a COUNTER config-chip. The chip's
counters are commissioned via a `cntr` file (which pads count, their pull + edge,
and a bank-global update rate); the Pico reads ALL counters in one transaction
(READ / READ_CLR -> stream the 36-byte shadow). Undeclared pads are free for the
bench tools. Each spare (non-counter) pad carries a fixed bench ROLE declared in
the `cntr` file: `dac` (square/DC stimulus, D0/A0 only), `adc` (16x oneshot),
`out` (gpio write), `in` (gpio read). The bench is addressed by Seeed pad name
(D0..D10 == A0..A10) and validated against the pad's role; mismatches report via
the BENCH_STAT register. The DAC stimulus now requires D0/A0 commissioned as `dac`
(it no longer auto-enables). A1 = AIN4 = D1, so the usual A0->A1 jumper is A0->D1.

NOTE: the board's timer clock is now correct (the "9x slow clock" was a watchdog-
pet APB stall, since fixed), so absolute rates are reliable. The counting-logic
assertions remain frequency-independent for robustness, but rate checks are valid.
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
    # bench tools on the spare pads (request/get; role-validated)
    "BENCH_SEL":   0x15,  # w/r: Seeed pad index 0..10
    "BENCH_GPO":   0x16,  # w:   drive 0/1 (role out)
    "BENCH_GPI":   0x17,  # r:   level 0/1 (role in)
    "BENCH_ADCRQ": 0x18,  # w:   request 16x oneshot (role adc)
    "BENCH_ADCST": 0x19,  # r:   0 ready, 1 pending
    "BENCH_ADCV":  0x1B,  # r:   u16 result (0x1B lo, 0x1C hi)
    "BENCH_STAT":  0x1D,  # r:   last cmd status (see BENCH_*)
    "BENCH_ROLE":  0x1E,  # r:   selected pad role (see ROLE_*)
    "DAC_T1_TYPE": 0x20, "DAC_T1_AMP": 0x21, "DAC_T1_FREQ": 0x23,
    "DAC_APPLY":   0x25, "DAC_T2_TYPE": 0x26,
    "DAC_OFF_LO":  0x2B, "DAC_OFF_HI": 0x2C,
}
T_OFF, T_SQUARE = 0, 3

# BENCH_STAT codes / BENCH_ROLE codes (mirror the firmware enums).
BENCH_OK, BENCH_BAD_PIN, BENCH_WRONG_ROLE, BENCH_UNSUPPORTED = 0, 1, 2, 3
ROLE_NONE, ROLE_IN, ROLE_OUT, ROLE_ADC, ROLE_DAC = 0, 1, 2, 3, 4

# Seeed pad name -> bench index (D0..D10; A_n aliases D_n).
BENCH_IDX = {n: i for i, n in enumerate(
    ('D0', 'D1', 'D2', 'D3', 'D4', 'D5', 'D6', 'D7', 'D8', 'D9', 'D10'))}
BENCH_IDX.update({'A0': 0, 'A1': 1, 'A2': 2, 'A3': 3, 'A6': 6,
                  'A7': 7, 'A8': 8, 'A9': 9, 'A10': 10})


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


# ---- COUNTER bench tools (spare-pad ADC/GPIO; request/get, role-validated) -----
def bench_select(dg, name):
    """Select a bench pad by Seeed name; returns BENCH_STAT (0 OK, 1 BAD_PIN)."""
    dg.reg_write(R["BENCH_SEL"], BENCH_IDX[name])
    return dg.reg_read(R["BENCH_STAT"])


def bench_role(dg, name):
    """Selected pad's commissioned role (ROLE_*)."""
    dg.reg_write(R["BENCH_SEL"], BENCH_IDX[name])
    return dg.reg_read(R["BENCH_ROLE"])


def bench_gpo(dg, name, level):
    """Drive a gpio-out pad; returns BENCH_STAT (OK / WRONG_ROLE / BAD_PIN)."""
    dg.reg_write(R["BENCH_SEL"], BENCH_IDX[name])
    dg.reg_write(R["BENCH_GPO"], 1 if level else 0)
    return dg.reg_read(R["BENCH_STAT"])


def bench_gpi(dg, name):
    """Read a gpio-in pad; returns (level, BENCH_STAT)."""
    dg.reg_write(R["BENCH_SEL"], BENCH_IDX[name])
    v = dg.reg_read(R["BENCH_GPI"])
    return v, dg.reg_read(R["BENCH_STAT"])


def bench_adc(dg, name, timeout=0.3):
    """Request + poll + get a 16x ADC oneshot on a bench pad. Returns (value, stat)."""
    dg.reg_write(R["BENCH_SEL"], BENCH_IDX[name])
    dg.reg_write(R["BENCH_ADCRQ"], 1)
    t0 = time.time()
    while dg.reg_read(R["BENCH_ADCST"]) and (time.time() - t0) < timeout:
        time.sleep(0.005)
    lo = dg.reg_read(R["BENCH_ADCV"])
    hi = dg.reg_read(R["BENCH_ADCV"] + 1)
    return lo | (hi << 8), dg.reg_read(R["BENCH_STAT"])


def bench_unsupported(dg):
    """Poke an unknown bench register -> expect BENCH_UNSUPPORTED."""
    dg.reg_write(0x1F, 0)        # 0x1F: not a bench reg, below the DAC sub-bank
    return dg.reg_read(R["BENCH_STAT"])


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
