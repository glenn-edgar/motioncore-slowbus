#!/usr/bin/env python3
"""namespace_db.py -- the namespace DB layer (SQLite + ltree extension).

The one authoritative store the whole system is a client of: an ltree-indexed
namespace, a UID binding table with engine-enforced uniqueness, and per-node
JSONB status. The DSL constructs/regenerates the namespace; the commission tool
writes UID bindings; the Linux access programs resolve a namespace *name* to a
UID and (live) to a ttyACM.

Three tables, three lifecycles:

  * ns_node       -- the namespace tree (ltree path -> JSONB config attrs).
                     CONSTRUCTED by the DSL, regenerable.
  * uuid_binding  -- path <-> uuid, the AUTHORITATIVE binding. Hard constraints:
                     PRIMARY KEY(path) = one binding per node, UNIQUE(uuid) = one
                     node per physical chip (no stealing). Written at commission.
  * node_status   -- soft runtime telemetry (state/last_seen/...) as JSONB.
                     Written at runtime by the access/control programs.

`uuid_binding` and `node_status` are FK'd to `ns_node` ON DELETE CASCADE, so
removing a namespace node releases its chip binding and status in one step
(delete == unbind).

Locked-in rules (see the design discussion):
  1. JSONB version check -- JSONB (binary) requires SQLite >= 3.45; older falls
     back to TEXT json (JSON1, >= 3.38). Below 3.38 we refuse.
  2. UID uniqueness -- a dedicated table with UNIQUE(uuid), not a JSON index.
  3. Idempotent, SUBTREE-SCOPED regen -- regen_subtree(root, declared) deletes
     only orphans UNDER `root`, adds new nodes blank, preserves the rest (and
     their bindings/status). It NEVER touches nodes outside `root`, so building
     one branch can't wipe another. This is what makes regen safe, not just
     idempotent.
"""

import json
import os
import sqlite3
import sys

# -- vendored ltree extension (same artifact config_tree.py uses) ------------
_VENDOR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vendor")
_LTREE = os.path.join(_VENDOR, "ltree", "ltree")          # Construct_KB strips ".so"

SCHEMA_VERSION = 1


class NamespaceError(Exception):
    pass


class AlreadyBound(NamespaceError):
    """path already holds a different uuid (use rebind)."""


class UidInUse(NamespaceError):
    """uuid already bound to a different path (no chip stealing)."""


def _ensure_ltree_so():
    so = _LTREE + ".so"
    if os.path.exists(so):
        return so
    import subprocess
    d = os.path.dirname(_LTREE)
    try:
        subprocess.run(["make", "-s", "-C", d, "ltree.so"],
                       check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as e:
        detail = getattr(e, "stderr", "") or str(e)
        raise NamespaceError(
            "ltree extension not built and auto-build failed -- run "
            "`make -C %s ltree.so` (needs gcc + libsqlite3-dev).\n%s" % (d, detail))
    return so


def _ver_tuple(s):
    return tuple(int(x) for x in s.split("."))


def _depth(path):
    """ltree depth = number of dot-separated components (matches ltree_depth)."""
    return path.count(".") + 1


def _now_iso():
    # Imported lazily so the module loads in environments where time is faked.
    import datetime
    return datetime.datetime.now().isoformat(timespec="seconds")


class NamespaceDB:
    def __init__(self, db_path):
        self.db_path = db_path
        self._db = sqlite3.connect(db_path)
        self._db.row_factory = sqlite3.Row

        # JSONB version gate (locked-in rule #1).
        v = _ver_tuple(sqlite3.sqlite_version)
        if v < (3, 38, 0):
            raise NamespaceError(
                "SQLite %s too old: JSON1 needs >= 3.38" % sqlite3.sqlite_version)
        self.jsonb = v >= (3, 45, 0)
        self._jfn = "jsonb" if self.jsonb else "json"   # writer; reader is always json()

        self._load_ltree()
        self._db.execute("PRAGMA foreign_keys = ON")
        self._db.execute("PRAGMA journal_mode = WAL")    # multi-writer (commission + control)
        self._db.execute("PRAGMA busy_timeout = 5000")
        self._init_schema()

    # -- setup ----------------------------------------------------------------
    def _load_ltree(self):
        so = _ensure_ltree_so()
        self._db.enable_load_extension(True)
        self._db.load_extension(so)
        self._db.enable_load_extension(False)
        # smoke-test the function is present
        if self._db.execute("SELECT ltree_descendant('a', 'a.b')").fetchone()[0] != 1:
            raise NamespaceError("ltree extension loaded but ltree_descendant misbehaves")

    def _init_schema(self):
        c = self._db
        c.executescript("""
            CREATE TABLE IF NOT EXISTS ns_meta (
                key   TEXT PRIMARY KEY,
                value TEXT
            );
            CREATE TABLE IF NOT EXISTS ns_node (
                path  TEXT PRIMARY KEY,      -- ltree dotted path
                depth INTEGER NOT NULL,
                kind  TEXT,                  -- link label / node kind (SLAVE, DEV, ...)
                attrs BLOB                   -- JSONB config attributes
            );
            CREATE INDEX IF NOT EXISTS idx_ns_node_depth ON ns_node(depth);

            CREATE TABLE IF NOT EXISTS uuid_binding (
                path     TEXT PRIMARY KEY
                         REFERENCES ns_node(path) ON DELETE CASCADE,
                uuid     TEXT NOT NULL UNIQUE,   -- one chip, one node (no stealing)
                vid      INTEGER,
                pid      INTEGER,
                bound_at TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS node_status (
                path       TEXT PRIMARY KEY
                           REFERENCES ns_node(path) ON DELETE CASCADE,
                status     BLOB,             -- JSONB runtime telemetry
                updated_at TEXT
            );
        """)
        row = c.execute("SELECT value FROM ns_meta WHERE key='schema_version'").fetchone()
        if row is None:
            c.execute("INSERT INTO ns_meta(key, value) VALUES ('schema_version', ?)",
                      (str(SCHEMA_VERSION),))
        elif int(row[0]) != SCHEMA_VERSION:
            raise NamespaceError(
                "schema_version %s != expected %d (migration needed)"
                % (row[0], SCHEMA_VERSION))
        c.commit()

    # -- namespace construction (DSL-facing) ----------------------------------
    def regen_subtree(self, root, declared):
        """Idempotent, SUBTREE-SCOPED reconcile of the namespace under `root`.

        `declared` = {path: {"kind": str|None, "attrs": dict|None}} -- every node
        that should exist at-or-under `root` after this call. Every declared path
        must be `root` itself or a descendant of it (else NamespaceError: the
        caller is reaching outside its scope).

        Effect, scoped to `root`'s subtree ONLY:
          * delete nodes present in the db but NOT declared  (cascade -> their
            uuid_binding + node_status go too: delete == unbind);
          * insert declared nodes that don't exist yet, BLANK (no binding/status);
          * update attrs/kind of nodes that persist -- bindings/status untouched.

        Returns {"added": [...], "deleted": [...], "kept": [...]}.
        """
        # validate scope
        bad = [p for p in declared if p != root and not _is_descendant(root, p)]
        if bad:
            raise NamespaceError(
                "declared paths outside subtree %r: %s" % (root, bad[:5]))

        c = self._db
        existing = {r["path"] for r in c.execute(
            "SELECT path FROM ns_node WHERE path = ? OR ltree_descendant(?, path) = 1",
            (root, root))}
        declared_paths = set(declared)
        to_delete = existing - declared_paths
        to_add = declared_paths - existing
        to_keep = existing & declared_paths

        if to_delete:
            c.executemany("DELETE FROM ns_node WHERE path = ?",
                          [(p,) for p in to_delete])   # FK cascade unbinds
        for p in sorted(to_add, key=_depth):           # parents before children
            spec = declared[p] or {}
            self._put_node(p, spec.get("kind"), spec.get("attrs"))
        for p in to_keep:
            spec = declared[p] or {}
            self._put_node(p, spec.get("kind"), spec.get("attrs"), update=True)
        c.commit()
        return {"added": sorted(to_add), "deleted": sorted(to_delete),
                "kept": sorted(to_keep)}

    def _put_node(self, path, kind, attrs, update=False):
        blob_sql = "%s(?)" % self._jfn
        attrs_json = json.dumps(attrs or {}, sort_keys=True)
        if update:
            self._db.execute(
                "UPDATE ns_node SET kind = ?, attrs = " + blob_sql + " WHERE path = ?",
                (kind, attrs_json, path))
        else:
            self._db.execute(
                "INSERT INTO ns_node(path, depth, kind, attrs) VALUES (?, ?, ?, " + blob_sql + ")",
                (path, _depth(path), kind, attrs_json))

    # -- read -----------------------------------------------------------------
    def get_node(self, path):
        r = self._db.execute(
            "SELECT path, depth, kind, json(attrs) AS attrs FROM ns_node WHERE path = ?",
            (path,)).fetchone()
        if r is None:
            return None
        return {"path": r["path"], "depth": r["depth"], "kind": r["kind"],
                "attrs": json.loads(r["attrs"]) if r["attrs"] else {}}

    def list_subtree(self, root):
        rows = self._db.execute(
            "SELECT path FROM ns_node WHERE path = ? OR ltree_descendant(?, path) = 1 "
            "ORDER BY depth, path", (root, root))
        return [r["path"] for r in rows]

    # -- uuid binding (commission-facing; minimal for now) --------------------
    def bind_uuid(self, path, uuid, *, vid=None, pid=None):
        """Create the path<->uuid binding. Idempotent for the same (path, uuid).
        Raises AlreadyBound / UidInUse on a conflicting bind (use rebind to override)."""
        if self.get_node(path) is None:
            raise NamespaceError("cannot bind unknown node %r" % path)
        cur = self._db.execute(
            "SELECT uuid FROM uuid_binding WHERE path = ?", (path,)).fetchone()
        if cur and cur["uuid"] != uuid:
            raise AlreadyBound("%s already bound to %s; use rebind()" % (path, cur["uuid"]))
        owner = self.resolve_path(uuid)
        if owner and owner != path:
            raise UidInUse("uuid %s already bound to %s; use rebind()" % (uuid, owner))
        self._db.execute(
            "INSERT INTO uuid_binding(path, uuid, vid, pid, bound_at) VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT(path) DO UPDATE SET uuid=excluded.uuid, vid=excluded.vid, "
            "pid=excluded.pid, bound_at=excluded.bound_at",
            (path, uuid, vid, pid, _now_iso()))
        self._db.commit()

    def resolve_uuid(self, path):
        """name -> uuid (the durable handle; the link layer resolves uuid->ttyACM live)."""
        r = self._db.execute("SELECT uuid FROM uuid_binding WHERE path = ?", (path,)).fetchone()
        return r["uuid"] if r else None

    def resolve_path(self, uuid):
        """reverse: a plugged-in chip -> which node is it bound to?"""
        r = self._db.execute("SELECT path FROM uuid_binding WHERE uuid = ?", (uuid,)).fetchone()
        return r["path"] if r else None

    def rebind(self, path, uuid, *, vid=None, pid=None):
        """Explicit override of the UNIQUE floor -- the one sanctioned way to
        change a binding (chip swap / RMA / re-cable). Releases any prior claim
        on BOTH this uuid (wherever it was) and this path (whatever it held),
        then binds. bind_uuid refuses these conflicts; rebind performs them
        deliberately."""
        if self.get_node(path) is None:
            raise NamespaceError("cannot bind unknown node %r" % path)
        self._db.execute("DELETE FROM uuid_binding WHERE uuid = ? OR path = ?", (uuid, path))
        self._db.execute(
            "INSERT INTO uuid_binding(path, uuid, vid, pid, bound_at) VALUES (?, ?, ?, ?, ?)",
            (path, uuid, vid, pid, _now_iso()))
        self._db.commit()

    def unbind(self, path):
        """Remove a node's uuid binding (decommission). The namespace node stays;
        only the path<->chip binding is released, freeing the chip to bind elsewhere."""
        self._db.execute("DELETE FROM uuid_binding WHERE path = ?", (path,))
        self._db.commit()

    # -- status (runtime telemetry; control/access-facing) --------------------
    def set_status(self, path, fields, merge=True):
        """Write JSONB runtime status for a node. merge=True (default) shallow-
        merges `fields` into the existing status (a null value deletes a key, per
        JSON merge-patch); merge=False replaces it wholesale. Stamps updated_at."""
        if self.get_node(path) is None:
            raise NamespaceError("cannot set status on unknown node %r" % path)
        js = json.dumps(fields or {}, sort_keys=True)
        if merge:
            patch = "jsonb_patch" if self.jsonb else "json_patch"
            sql = ("INSERT INTO node_status(path, status, updated_at) VALUES (?, %s(?), ?) "
                   "ON CONFLICT(path) DO UPDATE SET "
                   "status = %s(status, excluded.status), updated_at = excluded.updated_at"
                   % (self._jfn, patch))
        else:
            sql = ("INSERT INTO node_status(path, status, updated_at) VALUES (?, %s(?), ?) "
                   "ON CONFLICT(path) DO UPDATE SET "
                   "status = excluded.status, updated_at = excluded.updated_at" % self._jfn)
        self._db.execute(sql, (path, js, _now_iso()))
        self._db.commit()

    def get_status(self, path):
        """Read a node's runtime status dict (None if never written, {} if empty)."""
        r = self._db.execute(
            "SELECT json(status) AS s FROM node_status WHERE path = ?", (path,)).fetchone()
        if r is None:
            return None
        return json.loads(r["s"]) if r["s"] else {}

    def bump_status(self, path, field, by=1):
        """Increment a numeric status counter (e.g. reconnects, wdt_resets),
        creating it at `by` if absent. Returns the new value."""
        cur = self.get_status(path) or {}
        new = (cur.get(field) or 0) + by
        self.set_status(path, {field: new}, merge=True)
        return new

    # -- lifecycle ------------------------------------------------------------
    def close(self):
        self._db.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def _is_descendant(ancestor, path):
    """Pure-Python mirror of ltree_descendant(ancestor, path) for input validation."""
    return path.startswith(ancestor + ".")


# ---------------------------------------------------------------------------
# Self-test: build a subtree, bind a UID, regen with a node removed, prove the
# delete is subtree-scoped and cascades the binding (delete == unbind).
# ---------------------------------------------------------------------------
def selftest():
    import tempfile
    db_path = os.path.join(tempfile.mkdtemp(), "ns.db")
    ns = NamespaceDB(db_path)
    print("sqlite", sqlite3.sqlite_version, "jsonb=", ns.jsonb)

    # Two independent branches under the same root.
    ns.regen_subtree("site.cellA", {
        "site.cellA":        {"kind": "CELL"},
        "site.cellA.estop":  {"kind": "DEV", "attrs": {"cls": "SAMD21", "i2c": 0x20, "mode": "GPIO"}},
        "site.cellA.axis":   {"kind": "DEV", "attrs": {"cls": "SAMD21", "i2c": 0x40, "mode": "SERVO"}},
    })
    ns.regen_subtree("site.cellB", {
        "site.cellB":        {"kind": "CELL"},
        "site.cellB.pump":   {"kind": "DEV", "attrs": {"cls": "SAMD21", "i2c": 0x21, "mode": "MIXED"}},
    })

    ns.bind_uuid("site.cellA.estop", "UID-AAA", vid=0x2886, pid=0x802F)
    ns.bind_uuid("site.cellB.pump",  "UID-BBB", vid=0x2886, pid=0x802F)
    print("cellA nodes:", ns.list_subtree("site.cellA"))
    print("estop uuid :", ns.resolve_uuid("site.cellA.estop"))

    # uniqueness: binding UID-AAA elsewhere must fail.
    try:
        ns.bind_uuid("site.cellA.axis", "UID-AAA")
        print("FAIL: duplicate uuid accepted")
    except UidInUse as e:
        print("uniqueness OK:", e)

    # Regen cellA with estop REMOVED. estop + its binding must vanish; axis kept;
    # cellB must be entirely UNAFFECTED (subtree-scoped).
    r = ns.regen_subtree("site.cellA", {
        "site.cellA":      {"kind": "CELL"},
        "site.cellA.axis": {"kind": "DEV", "attrs": {"cls": "SAMD21", "i2c": 0x40, "mode": "SERVO"}},
    })
    print("regen cellA:", r)
    print("estop after regen:", ns.get_node("site.cellA.estop"), "uuid:", ns.resolve_uuid("site.cellA.estop"))
    print("cellB pump still bound:", ns.resolve_uuid("site.cellB.pump"), "node:", ns.get_node("site.cellB.pump") is not None)

    ok = (ns.get_node("site.cellA.estop") is None
          and ns.resolve_uuid("site.cellA.estop") is None      # cascade unbound
          and ns.resolve_uuid("site.cellB.pump") == "UID-BBB"  # other branch intact
          and ns.get_node("site.cellA.axis") is not None)

    # -- status read/write + merge + counter -------------------------------
    pump = "site.cellB.pump"
    ns.set_status(pump, {"state": "online", "last_seen": "t0"})
    ns.set_status(pump, {"last_seen": "t1"})              # merge: state preserved
    ns.bump_status(pump, "reconnects")
    ns.bump_status(pump, "reconnects")
    st = ns.get_status(pump)
    print("pump status:", st)
    ok &= (st.get("state") == "online" and st.get("last_seen") == "t1"
           and st.get("reconnects") == 2)

    # -- rebind (chip swap) + unbind --------------------------------------
    ns.rebind(pump, "UID-CCC")                            # swap the physical chip
    print("after rebind: pump ->", ns.resolve_uuid(pump),
          " old UID-BBB owner:", ns.resolve_path("UID-BBB"))
    ok &= (ns.resolve_uuid(pump) == "UID-CCC" and ns.resolve_path("UID-BBB") is None)
    ns.unbind(pump)                                       # decommission
    print("after unbind: pump uuid ->", ns.resolve_uuid(pump),
          " node still present:", ns.get_node(pump) is not None)
    ok &= (ns.resolve_uuid(pump) is None and ns.get_node(pump) is not None)

    print("RESULT:", "PASS" if ok else "FAIL")
    ns.close()
    return ok


if __name__ == "__main__":
    sys.exit(0 if selftest() else 1)
