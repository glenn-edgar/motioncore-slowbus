"""test_mixed_interlock.py -- MIXED ADC+GPIO interlock (needs the A0->A1 jumper).

MIXED runs one interlock mixing an ADC threshold and a (debounced) GPIO level in
a single expression. We drive the ADC half with the DAC (A0->A1) and the GPIO
half with the input's commissioned pull, and check cond-ok follows the combined
DNF. Three things under test:

  1. AND logic + ADC sweep: `A1 > 2.0V && D8` with D8 pulled up (=1) -> cond-ok
     tracks A1 across 2.0 V.
  2. GPIO veto: same interlock re-commissioned with D8 pulled down (=0) -> the AND
     fails regardless of A1 (the GPIO half vetoes).
  3. OR groups + debounce: `A1 > 2.5V || D8` with D8 declared `debounce_50ms`;
     the debounced level (= the pull) is the OR's GPIO term, and we confirm the
     debounced bitmap matches the pull.

The DAC self-test (A1 tracks the DAC) is implicit in the A1 readings printed.
A1 ~ level*4 counts; threshold 2.0 V = ~2482, 2.5 V = ~3102.
"""

from mixed_harness import (R, Checker, commission, dac_dc, ilstate, clear_trip,
                           adc_val, gpio_deb)
import slave_dsl
import time

SETTLE = 0.3   # MIXED samples single-shot at ~100 Hz; one DAC step settles fast


def _condok(dg):
    clear_trip(dg)                 # un-latch so cond-ok reflects the live comparison
    time.sleep(0.1)
    return ilstate(dg)["cond_ok"]


def run():
    c = Checker()

    # 1. AND + ADC sweep, D8 pulled up (=1) -> cond-ok follows A1 > 2.0V
    u = slave_dsl.Unit(0x55, "MIXED")
    u.pins(A1="adc", D8="in:up", D6="out")
    u.interlock("s", when="A1 > 2.0V && D8", drive={"D6": 1})
    dg, _ = commission(u)
    c.eq("MODE (3=MIXED)", dg.reg_read(R["MODE"]), 3)
    c.eq("ILSTAT armed", dg.reg_read(R["ILSTAT"]), 0)
    print("  A1 > 2.0V && D8(up=1):")
    for level, exp in [(256, 0), (768, 1), (256, 0), (900, 1)]:
        dac_dc(dg, level)
        time.sleep(SETTLE)
        ok = _condok(dg)
        print("    DAC=%-3d A1=%-4d cond-ok=%d" % (level, adc_val(dg, 0), ok))
        c.eq("DAC=%d" % level, ok, exp)
    dg.close()

    # 2. GPIO veto: D8 pulled down (=0) -> AND fails even with A1 high
    u = slave_dsl.Unit(0x55, "MIXED")
    u.pins(A1="adc", D8="in:down", D6="out")
    u.interlock("s", when="A1 > 2.0V && D8", drive={"D6": 1})
    dg, _ = commission(u)
    print("  A1 > 2.0V && D8(down=0)  -> GPIO vetoes:")
    dac_dc(dg, 900)                # A1 well above 2.0 V
    time.sleep(SETTLE)
    c.eq("D8=0 vetoes (A1 high)", _condok(dg), 0)
    c.eq("debounced D8 = 0", gpio_deb(dg) & 0b10, 0)   # input1 bit = 0
    dg.close()

    # 3. OR groups + debounce: `A1 > 2.5V || D8`, D8 debounce_50ms pulled up
    u = slave_dsl.Unit(0x55, "MIXED")
    u.pins(A1="adc", D8="in:up:debounce_50ms", D6="out")
    u.interlock("s", when="A1 > 2.5V || D8", drive={"D6": 1})
    dg, _ = commission(u)
    print("  A1 > 2.5V || D8(up, debounce 50ms):")
    c.eq("ILSTAT armed (debounce)", dg.reg_read(R["ILSTAT"]), 0)
    c.eq("debounced D8 = 1", (gpio_deb(dg) >> 1) & 1, 1)
    # D8=1 satisfies the OR no matter what A1 is -> cond-ok stays 1 even with A1 low
    dac_dc(dg, 128)
    time.sleep(SETTLE)
    c.eq("D8=1 holds OR (A1 low)", _condok(dg), 1)
    dg.close()

    return c.done("test_mixed_interlock")


if __name__ == "__main__":
    import sys
    sys.exit(0 if run() else 1)
