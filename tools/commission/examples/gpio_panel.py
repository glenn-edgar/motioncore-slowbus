#!/usr/bin/env python3
"""gpio_panel.py -- worked example: a GPIO device in the config namespace.

Shows the full GPIO authoring flow -- the balanced define_node/end_node namespace
(config_tree) plus a device() whose pin/interlock spec compiles to the on-chip
files (idnt + gpmp + ilcf) via slave_dsl. Run it on the dev box (the vendored KB
+ ltree are in tools/commission/vendor/):

    python3 examples/gpio_panel.py

A "control panel" RS-485 slave with one GPIO config-chip exercising every pin
role: pulled inputs (up/down/none), push-pull outputs (0/1), an open-drain output
on a shared fault line, and an interlock that drops the enable when a guard opens.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import config_tree as C


def panel_io(b):
    """Composable I2C-bus subtree: one GPIO panel controller.

    D0  estop button   in:up   (pulled high; pressed = low)
    D1  guard switch   in:up
    D2  mode select    in:down
    D3  spare input    in:none
    D7  run lamp       out:0   (push-pull)
    D8  enable relay   out:0   (push-pull; the interlock drops this)
    D9  aux output     out:1
    D10 fault line     oc  (open-drain, wire-OR'd across panels)
    """
    b.device("panel", i2c=0x20, cls="SAMD21", sub="GPIO",
             pins={"D0": "in:up",  "D1": "in:up",  "D2": "in:down", "D3": "in:none",
                   "D7": "out:0",  "D8": "out:0",  "D9": "out:1",   "D10": "oc"},
             # enable D8 only while estop released AND guard closed (both read 1)
             interlock={"name": "guard", "expr": "D0 && D1", "drive": {"D8": 1}})


def main():
    b = C.Builder("/tmp/gpio_panel.db")
    b.define_node("SITE", "acme")
    b.define_node("LINE", "line2")
    b.define_node("SLAVE", "panel_ctrl", rs485=12)   # the Pico RS-485 node
    panel_io(b)                                       # the I2C bus under it
    b.end_node(); b.end_node(); b.end_node()

    dev = b.devices[0]
    print("device:", dev["path"])
    for name, blob in dev["files"].items():
        shown = blob.decode() if name == "ilcf" else list(blob)
        print("  %-5s %s" % (name, shown))
    print("roster under the slave:")
    for e in b.roster("slow_bus.SITE.acme.LINE.line2.SLAVE.panel_ctrl"):
        print("  i2c=0x%02X %-5s %s" % (e["i2c"], e["sub"], e["cls"]))
    b.finish()


if __name__ == "__main__":
    main()
