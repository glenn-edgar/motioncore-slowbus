#!/usr/bin/env python3
"""adc_monitor.py -- worked example: an ADC monitor with a stream interlock.

ADC mode continuously samples every channel (min/max/avg/rms over 10/1/0.1 Hz
tumbling windows); the interlock compares per-watch ADC streams and drives D6.
This is how you'd author an analog supervisor in the config namespace. Run on the
dev box (vendored KB + ltree under tools/commission/vendor/):

    python3 examples/adc_monitor.py

A bus-supply monitor: trip D6 if the 24 V rail (sensed on A1) drifts out of band
on its 1 Hz average, OR a fast spike is seen on the instantaneous sample.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import config_tree as C


def supply_monitor(b):
    """Composable subtree: one ADC monitor chip. A1 = scaled rail sense."""
    b.device("rail_mon", i2c=0x30, cls="SAMD21", sub="ADC",
             # out-of-band on the steady (1 Hz) average, OR an instantaneous spike
             interlock={"name": "rail",
                        "expr": "A1.avg.mid < 1.0V || A1.avg.mid > 2.8V || A1 > 3.1V",
                        "drive": {"D6": 1}})


def main():
    b = C.Builder("/tmp/adc_monitor.db")
    b.define_node("SITE", "acme")
    b.define_node("SLAVE", "psu_ctrl", rs485=14)
    supply_monitor(b)
    b.end_node()
    b.end_node()

    dev = b.devices[0]
    print("device:", dev["path"], "(sub=%s)" % dev["sub"])
    for name, blob in dev["files"].items():
        print("  %-5s %s" % (name, blob.decode() if name == "ilcf" else list(blob)))
    b.finish()


if __name__ == "__main__":
    main()
