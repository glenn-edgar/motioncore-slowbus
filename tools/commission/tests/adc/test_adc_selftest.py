"""test_adc_selftest.py -- DAC->ADC self-test loop (needs the A0->A1 jumper).

No interlock: commissions a bare ADC unit (the 16 kHz single-channel sampler), sweeps
the DAC DC level, and verifies A1's window average tracks it through the jumper.
Validates the controllable source the rest of the ADC suite relies on, and that no
16 kHz conversions overran. DAC out = level/1023*3.3V; the ADC reads ~ level*4 counts.
"""

from adc_harness import (R, Checker, commission, dac_const, set_dac, read_avg,
                         overrun, WIN, WIN_SETTLE)
import slave_dsl
import time


def run():
    dg, _ = commission(slave_dsl.Unit(0x55, "ADC"))   # no interlock -> null ilcf
    c = Checker()
    c.eq("MODE (2=ADC)", dg.reg_read(R["MODE"]), 2)
    dac_const(dg)
    for level in (128, 384, 640, 896):
        set_dac(dg, level)
        time.sleep(WIN_SETTLE[WIN["hz100"]])
        a = read_avg(dg, WIN["hz100"])                # DC -> stable on any window
        ok = abs(a - level * 4) <= max(40, level * 4 // 25)   # +-3% of level*4
        c.eq("DAC=%d -> A1.avg~%d" % (level, level * 4), ok, True)
    c.eq("no 16 kHz overruns", overrun(dg), 0)
    dg.close()
    return c.done("test_adc_selftest")


if __name__ == "__main__":
    import sys
    sys.exit(0 if run() else 1)
