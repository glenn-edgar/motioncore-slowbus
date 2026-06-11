"""test_gpio_outputs.py -- output values: push-pull drive + open-drain drive-low.

Output VALUES are the one thing writable at runtime (via OLAT). This drives the
outputs and reads them back on the same chip:
  - push-pull output: OLAT bit -> pin reads that level (0 or 1)
  - open-drain output: OLAT bit 0 -> drives low (reads 0); OLAT bit 1 -> Hi-Z
    (released; reads float -- the wired-OR/jumper case is test_gpio_wired_or.py)

No interlock here so OLAT isn't overridden.
"""

from gpio_harness import R, Checker, commission, bit
import slave_dsl


def build():
    u = slave_dsl.Unit(0x20, "GPIO")
    u.pins(D0="in:none", D1="in:none", D2="in:none", D3="in:none",
           D7="out:0", D8="out:1", D9="out:od", D10="out:0")
    return u


def run():
    dg, _ = commission(build())
    c = Checker()

    # Push-pull: toggle D7 (out) and read it back.
    olat = dg.reg_read(R["OLAT"])
    dg.reg_write(R["OLAT"], olat | (1 << bit("D7")))      # D7 high
    c.eq("D7 driven high", (dg.reg_read(R["GPIO"]) >> bit("D7")) & 1, 1)
    dg.reg_write(R["OLAT"], dg.reg_read(R["OLAT"]) & ~(1 << bit("D7")))  # D7 low
    c.eq("D7 driven low",  (dg.reg_read(R["GPIO"]) >> bit("D7")) & 1, 0)

    # Open-drain D9: value 0 -> actively drives low -> reads 0 (deterministic).
    dg.reg_write(R["OLAT"], dg.reg_read(R["OLAT"]) & ~(1 << bit("D9")))
    c.eq("D9 od drive-low", (dg.reg_read(R["GPIO"]) >> bit("D9")) & 1, 0)
    # value 1 -> Hi-Z (released); reading its own pin is indeterminate without an
    # external pull, so we only assert the drive-low case here. Wired-OR with a
    # pull-up is covered, with a jumper, in test_gpio_wired_or.py.

    dg.close()
    return c.done("test_gpio_outputs")


if __name__ == "__main__":
    import sys
    sys.exit(0 if run() else 1)
