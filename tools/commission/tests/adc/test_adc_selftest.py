"""test_adc_selftest.py -- DAC->ADC self-test loop (needs the A0->A1 jumper).

No interlock: commissions a bare ADC unit (just the sampler), sweeps the DAC, and
verifies A1's window average tracks it through the jumper. This validates the
controllable source the interlock tests rely on. DAC out = level/1023*3.3V; the
ADC reads ~ level*4 counts.
"""

from adc_harness import R, Checker, commission, dac_const, set_dac, read_avg, WIN
import slave_dsl
import time


def run():
    dg, _ = commission(slave_dsl.Unit(0x55, "ADC"))   # no interlock -> null ilcf
    c = Checker()
    c.eq("MODE (2=ADC)", dg.reg_read(R["MODE"]), 2)
    dac_const(dg)
    for level in (128, 384, 640, 896):
        set_dac(dg, level)
        time.sleep(2.0)          # fast window ~0.8 s at ~125 Hz/ch; ~2 windows to settle
        a = read_avg(dg, ch=0, win=WIN["fast"])
        # within +-3% of level*4 (ADC vs 10-bit DAC, both ref 3.3V)
        ok = abs(a - level * 4) <= max(40, level * 4 // 25)
        c.eq("DAC=%d -> A1.avg~%d" % (level, level * 4), ok, True)
    dg.close()
    return c.done("test_adc_selftest")


if __name__ == "__main__":
    import sys
    sys.exit(0 if run() else 1)
