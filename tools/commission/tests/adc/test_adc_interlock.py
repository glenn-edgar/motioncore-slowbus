"""test_adc_interlock.py -- ADC interlock over streams (needs the A0->A1 jumper).

Commissions single-channel (A1) ADC-mode interlocks and sweeps the DAC to move A1
across the threshold, checking cond-ok (the live comparison) flips where it should.
Covers the stream selector (.stat.window with the khz1/hz100/hz10 downsample windows
+ default instantaneous) and DNF (AND within a group, OR across groups). The trip is
cleared each step so cond-ok follows live.
"""

from adc_harness import (R, Checker, commission, dac_const, set_dac,
                         clear_trip, ilstate, WIN, WIN_SETTLE)
import slave_dsl
import time


def sweep(expr, cases, settle):
    """Commission `expr` (drive D6), then for each (dac_level, expect_cond_ok)
    verify cond-ok. Returns True if all match."""
    u = slave_dsl.Unit(0x55, "ADC")
    u.interlock("t", expr, drive={"D6": 1})
    dg, _ = commission(u)
    dac_const(dg)
    c = Checker()
    c.eq("ILSTAT armed", dg.reg_read(R["ILSTAT"]), 0)
    for level, exp in cases:
        set_dac(dg, level)
        time.sleep(settle)
        clear_trip(dg)
        time.sleep(0.05)
        c.eq("DAC=%d cond-ok" % level, ilstate(dg)["cond_ok"], exp)
    dg.close()
    return c.done(expr)


def run():
    w = WIN_SETTLE[WIN["hz100"]]
    ok = True
    print("instantaneous (default stat):")
    ok &= sweep("A1 > 2.0V", [(256, 0), (768, 1)], 0.15)
    print("windowed average (avg.hz100):")
    ok &= sweep("A1.avg.hz100 > 2.0V", [(256, 0), (768, 1)], w)
    print("window max (max.hz100):")
    ok &= sweep("A1.max.hz100 > 2.0V", [(256, 0), (768, 1)], w)
    print("AND band (one stream, two clauses, one group):")
    ok &= sweep("A1.avg.hz100 > 1.0V && A1.avg.hz100 < 3.0V", [(128, 0), (512, 1), (960, 0)], w)
    print("OR (two DNF groups):")
    ok &= sweep("A1.avg.hz100 < 1.0V || A1.avg.hz100 > 3.0V", [(128, 1), (512, 0), (960, 1)], w)
    return ok


if __name__ == "__main__":
    import sys
    sys.exit(0 if run() else 1)
