"""test_gpio_pinmap.py -- commission a GPIO unit and verify the gpmp pin-map.

Checks every pin role applies at boot: direction, pull up/down/none, output
init, open-drain, and that DIR/PULL/OD are read-only at runtime (the board is
fixed -- only output values move). No jumpers needed; pulls make the reads
deterministic.

Example device: 4 inputs (up/down/up/none) + 4 outputs (low/high/od/od).
"""

from gpio_harness import R, Checker, commission, bit
import slave_dsl


def build():
    u = slave_dsl.Unit(0x20, "GPIO")
    u.pins(D0="in:up", D1="in:down", D2="in:up", D3="in:none",
           D7="out:0", D8="out:1", D9="out:od", D10="out:od")
    return u


def run():
    u = build()
    print("gpmp:", list(u.files()["gpmp"]))      # [2, 0xF0, 0x07, 0xE5, 0xC0, 0x00]
    dg, _ = commission(u)
    c = Checker()
    c.eq("MODE (1=GPIO)", dg.reg_read(R["MODE"]), 1)
    c.eq("IODIR",         dg.reg_read(R["IODIR"]), 0x0F)   # D0-D3 in, D7-D10 out
    c.eq("GPPU (up)",     dg.reg_read(R["GPPU"]),  0x05)   # D0, D2
    c.eq("GPPD (down)",   dg.reg_read(R["GPPD"]),  0x02)   # D1
    c.eq("OD",            dg.reg_read(R["OD"]),    0xC0)   # D9, D10
    c.eq("OLAT",          dg.reg_read(R["OLAT"]),  0xE5)
    # Pulled-input reads are deterministic -- assert those. Output-pin reads are
    # load-dependent (a wired output reads the BUS, not its own drive), so report
    # them as info only; driven-output read-back is in test_gpio_outputs.py.
    g = dg.reg_read(R["GPIO"])
    c.eq("D0 pull-up reads 1",   (g >> 0) & 1, 1)
    c.eq("D1 pull-down reads 0", (g >> 1) & 1, 0)
    c.eq("D2 pull-up reads 1",   (g >> 2) & 1, 1)
    print("  (info) output reads D7,D8,D9,D10 = %s  (load-dependent)"
          % [(g >> bit(p)) & 1 for p in ("D7", "D8", "D9", "D10")])

    # Static config is read-only at runtime: writes must be ignored.
    for reg in ("IODIR", "GPPU", "GPPD", "OD"):
        before = dg.reg_read(R[reg])
        dg.reg_write(R[reg], before ^ 0xFF)
        c.eq(reg + " read-only", dg.reg_read(R[reg]), before)

    dg.close()
    return c.done("test_gpio_pinmap")


if __name__ == "__main__":
    import sys
    sys.exit(0 if run() else 1)
