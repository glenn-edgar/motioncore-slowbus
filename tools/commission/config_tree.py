#!/usr/bin/env python3
"""config_tree.py -- slow_bus system configuration as a composable node tree.

A thin, *balanced* ``define_node`` / ``end_node`` layer over the ChainTree
KnowledgeBaseManager (``Construct_KB``), storing the configuration in an
ltree-indexed SQLite database so a configuration can be queried and mapped
across an enterprise.

Hierarchy (DSL namespace == Zenoh key space == ltree path):

    container / zenoh root
      dongle / RS-485 master node
        RS-485 slave unit
          I2C nodes (SAMD21 devices + other I2C chips)

Subtrees are plain Python functions that take a Builder and call
``define_node()`` / ``end_node()``. Because the calls are balanced, a subtree
inherits whatever path is open at its call site -- so the *same* subtree can be
developed and tested locally (under a fresh root) and composed unchanged
anywhere in the enterprise. The I2C level only needs its local context; the
enterprise path above it is inherited for naming and rostering.

This module is the namespace/KB layer only. Per-device ``idnt`` is emitted here
(``[i2c_addr, mode]``); the ``ilcf`` interlock/pin compilation stays in
``slave_dsl`` and is wired in a later step.
"""

import contextlib
import io
import json
import os
import sys

# -- vendored, self-contained dependencies (the repo stands on its own) ------
# Both the ChainTree KnowledgeBaseManager (construct_kb) and the ltree SQLite
# extension live under vendor/, so commissioning needs no external paths or
# system installs. See vendor/README.md for provenance.
_VENDOR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vendor")
if _VENDOR not in sys.path:
    sys.path.insert(0, _VENDOR)
from construct_kb.construct_kb import Construct_KB  # noqa: E402  (vendor/construct_kb)

# ltree extension (vendor/ltree/ltree.so); Construct_KB strips the .so suffix.
# The .so is a build artifact (repo .gitignore excludes *.so), so build it from
# the vendored source on first use -- the repo bootstraps itself.
_LTREE = os.path.join(_VENDOR, "ltree", "ltree")


def _ensure_ltree():
    so = _LTREE + ".so"
    if os.path.exists(so):
        return
    import subprocess
    d = os.path.dirname(_LTREE)
    try:
        subprocess.run(["make", "-s", "-C", d, "ltree.so"],
                       check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as e:
        detail = getattr(e, "stderr", "") or str(e)
        raise RuntimeError(
            "ltree extension not built and auto-build failed -- run "
            "`make -C %s ltree.so` (needs gcc + libsqlite3-dev).\n%s" % (d, detail))


_ensure_ltree()

import slave_dsl  # noqa: E402  -- mode-name -> mode-number (ilcf compile comes later)

DEV_LINK = "DEV"  # ltree link label marking an I2C device (leaf) node


@contextlib.contextmanager
def _quiet():
    """Swallow Construct_KB's debug prints (it print()s every path it builds)."""
    with contextlib.redirect_stdout(io.StringIO()):
        yield


class Builder:
    """Stack-based, balanced builder over Construct_KB.

    define_node()/end_node() push/pop hierarchy nodes; device() writes an I2C
    device leaf and records its idnt. A subtree is any function taking a Builder.
    """

    def __init__(self, db_path, kb_name="slow_bus", description="slow_bus config"):
        if os.path.exists(db_path):
            os.remove(db_path)  # declarative build -> start clean
        with _quiet():
            self.kb = Construct_KB(db_path, kb_name, ltree_extension_path=_LTREE)
            self.kb.add_kb(kb_name, description)
            self.kb.select_kb(kb_name)
        self.kb_name = kb_name
        self._stack = []   # [(link, name), ...] currently-open nodes
        self.devices = []  # collected device records (for the commissioner)
        # Accumulated declared namespace for namespace_db.regen_subtree(): every
        # node this builder constructs, keyed by ltree path. The root node
        # (kb_name) anchors the subtree the regen is scoped to.
        self.declared = {self.kb_name: {"kind": "KB", "attrs": {"description": description}}}

    # -- current open path (kb.link.name.link.name...) -------------------
    def path(self):
        labels = [self.kb_name]
        for link, name in self._stack:
            labels += [link, name]
        return ".".join(labels)

    # -- balanced hierarchy primitives -----------------------------------
    def define_node(self, link, name, **props):
        with _quiet():
            self.kb.add_header_node(link, name, dict(props), {})
        self._stack.append((link, name))
        self.declared[self.path()] = {"kind": link, "attrs": dict(props)}
        return self

    def end_node(self):
        if not self._stack:
            raise RuntimeError("end_node() with no open node")
        link, name = self._stack.pop()
        with _quiet():
            self.kb.leave_header_node(link, name)
        return self

    # -- I2C device leaf -------------------------------------------------
    def device(self, name, i2c, cls, sub, pins=None, interlock=None):
        sub_u = str(sub).upper()
        if sub_u not in slave_dsl.MODES:
            raise ValueError("unknown device sub-type %r" % sub)
        mode = slave_dsl.MODES[sub_u]

        # Compile the on-chip files via slave_dsl: idnt always; gpmp + ilcf for
        # GPIO/MIXED. slave_dsl validates (all 8 GPIO pins, limits) and raises
        # DSLError, so a bad device fails the build loudly.
        u = slave_dsl.Unit(i2c, sub_u)
        if pins:
            u.pins(**pins)
        if interlock:
            u.interlock(interlock["name"], interlock["expr"], interlock.get("drive"))
        files = u.files()                         # {name: raw bytes}

        # The KB node stores the human SOURCE (queryable, regenerable); the record
        # carries the compiled bytes for the commissioner. (bytes aren't JSON, and
        # the files are derivable from the source, so keep one source of truth.)
        props = {"i2c": i2c, "cls": cls, "sub": sub_u}
        data = {"pins": pins or {}, "interlock": interlock}
        with _quiet():
            self.kb.add_info_node(DEV_LINK, name, props, data)  # leaf: push+pop
        devpath = "%s.%s.%s" % (self.path(), DEV_LINK, name)
        self.devices.append({
            "path": devpath,
            "name": name, "i2c": i2c, "cls": cls, "sub": sub_u, "mode": mode,
            "files": files, "pins": pins or {}, "interlock": interlock,
        })
        # namespace_db attrs carry the JSON-able SOURCE (one source of truth); the
        # commissioner recompiles the raw on-chip files from it via slave_dsl.
        self.declared[devpath] = {"kind": DEV_LINK, "attrs": {
            "i2c": i2c, "cls": cls, "sub": sub_u, "mode": mode,
            "pins": pins or {}, "interlock": interlock}}
        return self

    # -- roster: immediate I2C devices under a node ----------------------
    def roster(self, node_path):
        """Devices one ltree level past DEV under node_path (the Pico's bus roster)."""
        with _quiet():
            rows = self.kb.find_by_pattern(node_path + "." + DEV_LINK + ".*", self.kb_name)
        out = []
        for r in rows:
            props = json.loads(r["properties"]) if r["properties"] else {}
            out.append({"path": r["path"], "name": r["name"], **props})
        return out

    # -- drive the namespace DB ------------------------------------------
    def commit_to(self, ns, root=None):
        """Subtree-scoped regen of everything this builder declared into a
        namespace_db.NamespaceDB. `root` defaults to the shallowest declared
        path (this builder's natural subtree root), so composed subtrees each
        reconcile only their own branch and never touch a sibling's. Returns the
        regen summary {added, deleted, kept}. UID bindings + status under the
        root survive unless their node was dropped from the declaration."""
        if not self.declared:
            return {"added": [], "deleted": [], "kept": []}
        if root is None:
            root = min(self.declared, key=lambda p: (p.count("."), p))
        sub = {p: v for p, v in self.declared.items()
               if p == root or p.startswith(root + ".")}
        return ns.regen_subtree(root, sub)

    def finish(self):
        with _quiet():
            self.kb.check_installation()  # asserts the stack emptied (balanced)
            self.kb.disconnect()


# ---------------------------------------------------------------------------
# Demo: one composable I2C-bus subtree, built two ways (local + enterprise)
# ---------------------------------------------------------------------------

def conveyor_io(b):
    """A composable I2C-bus subtree -- builds under whatever node is open.

    Two SAMD21 devices: a GPIO e-stop (full pin definition + interlock) and a
    servo axis. Identical wherever it is instantiated; only the inherited path
    above it changes.
    """
    b.device("estop", i2c=0x20, cls="SAMD21", sub="GPIO",
             pins={"D0": "in:up", "D1": "in:up", "D2": "in:none", "D3": "in:down",
                   "D7": "out:0", "D8": "out:0", "D9": "out:1", "D10": "out:0"},
             interlock={"name": "safe",
                        "expr": "(D0 && D1) && (D2 || D3)",
                        "drive": {"D8": 1}})
    b.device("axis", i2c=0x40, cls="SAMD21", sub="SERVO")


def _show(title, b, roster_path):
    print("\n=== %s ===" % title)
    print("devices built (compiled on-chip files):")
    for d in b.devices:
        f = d["files"]
        desc = "idnt=%s" % list(f["idnt"])
        if "gpmp" in f:
            desc += " gpmp=%s" % list(f["gpmp"])
        if "ilcf" in f:
            desc += " ilcf=%dB" % len(f["ilcf"])
        print("  %-46s %s" % (d["path"], desc))
    print("roster(%s):" % roster_path)
    for e in b.roster(roster_path):
        print("  i2c=0x%02X %-6s %s  @ %s" % (e["i2c"], e["sub"], e["cls"], e["path"]))


if __name__ == "__main__":
    # (A) Local development: drop the subtree directly under one RS-485 slave.
    a = Builder("/tmp/cfg_local.db")
    a.define_node("SLAVE", "node1", rs485=12)
    conveyor_io(a)                       # inherits slow_bus.SLAVE.node1
    a.end_node()
    _show("A  local (slave only)", a, "slow_bus.SLAVE.node1")
    a.finish()

    # (B) Enterprise: SAME subtree, composed deeper under master -> slave.
    b = Builder("/tmp/cfg_enterprise.db")
    b.define_node("MASTER", "dongle1")
    b.define_node("SLAVE", "node1", rs485=12)
    conveyor_io(b)                       # inherits slow_bus.MASTER.dongle1.SLAVE.node1
    b.end_node()
    b.end_node()
    _show("B  enterprise (master -> slave)", b, "slow_bus.MASTER.dongle1.SLAVE.node1")
    b.finish()

    print("\nSame conveyor_io() subtree, two contexts: device idnt is identical,")
    print("only the inherited ltree path differs -> config maps across the enterprise.")

    # (C) Wire the DSL into the namespace DB: commit drives subtree-scoped regen.
    import namespace_db
    nsf = "/tmp/cfg_ns.db"
    if os.path.exists(nsf):
        os.remove(nsf)
    ns = namespace_db.NamespaceDB(nsf)

    c1 = Builder("/tmp/cfg_ns_build.db")
    c1.define_node("SLAVE", "node1", rs485=12)
    conveyor_io(c1)                       # estop + axis under slow_bus.SLAVE.node1
    c1.end_node(); c1.finish()
    print("\n=== C  DSL -> namespace_db ===")
    print("commit (full build):", c1.commit_to(ns))
    print("nodes:", ns.list_subtree("slow_bus"))

    # The commission tool would write this; do it inline to show it survives regen.
    ns.bind_uuid("slow_bus.SLAVE.node1.DEV.estop", "UID-ESTOP-01", vid=0x2886, pid=0x802F)
    print("estop bound to:", ns.resolve_uuid("slow_bus.SLAVE.node1.DEV.estop"))

    # Re-run the DSL with `axis` DROPPED -> subtree-scoped regen deletes axis and
    # preserves the estop binding (a different node).
    c2 = Builder("/tmp/cfg_ns_build.db")
    c2.define_node("SLAVE", "node1", rs485=12)
    c2.device("estop", i2c=0x20, cls="SAMD21", sub="GPIO",
              pins={"D0": "in:up", "D1": "in:up", "D2": "in:none", "D3": "in:down",
                    "D7": "out:0", "D8": "out:0", "D9": "out:1", "D10": "out:0"},
              interlock={"name": "safe", "expr": "(D0 && D1) && (D2 || D3)", "drive": {"D8": 1}})
    c2.end_node(); c2.finish()
    print("regen (axis dropped):", c2.commit_to(ns))
    print("nodes:", ns.list_subtree("slow_bus"))
    print("estop binding preserved:", ns.resolve_uuid("slow_bus.SLAVE.node1.DEV.estop"))
    ns.close()
