"""servo_harness.py -- shared helpers for the SAMD21 SERVO-mode serial tests.

SERVO mode drives up to 8 RC servos on a software 50 Hz frame (TC4). Pads are
configured by the `srvo` file: each non-D6 bank pad (D0,D1,D2,D3,D7,D8,D9,D10) is a
`servo` channel or a bench role (gpio-in/out/adc/dac). D6 is ALWAYS the e-stop
interlock -- an input with pull-up, active-low: an external open-drain node pulling
the shared line low stops all servos (outputs go limp) and LATCHES the stop. The
servos resume only when D6 is high again AND a START (CTRL=1) is commanded.

Registers (mode bank): CH_SEL/WIDTH/ENABLE as before, plus CTRL (1=start, 0=stop)
and STATE (bit0 running, bit1 e-stop latched, bit2 e-stop line low now). The spare
pads use the shared bench at 0x15-0x1E (same as COUNTER).
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
    "CH_SEL":    0x10, "WIDTH_L": 0x11, "WIDTH_H": 0x12,
    "ENABLE_L":  0x13, "ENABLE_H": 0x14,
    "CTRL":      0x1A,   # w: 1=start, 0=stop
    "STATE":     0x1F,   # r: bit0 run, bit1 latched, bit2 estop-low
    # shared bench tools on the spare pads
    "BENCH_SEL": 0x15, "BENCH_GPO": 0x16, "BENCH_GPI": 0x17,
    "BENCH_ADCRQ": 0x18, "BENCH_ADCST": 0x19, "BENCH_ADCV": 0x1B,
    "BENCH_STAT": 0x1D, "BENCH_ROLE": 0x1E,
}

# STATE bits / bench codes / role codes (mirror the firmware enums).
ST_RUN, ST_LATCHED, ST_ESTOP = 0x01, 0x02, 0x04
BENCH_OK, BENCH_BAD_PIN, BENCH_WRONG_ROLE, BENCH_UNSUPPORTED = 0, 1, 2, 3
ROLE_NONE, ROLE_IN, ROLE_OUT, ROLE_ADC, ROLE_DAC = 0, 1, 2, 3, 4

BENCH_IDX = {n: i for i, n in enumerate(
    ('D0', 'D1', 'D2', 'D3', 'D4', 'D5', 'D6', 'D7', 'D8', 'D9', 'D10'))}
BENCH_IDX.update({'A0': 0, 'A1': 1, 'A2': 2, 'A3': 3, 'A6': 6,
                  'A7': 7, 'A8': 8, 'A9': 9, 'A10': 10})


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


# ---- servo control -------------------------------------------------------------
def servo_enable(dg, mask):
    """Set the enable mask (CH0..CH7); returns the (declared-gated) readback."""
    dg.reg_write(R["ENABLE_L"], mask & 0xFF)
    return dg.reg_read(R["ENABLE_L"])


def servo_width(dg, ch, us):
    """Set channel ch pulse width (us); returns the (clamped) readback width."""
    dg.reg_write(R["CH_SEL"], ch)
    dg.reg_write(R["WIDTH_L"], us & 0xFF)
    dg.reg_write(R["WIDTH_H"], (us >> 8) & 0xFF)         # WIDTH_H write latches
    return dg.reg_read(R["WIDTH_L"]) | (dg.reg_read(R["WIDTH_H"]) << 8)


def servo_ctrl(dg, run):
    dg.reg_write(R["CTRL"], 1 if run else 0)


def servo_state(dg):
    s = dg.reg_read(R["STATE"])
    return {"run": bool(s & ST_RUN), "latched": bool(s & ST_LATCHED),
            "estop": bool(s & ST_ESTOP), "raw": s}


# ---- shared bench (subset used by the servo tests) -----------------------------
def bench_select(dg, name):
    dg.reg_write(R["BENCH_SEL"], BENCH_IDX[name])
    return dg.reg_read(R["BENCH_STAT"])


def bench_role(dg, name):
    dg.reg_write(R["BENCH_SEL"], BENCH_IDX[name])
    return dg.reg_read(R["BENCH_ROLE"])


def bench_gpo(dg, name, level):
    dg.reg_write(R["BENCH_SEL"], BENCH_IDX[name])
    dg.reg_write(R["BENCH_GPO"], 1 if level else 0)
    return dg.reg_read(R["BENCH_STAT"])


def bench_gpi(dg, name):
    dg.reg_write(R["BENCH_SEL"], BENCH_IDX[name])
    v = dg.reg_read(R["BENCH_GPI"])
    return v, dg.reg_read(R["BENCH_STAT"])


class Checker:
    def __init__(self):
        self.fails = []
        self.n = 0

    def eq(self, name, got, exp):
        self.n += 1
        ok = got == exp
        print("    %-32s = %-7s expect %-7s %s" % (name, got, exp, "OK" if ok else "FAIL"))
        if not ok:
            self.fails.append(name)
        return ok

    def done(self, title):
        bad = len(self.fails)
        print("  %s: %s  (%d/%d)" % (title, "PASS" if not bad else "FAIL", self.n - bad, self.n))
        return not bad
