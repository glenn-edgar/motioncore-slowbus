"""test_mixed_oc.py -- MIXED interlock with an OPEN-COLLECTOR output (D6).

D6 is commissioned `oc`: the interlock releases it (Hi-Z) while safe and pulls it
LOW on trip (drive={'D6':1} -> ok=1=release, err=0=pull-low). D6 keeps INEN, so the
trip freeze-frame's OUT_LIVE reads the pin back directly -- no jumper:
  - trip  -> D6 driven low  -> OUT_LIVE = 0
  - the logical safe value (OUT_VAL) is 0 too.

Needs the A0->A1 jumper (DAC drives the ADC half so we can force the trip).
"""

from mixed_harness import (R, Checker, commission, dac_dc, ilstate, clear_trip,
                           int_flags, snapshot, MIXED_INT_BIT)
import slave_dsl
import time

SETTLE = 0.3


def run():
    c = Checker()
    u = slave_dsl.Unit(0x55, "MIXED")
    u.pins(A1="adc", D8="in:up", D6="oc")
    u.interlock("s", when="A1 > 2.0V && D8", drive={"D6": 1})    # safe=release, trip=pull-low
    dg, _ = commission(u)
    c.eq("MODE (3=MIXED)", dg.reg_read(R["MODE"]), 3)

    # OK baseline: A1 high, clear any boot trip.
    dac_dc(dg, 900)
    time.sleep(SETTLE)
    clear_trip(dg)
    time.sleep(SETTLE)
    c.eq("armed: not tripped", ilstate(dg)["tripped"], 0)

    # Trip: A1 below 2.0 V -> D6 pulled low (oc asserted).
    dac_dc(dg, 128)
    time.sleep(SETTLE)
    c.eq("A1 low -> tripped", ilstate(dg)["tripped"], 1)
    c.eq("INT_FLAGS MIXED bit set", int_flags(dg) & MIXED_INT_BIT, MIXED_INT_BIT)

    snap = snapshot(dg)
    print("    snapshot:", snap)
    out = snap["outputs"][0]
    c.eq("output is D6 (phys 40)", out["phys"], 40)            # PB08 = (1<<5)|8 = 40
    c.eq("safe value (logical) = 0", out["val"], 0)
    c.eq("D6 read-back LOW on trip (OUT_LIVE=0)", out["live"], 0)   # <-- the read-back

    # Clear the fault + W1C reset -> re-arm (D6 released), snapshot cleared.
    dac_dc(dg, 900)
    time.sleep(SETTLE)
    clear_trip(dg)
    time.sleep(SETTLE)
    c.eq("after reset: not tripped", ilstate(dg)["tripped"], 0)
    c.eq("after reset: snapshot cleared", snapshot(dg)["valid"], 0)

    dg.close()
    return c.done("test_mixed_oc")


if __name__ == "__main__":
    import sys
    sys.exit(0 if run() else 1)
