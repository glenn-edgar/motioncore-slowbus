"""test_mixed_trip.py -- MIXED interlock TRIP path: D6 drive + INT_FLAGS + W1C
re-arm + the trip freeze-frame (latched inputs/output, cleared on reset).

Needs the A0->A1 jumper (DAC drives the ADC half). The interlock is armed safe
while `A1 > 2.0V && D8` holds; dropping A1 below 2.0 V violates it -> TRIP, which:
  - latches ILSTATE.tripped and sets the MIXED bit in INT_FLAGS,
  - drives the trip value onto the output (drive={'D6':1} means D6=1 while OK, so
    the trip/err value driven is 0),
  - freezes a snapshot of the inputs (A1 value, D8 level) + the output drive.
Clearing INT_FLAGS (W1C) with the fault gone re-arms and clears the snapshot.
"""

from mixed_harness import (R, Checker, commission, dac_dc, ilstate, clear_trip,
                           int_flags, snapshot, MIXED_INT_BIT)
import slave_dsl
import time

SETTLE = 0.3   # > one 100 Hz MIXED tick, lets the DAC step propagate + evaluate


def run():
    c = Checker()
    u = slave_dsl.Unit(0x55, "MIXED")
    u.pins(A1="adc", D8="in:up", D6="out")
    u.interlock("s", when="A1 > 2.0V && D8", drive={"D6": 1})   # safe while A1>2V & D8=1
    dg, _ = commission(u)
    c.eq("MODE (3=MIXED)", dg.reg_read(R["MODE"]), 3)

    # Establish the OK baseline: A1 high (>2V), clear any boot trip.
    dac_dc(dg, 900)
    time.sleep(SETTLE)
    clear_trip(dg)
    time.sleep(SETTLE)
    c.eq("armed: not tripped", ilstate(dg)["tripped"], 0)
    c.eq("armed: snapshot empty", snapshot(dg)["valid"], 0)

    # Violate the condition: drop A1 below 2.0 V -> TRIP.
    dac_dc(dg, 128)
    time.sleep(SETTLE)
    st = ilstate(dg)
    c.eq("A1 low -> tripped", st["tripped"], 1)
    c.eq("INT_FLAGS MIXED bit set", int_flags(dg) & MIXED_INT_BIT, MIXED_INT_BIT)

    # Freeze-frame: inputs (A1 low, D8=1) + the safe output drive (D6 = 1).
    snap = snapshot(dg)
    print("    snapshot:", snap)
    c.eq("snapshot valid", snap["valid"], 1)
    c.eq("snapshot has 2 inputs", len(snap["inputs"]), 2)
    c.eq("snapshot has 1 output", len(snap["outputs"]), 1)
    adc_in = [i for i in snap["inputs"] if i["role"] == 2]
    gpio_in = [i for i in snap["inputs"] if i["role"] == 1]
    c.eq("one ADC + one GPIO input", (len(adc_in), len(gpio_in)), (1, 1))
    c.eq("latched A1 low (<800)", adc_in[0]["val"] < 800, True)
    c.eq("latched D8 = 1", gpio_in[0]["val"], 1)
    # drive={'D6':1} = "D6=1 while OK" -> the value driven on TRIP (err) is 0.
    c.eq("latched D6 trip-drive = 0", snap["outputs"][0]["val"], 0)

    # Re-trip is idempotent: the snapshot holds the FIRST trip, not the latest.
    dac_dc(dg, 100)
    time.sleep(SETTLE)
    c.eq("snapshot still valid (held)", snapshot(dg)["valid"], 1)

    # Clear the fault, then W1C-reset -> re-arm + snapshot cleared.
    dac_dc(dg, 900)
    time.sleep(SETTLE)
    clear_trip(dg)
    time.sleep(SETTLE)
    c.eq("after reset: not tripped", ilstate(dg)["tripped"], 0)
    c.eq("after reset: INT bit clear", int_flags(dg) & MIXED_INT_BIT, 0)
    c.eq("after reset: snapshot cleared", snapshot(dg)["valid"], 0)

    dg.close()
    return c.done("test_mixed_trip")


if __name__ == "__main__":
    import sys
    sys.exit(0 if run() else 1)
