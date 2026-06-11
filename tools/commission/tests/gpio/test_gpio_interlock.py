"""test_gpio_interlock.py -- the GPIO interlock: arm, cond-ok, trip, manual reset.

Commissions a device whose interlock condition is driven by the pins' own pulls,
so it's deterministic without jumpers:

    interlock 'safe' : (D0 && D2)        -> drive D8
    D0 = in:up (1), D2 = in:up (1)  -> condition TRUE  -> armed, cond-ok, no trip

Then forces the condition false by re-commissioning D0 as a pull-down (the board
can't change at runtime, so we go through the DSL + reboot, exactly as you would
on real hardware) and confirms it trips and asserts INT_FLAGS.
"""

from gpio_harness import R, Checker, commission
import slave_dsl

INT_FLAGS = 0x04          # control-bank INT_FLAGS register (bit0 = PIO interlock trip; W1C)


def build(d0_pull):
    u = slave_dsl.Unit(0x20, "GPIO")
    u.pins(D0="in:%s" % d0_pull, D1="in:none", D2="in:up", D3="in:none",
           D7="out:0", D8="out:0", D9="out:0", D10="out:0")
    u.interlock("safe", "D0 && D2", drive={"D8": 1})
    return u


def run():
    c = Checker()

    # 1) D0 pulled up -> D0 && D2 true -> armed, cond-ok, not tripped.
    dg, _ = commission(build("up"))
    c.eq("ILSTAT parse OK", dg.reg_read(R["ILSTAT"]), 0)
    st = dg.reg_read(R["ILSTATE"])
    c.eq("armed",   (st >> 2) & 1, 1)
    c.eq("cond-ok", (st >> 1) & 1, 1)
    c.eq("not tripped", st & 1, 0)
    dg.close()

    # 2) Re-commission D0 pulled DOWN -> condition false -> trips on boot.
    dg, _ = commission(build("down"))
    st = dg.reg_read(R["ILSTATE"])
    c.eq("tripped", st & 1, 1)
    c.eq("cond-ok clear", (st >> 1) & 1, 0)
    c.eq("INT_FLAGS bit0", dg.reg_read(INT_FLAGS) & 1, 1)
    dg.close()

    return c.done("test_gpio_interlock")


if __name__ == "__main__":
    import sys
    sys.exit(0 if run() else 1)
