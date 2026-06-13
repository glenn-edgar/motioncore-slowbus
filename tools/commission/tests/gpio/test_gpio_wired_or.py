"""test_gpio_wired_or.py -- open-drain wired-OR (needs a MANUAL jumper).

This is the point of open-drain outputs: several writers share one line, any of
them pulling it low. Here one chip plays both writer and reader:

    D8 = oc  (the open-drain writer)
    D0 = in:up   (reads the shared line; its pull-up is the line's pull-up)

Jumper D8 <-> D0, then:
    D8 released (OLAT bit = 1) -> nobody drives -> pull-up wins -> D0 reads 1
    D8 driven low (OLAT bit = 0) -> D8 pulls the line -> D0 reads 0

On a real bus the pull-up is external and the writers are different chips; the
behaviour is identical.
"""

from gpio_harness import R, Checker, commission, bit
import slave_dsl


def build():
    u = slave_dsl.Unit(0x20, "GPIO")
    u.pins(D0="in:up", D1="in:none", D2="in:none", D3="in:none",
           D7="out:0", D8="oc", D9="out:0", D10="out:0")
    return u


def run():
    dg, _ = commission(build())
    try:
        input("\n>>> Jumper D8 to D0, then press Enter (Ctrl-C to skip)... ")
    except (EOFError, KeyboardInterrupt):
        print("skipped (no jumper)")
        dg.close()
        return True
    c = Checker()
    b8 = bit("D8")
    # D8 released -> D0's pull-up holds the line high.
    dg.reg_write(R["OLAT"], dg.reg_read(R["OLAT"]) | (1 << b8))
    c.eq("D8 released -> D0 = 1", dg.reg_read(R["GPIO"]) & 1, 1)
    # D8 drives low -> shared line low.
    dg.reg_write(R["OLAT"], dg.reg_read(R["OLAT"]) & ~(1 << b8))
    c.eq("D8 low -> D0 = 0", dg.reg_read(R["GPIO"]) & 1, 0)
    dg.close()
    return c.done("test_gpio_wired_or")


if __name__ == "__main__":
    import sys
    sys.exit(0 if run() else 1)
