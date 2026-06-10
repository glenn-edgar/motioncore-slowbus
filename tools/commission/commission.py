#!/usr/bin/env python3
"""commission.py -- bench bring-up / commissioning tool for the SAMD21 config chip.

Talks to a dongle over USB-CDC (ttyACM) using the SLIP/CRC frame protocol in
libcomm.py. JSON config files are stored on-chip as canonical CBOR.

Examples:
    python3 commission.py list
    python3 commission.py --uid <serial> whoami
    python3 commission.py --port /dev/ttyACM0 put CFG0 myconfig.json
    python3 commission.py --port /dev/ttyACM0 get CFG0
    python3 commission.py ls
    python3 commission.py mode          # read MODE register
    python3 commission.py mode 2        # set MODE register
    python3 commission.py regread 0x01
    python3 commission.py regwrite 0x02 0x03

Port selection (for everything except `list`):
    --port /dev/ttyACMx   explicit serial device
    --uid <serial>        match a dongle by its USB serial number (chip UID)
    (neither)             if exactly one dongle is present, it is used

NOTE: `put`/`get` require cbor2, and any chip access requires pyserial. On
PEP-668 ("externally managed") systems install them into a venv:
    python3 -m venv .venv && . .venv/bin/activate
    pip install -r requirements.txt
"""

import argparse
import json
import sys

import libcomm


def _parse_int(s):
    """Parse an int that may be decimal or 0x-hex."""
    return int(s, 0)


def _resolve_port(args):
    """Resolve the serial port to use from --port / --uid / sole-dongle."""
    if args.port:
        return args.port
    dongles = libcomm.enumerate_dongles()
    if args.uid:
        for d in dongles:
            if d["serial"] == args.uid:
                return d["port"]
        sys.exit("error: no dongle with UID %r (found: %s)"
                 % (args.uid, [d["serial"] for d in dongles]))
    if len(dongles) == 1:
        return dongles[0]["port"]
    if not dongles:
        sys.exit("error: no dongles found; specify --port")
    sys.exit("error: multiple dongles found; specify --port or --uid:\n  "
             + "\n  ".join("%s  uid=%s" % (d["port"], d["serial"])
                           for d in dongles))


def _open(args):
    return libcomm.Dongle(_resolve_port(args), timeout=args.timeout)


# ---------------------------------------------------------------------------
# Subcommand handlers
# ---------------------------------------------------------------------------

def cmd_list(args):
    dongles = libcomm.enumerate_dongles()
    if not dongles:
        print("(no dongles found)")
        return
    for d in dongles:
        print("%s  serial/UID=%s" % (d["port"], d["serial"]))


def cmd_whoami(args):
    with _open(args) as dg:
        val = dg.whoami()
        ok = " (OK)" if val == libcomm.WHO_AM_I_EXPECTED else " (UNEXPECTED!)"
        print("WHO_AM_I = 0x%02X%s" % (val, ok))


def cmd_put(args):
    import cbor2  # lazy
    with open(args.jsonfile, "r") as f:
        obj = json.load(f)
    blob = cbor2.dumps(obj, canonical=True)
    with _open(args) as dg:
        dg.file_put(args.name, blob)
    print("put %r: %d bytes of CBOR written" % (args.name, len(blob)))


def cmd_get(args):
    import cbor2  # lazy
    with _open(args) as dg:
        blob = dg.file_get(args.name)
    obj = cbor2.loads(blob)
    print(json.dumps(obj, indent=2, sort_keys=True))


def cmd_ls(args):
    with _open(args) as dg:
        files = dg.file_list()
    if not files:
        print("(no files)")
        return
    for name, length in files:
        print("%-4s  %d bytes" % (name, length))


def cmd_mode(args):
    with _open(args) as dg:
        val = dg.mode(set=args.value)
    print("MODE = 0x%02X (%d)" % (val, val))


def cmd_regread(args):
    with _open(args) as dg:
        val = dg.reg_read(args.reg)
    print("reg 0x%02X = 0x%02X (%d)" % (args.reg, val, val))


def cmd_regwrite(args):
    with _open(args) as dg:
        dg.reg_write(args.reg, args.val)
    print("reg 0x%02X <- 0x%02X" % (args.reg, args.val))


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser():
    p = argparse.ArgumentParser(
        description="SAMD21 config-chip commissioning tool (USB-CDC).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument("--port", help="serial device, e.g. /dev/ttyACM0")
    p.add_argument("--uid", help="select dongle by USB serial number (chip UID)")
    p.add_argument("--timeout", type=float, default=1.0,
                   help="per-frame reply timeout in seconds (default 1.0)")

    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("list", help="enumerate dongles (port + serial/UID)"
                   ).set_defaults(func=cmd_list)

    sub.add_parser("whoami", help="read WHO_AM_I register"
                   ).set_defaults(func=cmd_whoami)

    sp = sub.add_parser("put", help="store a JSON file on-chip as CBOR")
    sp.add_argument("name", help="4-char file name")
    sp.add_argument("jsonfile", help="path to a JSON file")
    sp.set_defaults(func=cmd_put)

    sp = sub.add_parser("get", help="read a file and print it as JSON")
    sp.add_argument("name", help="4-char file name")
    sp.set_defaults(func=cmd_get)

    sub.add_parser("ls", help="list files on-chip"
                   ).set_defaults(func=cmd_ls)

    sp = sub.add_parser("mode", help="get/set the MODE register (0x02)")
    sp.add_argument("value", nargs="?", type=_parse_int,
                    help="value to write; omit to read")
    sp.set_defaults(func=cmd_mode)

    sp = sub.add_parser("regread", help="read a register")
    sp.add_argument("reg", type=_parse_int, help="register address (0x..)")
    sp.set_defaults(func=cmd_regread)

    sp = sub.add_parser("regwrite", help="write a register")
    sp.add_argument("reg", type=_parse_int, help="register address (0x..)")
    sp.add_argument("val", type=_parse_int, help="value (0x..)")
    sp.set_defaults(func=cmd_regwrite)

    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
