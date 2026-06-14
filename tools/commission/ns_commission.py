#!/usr/bin/env python3
"""ns_commission.py -- namespace-driven SAMD21 commissioning.

The Python flow now speaks ONLY the DSL and the namespace DB -- no hand-typed
register pokes. Two phases:

  Phase 1 (DSL):   a config_tree Builder defines the namespace + per-device config
                   (i2c / class / mode / pins / interlock) and commit_to()'s it
                   into the namespace DB (one ltree SQLite store). Config is the
                   SOURCE; the on-chip bytes are derived, never stored.

  Phase 2 (this):  given a user-supplied namespace PATH, read that node's config
                   from the DB, recompile its on-chip files via slave_dsl, push
                   them to the physical SAMD21 (offline -> format -> file_put ->
                   reboot), verify the mode applied (reopening the chip by UID),
                   then write the UID binding + status back into the DB.

    python3 ns_commission.py --db cfg.db define                       # phase 1 (demo)
    python3 ns_commission.py --db cfg.db commission slow_bus.SLAVE.node1.DEV.estop
    python3 ns_commission.py --db cfg.db show                         # inspect the DB

The user never types a UID or a ttyACM: phase 2 discovers the chip, reads its
UID (USB serial), and the namespace stores name->uid; uid->ttyACM stays a live
lookup.
"""

import argparse
import datetime
import os
import sys
import tempfile
import time

import libcomm
from libcomm import Dongle
import slave_dsl
import config_tree
import namespace_db

REG_MODE = 0x02


def _now():
    return datetime.datetime.now().isoformat(timespec="seconds")


# ---------------------------------------------------------------------------
# Phase 1 -- DSL: define a GPIO device with an arbitrary interlock, into the DB.
# ---------------------------------------------------------------------------
def define_gpio_test(ns, slave="node1", i2c=0x20):
    """A GPIO 'estop' SAMD21 with an arbitrary interlock. Returns (regen_summary,
    device_paths). config_tree drives namespace_db.regen_subtree under its root."""
    kb_scratch = os.path.join(tempfile.mkdtemp(), "kb.db")   # legacy Construct_KB scratch
    b = config_tree.Builder(kb_scratch)
    b.define_node("SLAVE", slave)
    b.device("estop", i2c=i2c, cls="SAMD21", sub="GPIO",
             pins={"D0": "in:up", "D1": "in:up", "D2": "in:none", "D3": "in:down",
                   "D7": "out:0", "D8": "out:0", "D9": "out:1", "D10": "out:0"},
             interlock={"name": "safe",
                        "expr": "(D0 && D1) && (D2 || D3)",   # arbitrary boolean interlock
                        "drive": {"D8": 1}})                   # D8 = 1 while the condition holds
    b.end_node()
    b.finish()
    return b.commit_to(ns), [d["path"] for d in b.devices]


# ---------------------------------------------------------------------------
# Phase 2 -- commission a node BY NAME.
# ---------------------------------------------------------------------------
def _compile_files(attrs):
    """Recompile a node's on-chip files from its stored config source (slave_dsl)."""
    u = slave_dsl.Unit(attrs["i2c"], attrs["sub"])
    if attrs.get("pins"):
        u.pins(**attrs["pins"])
    il = attrs.get("interlock")
    if il:
        u.interlock(il["name"], il["expr"], il.get("drive"))
    return u.files()


def _resolve_chip(port=None, uid=None):
    """Find the target SAMD21 -> (port, uid). Sole-device if neither given."""
    ds = libcomm.enumerate_dongles()
    if uid:
        for d in ds:
            if d["serial"] == uid:
                return d["port"], d["serial"]
        raise SystemExit("no SAMD21 present with uid %s (found: %s)"
                         % (uid, [d["serial"] for d in ds]))
    if port:
        for d in ds:
            if d["port"] == port:
                return d["port"], d["serial"]
        raise SystemExit("port %s is not an enumerated SAMD21 (2886:802f)" % port)
    if len(ds) == 1:
        return ds[0]["port"], ds[0]["serial"]
    if not ds:
        raise SystemExit("no SAMD21 found (is the XIAO enumerated as 2886:802f?)")
    raise SystemExit("multiple SAMD21 present; pass --port or --uid:\n  "
                     + "\n  ".join("%s uid=%s" % (d["port"], d["serial"]) for d in ds))


def commission_node(ns, path, *, port=None, uid=None, erase=True, settle=9.0,
                    rebind=False):
    """Commission the physical chip for namespace `path` and update the DB."""
    node = ns.get_node(path)
    if node is None:
        raise SystemExit("no namespace node %r (run `define` / build the DSL first)" % path)
    attrs = node["attrs"]
    if attrs.get("cls") != "SAMD21":
        raise SystemExit("node %r is cls=%r, not SAMD21" % (path, attrs.get("cls")))

    files = _compile_files(attrs)
    tgt_port, chip_uid = _resolve_chip(port, uid)
    print("commissioning %s -> chip uid=%s on %s" % (path, chip_uid, tgt_port))
    print("  mode=%s i2c=0x%02X files=%s" % (attrs["sub"], attrs["i2c"], sorted(files)))

    # push (the proven offline -> format -> file_put -> reboot sequence)
    dg = Dongle(tgt_port)
    who = dg.whoami()
    if who != libcomm.WHO_AM_I_EXPECTED:
        dg.close()
        raise SystemExit("whoami 0x%02X != 0x%02X -- wrong device?" % (who, libcomm.WHO_AM_I_EXPECTED))
    dg.offline()
    if erase:
        dg.file_format()
    for name, blob in files.items():
        dg.file_put(name, blob)
    dg.close()                        # disconnect -> reboot -> apply
    time.sleep(settle)

    # verify: reopen BY UID (the port may renumber after the reboot) and confirm
    # the mode actually applied -- this exercises the name->uid->ttyACM path.
    vport, _ = _resolve_chip(uid=chip_uid)
    dgv = Dongle(vport)
    mode = dgv.reg_read(REG_MODE)
    dgv.close()
    expected = slave_dsl.MODES[attrs["sub"]]
    applied = (mode == expected)

    # write the binding + status into the namespace DB
    if rebind:
        ns.rebind(path, chip_uid, vid=libcomm.USB_VID, pid=libcomm.USB_PID)
    else:
        ns.bind_uuid(path, chip_uid, vid=libcomm.USB_VID, pid=libcomm.USB_PID)
    ns.set_status(path, {
        "state": "commissioned" if applied else "mode-mismatch",
        "mode": mode, "files": sorted(files), "last_seen": _now(),
    })
    print("  applied mode=0x%02X (expect 0x%02X) -> %s" %
          (mode, expected, "OK" if applied else "MISMATCH"))
    print("  bound %s <- uid %s" % (path, chip_uid))
    return {"path": path, "uid": chip_uid, "mode": mode, "applied": applied}


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def cmd_define(ns, args):
    summary, paths = define_gpio_test(ns, slave=args.slave, i2c=args.i2c)
    print("defined namespace (regen):", summary)
    print("device paths:", paths)


def cmd_commission(ns, args):
    commission_node(ns, args.path, port=args.port, uid=args.uid,
                    erase=not args.no_erase, settle=args.settle, rebind=args.rebind)


def cmd_show(ns, args):
    root = args.path or "slow_bus"
    print("namespace under %r:" % root)
    for p in ns.list_subtree(root):
        b = ns.resolve_uuid(p)
        st = ns.get_status(p)
        tag = ("  uid=%s" % b) if b else ""
        tag += ("  status=%s" % st) if st else ""
        print("  %s%s" % (p, tag))


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", required=True, help="namespace SQLite DB path")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("define", help="phase 1: build the GPIO-test namespace into the DB")
    sp.add_argument("--slave", default="node1")
    sp.add_argument("--i2c", type=lambda s: int(s, 0), default=0x20)
    sp.set_defaults(func=cmd_define)

    sp = sub.add_parser("commission", help="phase 2: commission the chip for a namespace path")
    sp.add_argument("path", help="namespace ltree path, e.g. slow_bus.SLAVE.node1.DEV.estop")
    sp.add_argument("--port", help="explicit ttyACM (else sole device / --uid)")
    sp.add_argument("--uid", help="select chip by USB serial / UID")
    sp.add_argument("--no-erase", action="store_true", help="don't FORMAT the store first")
    sp.add_argument("--rebind", action="store_true", help="override an existing binding (chip swap)")
    sp.add_argument("--settle", type=float, default=9.0, help="post-reboot settle seconds")
    sp.set_defaults(func=cmd_commission)

    sp = sub.add_parser("show", help="list namespace nodes + bindings + status")
    sp.add_argument("path", nargs="?", help="subtree root (default slow_bus)")
    sp.set_defaults(func=cmd_show)
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)
    ns = namespace_db.NamespaceDB(args.db)
    try:
        args.func(ns, args)
    finally:
        ns.close()


if __name__ == "__main__":
    main()
