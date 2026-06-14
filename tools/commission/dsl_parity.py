#!/usr/bin/env python3
"""dsl_parity.py -- prove the LuaJIT slave_dsl port is byte-for-byte identical to
the Python slave_dsl (the HW-verified oracle).

For each config: compile with Python slave_dsl AND with lua/dsl_compile.lua, then
diff every on-chip file. Pins/drive are fed to Python in canonical PAD order so
its dict-insertion order matches the Lua port's deterministic pad-sorted output.
"""
import os
import re
import subprocess
import sys

import slave_dsl

HERE = os.path.dirname(os.path.abspath(__file__))


def pad_rank(k):
    m = re.match(r'^[ADad](\d+)$', k)
    return int(m.group(1)) if m else 999


def py_compile(cfg):
    u = slave_dsl.Unit(cfg["i2c"], cfg["mode"])
    if cfg["mode"] == "COUNTER":
        u.counter(cfg.get("rate", 1000))
    if cfg["mode"] == "SERVO":
        u.servo()
    pins = cfg.get("pins")
    if pins:
        u.pins(**{k: pins[k] for k in sorted(pins, key=pad_rank)})
    il = cfg.get("interlock")
    if il:
        drive = il.get("drive")
        if drive:
            drive = {k: drive[k] for k in sorted(drive, key=pad_rank)}
        u.interlock(il["name"], il["expr"], drive)
    return {k: v.hex() for k, v in u.files().items()}


def _lua_lit(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, dict):
        return "{" + ",".join("[%s]=%s" % (_lua_lit(k), _lua_lit(val)) for k, val in v.items()) + "}"
    if isinstance(v, str):
        return '"%s"' % v.replace("\\", "\\\\").replace('"', '\\"')
    if isinstance(v, (int, float)):
        return repr(v)
    raise TypeError(type(v))


def lua_compile(cfg):
    chunk = "return " + _lua_lit(cfg)
    p = subprocess.run(["luajit", "lua/dsl_compile.lua"], input=chunk,
                       capture_output=True, text=True, cwd=HERE)
    if p.returncode != 0:
        return {"_ERROR": p.stderr.strip()}
    out = {}
    for line in p.stdout.strip().splitlines():
        name, hexs = line.split()
        out[name] = hexs
    return out


# Under the coverage rule, EVERY configurable pad for the mode must be declared
# (a real role or `safe` to hold it Hi-Z). D4/D5 = I2C; D6 = INT (GPIO/MIXED/ADC),
# e-stop (SERVO) or a free pad (COUNTER). ADC also reserves D0/D1.
CONFIGS = [
    ("gpio", {"i2c": 0x20, "mode": "GPIO",
              "pins": {"D0": "in:up", "D1": "in:down", "D2": "in:up", "D3": "in:none",
                       "D7": "out:0", "D8": "out:0", "D9": "out:1", "D10": "out:0"},
              "interlock": {"name": "safe", "expr": "(D0 && D1) && (D2 || D3)", "drive": {"D8": 1}}}),
    ("adc", {"i2c": 0x21, "mode": "ADC",
             "pins": {"D2": "safe", "D3": "safe", "D7": "safe", "D8": "safe",
                      "D9": "safe", "D10": "safe"},
             "interlock": {"name": "rms", "expr": "A1.avg.hz100 > 2.0V", "drive": {"D6": 1}}}),
    ("mixed", {"i2c": 0x22, "mode": "MIXED",
               "pins": {"D0": "safe", "D1": "safe", "D2": "safe", "D3": "safe",
                        "D7": "out", "D8": "in:up", "D9": "safe", "D10": "safe"},
               "interlock": {"name": "mix", "expr": "D8", "drive": {"D7": 1}}}),
    ("counter", {"i2c": 0x23, "mode": "COUNTER", "rate": 1000,
                 "pins": {"D2": "count:up:rising", "A0": "dac", "D1": "adc", "D9": "out",
                          "D10": "in", "D3": "safe", "D7": "safe",
                          "D8": "safe", "D6": "safe"}}),
    # SERVO: servo channels + bench roles + safe; D6 = e-stop (not declared)
    ("servo", {"i2c": 0x24, "mode": "SERVO",
               "pins": {"D0": "servo", "D1": "servo", "D2": "adc", "D3": "out",
                        "D7": "oc:up", "D8": "safe", "D9": "safe", "D10": "safe"}}),
    # edge cases -----------------------------------------------------------
    ("gpio_no_il", {"i2c": 0x20, "mode": "GPIO",
                    "pins": {"D0": "in:up", "D1": "in:up", "D2": "in:up", "D3": "in:up",
                             "D7": "in:down", "D8": "out:1", "D9": "oc", "D10": "oc:up"}}),
    ("gpio_or_not", {"i2c": 0x20, "mode": "GPIO",
                     "pins": {"D0": "in:up", "D1": "in:up", "D2": "in:none", "D3": "in:none",
                              "D7": "out:0", "D8": "oc", "D9": "out:1", "D10": "out:0"},
                     "interlock": {"name": "n", "expr": "~D0 || (D1 && ~D2)", "drive": {"D8": 1, "D7": 0}}}),
    # GPIO with a `safe` (Hi-Z) pad
    ("gpio_safe", {"i2c": 0x20, "mode": "GPIO",
                   "pins": {"D0": "in:up", "D1": "safe", "D2": "in:none", "D3": "in:down",
                            "D7": "out:0", "D8": "out:1", "D9": "oc:up", "D10": "safe"},
                   "interlock": {"name": "g", "expr": "D0 && ~D2", "drive": {"D7": 1}}}),
    ("adc_dnf", {"i2c": 0x21, "mode": "ADC",
                 "pins": {"D2": "safe", "D3": "safe", "D7": "safe", "D8": "safe",
                          "D9": "safe", "D10": "safe"},
                 "interlock": {"name": "t", "expr": "A1.avg.hz100 > 1.0V && A1.avg.hz100 < 3.0V",
                               "drive": {"D6": 1}}}),
    # MIXED mxmp: a mix of roles incl. dac on D0, an in:up:debounce_50ms, safe pads;
    # the interlock watches D8 and drives the oc pin D7.
    ("mixed_mxmp", {"i2c": 0x22, "mode": "MIXED",
                    "pins": {"D0": "dac", "A1": "adc", "D2": "out", "D3": "oc",
                             "D7": "oc:up", "D8": "in:up:debounce_50ms", "D9": "in:down",
                             "D10": "safe"},
                    "interlock": {"name": "s", "expr": "A1 > 1.5V && D8", "drive": {"D7": 1}}}),
    # MIXED dac on D0 + plain safe everywhere else, no interlock
    ("mixed_dac", {"i2c": 0x22, "mode": "MIXED",
                   "pins": {"D0": "dac", "D1": "safe", "D2": "safe", "D3": "safe",
                            "D7": "safe", "D8": "safe", "D9": "safe", "D10": "safe"}}),
    ("counter_multi", {"i2c": 0x23, "mode": "COUNTER", "rate": 2000,
                       "pins": {"D0": "count:up:rising", "D2": "count:down:both",
                                "D3": "count:none:falling", "D7": "out", "D1": "adc",
                                "D8": "safe", "D9": "safe", "D10": "safe", "D6": "count:up"}}),
]

# Configs that MUST raise DSLError in BOTH Python and Lua.
ERROR_CONFIGS = [
    # D6 is the interlock/INT output in GPIO -- not a configurable pad
    ("err_gpio_d6", {"i2c": 0x20, "mode": "GPIO",
                     "pins": {"D0": "in:up", "D1": "in:up", "D2": "in:up", "D3": "in:up",
                              "D6": "out:0", "D7": "out:0", "D8": "out:0", "D9": "out:0",
                              "D10": "out:0"}}),
    # dac is only on D0/A0 in MIXED
    ("err_mixed_dac", {"i2c": 0x22, "mode": "MIXED",
                       "pins": {"D0": "safe", "D1": "safe", "D2": "dac", "D3": "safe",
                                "D7": "safe", "D8": "safe", "D9": "safe", "D10": "safe"}}),
    # missing pad: D10 unassigned -> coverage error
    ("err_missing_pad", {"i2c": 0x20, "mode": "GPIO",
                         "pins": {"D0": "in:up", "D1": "in:up", "D2": "in:up", "D3": "in:up",
                                  "D7": "out:0", "D8": "out:0", "D9": "out:0"}}),
]


def main():
    npass = nfail = 0
    for name, cfg in CONFIGS:
        try:
            py = py_compile(cfg)
        except Exception as e:
            print("  %-14s PYTHON-ERROR %s" % (name, e)); nfail += 1; continue
        lua = lua_compile(cfg)
        if "_ERROR" in lua:
            print("  %-14s LUA-ERROR %s" % (name, lua["_ERROR"])); nfail += 1; continue
        if py == lua:
            print("  %-14s OK  (%s)" % (name, ", ".join("%s=%dB" % (k, len(v) // 2)
                                                        for k, v in sorted(py.items()))))
            npass += 1
        else:
            print("  %-14s MISMATCH" % name)
            for k in sorted(set(py) | set(lua)):
                if py.get(k) != lua.get(k):
                    print("      %-5s py=%s" % (k, py.get(k)))
                    print("      %-5s lua=%s" % (k, lua.get(k)))
            nfail += 1

    # expected-error configs: BOTH sides must reject (Python raises, Lua _ERROR)
    for name, cfg in ERROR_CONFIGS:
        py_err = lua_err = False
        try:
            py_compile(cfg)
        except Exception:
            py_err = True
        lua = lua_compile(cfg)
        lua_err = "_ERROR" in lua
        if py_err and lua_err:
            print("  %-14s OK  (both reject)" % name); npass += 1
        else:
            print("  %-14s MISMATCH py_err=%s lua_err=%s" % (name, py_err, lua_err)); nfail += 1

    print("\n%d/%d configs byte-identical" % (npass, npass + nfail))
    sys.exit(0 if nfail == 0 else 1)


if __name__ == "__main__":
    main()
