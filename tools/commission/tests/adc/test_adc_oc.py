"""test_adc_oc.py -- ADC-mode interlock with an OPEN-COLLECTOR output (D6).

The ADC interlock watches the A1 stream; D6 is commissioned `oc` -- released while
safe, pulled LOW on trip (drive={'D6':1} -> ok=1=release, err=0=pull-low). D6 keeps
INEN, so the trip freeze-frame's OUT_LIVE reads it back: trip -> D6 low -> OUT_LIVE 0.

Needs the A0->A1 jumper (the DAC is the controllable ADC source).
"""

from adc_harness import (R, Checker, commission, set_dac, ilstate, clear_trip,
                         snapshot, ADC_IL_INT_BIT)
import slave_dsl
import time

SETTLE = 0.4   # ADC interlock samples instantaneous; a DAC step propagates fast


def run():
    c = Checker()
    u = slave_dsl.Unit(0x55, "ADC")
    u.pins(D6="oc")
    u.interlock("s", when="A1 > 2.5V", drive={"D6": 1})          # safe=release, trip=pull-low
    dg, _ = commission(u)
    c.eq("MODE (2=ADC)", dg.reg_read(R["MODE"]), 2)
    c.eq("cfg emits (D6):oc", "(D6):oc" in _.get("ilcf", b"").decode("ascii", "ignore"), True)

    # OK baseline: A1 high, clear any boot trip.
    set_dac(dg, 900)
    time.sleep(SETTLE)
    clear_trip(dg)
    time.sleep(SETTLE)
    c.eq("armed: not tripped", ilstate(dg)["tripped"], 0)

    # Trip: A1 below 2.5 V -> D6 pulled low.
    set_dac(dg, 80)
    time.sleep(SETTLE)
    c.eq("A1 low -> tripped", ilstate(dg)["tripped"], 1)
    c.eq("INT_FLAGS ADC bit set", dg.reg_read(R["INT_FLAGS"]) & ADC_IL_INT_BIT, ADC_IL_INT_BIT)

    snap = snapshot(dg)
    print("    snapshot:", snap)
    out = snap["outputs"][0]
    c.eq("output is D6 (phys 40)", out["phys"], 40)
    c.eq("safe value (logical) = 0", out["val"], 0)
    c.eq("D6 read-back LOW on trip (OUT_LIVE=0)", out["live"], 0)

    # Clear fault + reset -> re-arm, snapshot cleared.
    set_dac(dg, 900)
    time.sleep(SETTLE)
    clear_trip(dg)
    time.sleep(SETTLE)
    c.eq("after reset: not tripped", ilstate(dg)["tripped"], 0)
    c.eq("after reset: snapshot cleared", snapshot(dg)["valid"], 0)

    dg.close()
    return c.done("test_adc_oc")


if __name__ == "__main__":
    import sys
    sys.exit(0 if run() else 1)
