-- ns_selftest.lua -- exercise namespace_db.lua: subtree-scoped regen, delete=unbind
-- cascade, engine UNIQUE, status merge/bump, rebind, unbind.  `luajit ns_selftest.lua`

local _dir = (debug.getinfo(1, "S").source:sub(2)):match("(.*/)") or "./"
package.path = _dir .. "?.lua;" .. package.path
local ndb = require("namespace_db")

local dbpath = "/tmp/ns_lua_selftest.db"
os.remove(dbpath); os.remove(dbpath .. "-wal"); os.remove(dbpath .. "-shm")
local ns = ndb.open(dbpath)
local ok = true
local function check(label, cond) print((cond and "  ok   " or "  FAIL ") .. label); ok = ok and cond end

-- two independent branches
ns:regen_subtree("site.cellA", {
    ["site.cellA"]       = { kind = "CELL" },
    ["site.cellA.estop"] = { kind = "DEV", attrs = { cls = "SAMD21", i2c = 0x20, mode = "GPIO" } },
    ["site.cellA.axis"]  = { kind = "DEV", attrs = { cls = "SAMD21", i2c = 0x40, mode = "SERVO" } },
})
ns:regen_subtree("site.cellB", {
    ["site.cellB"]      = { kind = "CELL" },
    ["site.cellB.pump"] = { kind = "DEV", attrs = { cls = "SAMD21", i2c = 0x21, mode = "MIXED" } },
})

ns:bind_uuid("site.cellA.estop", "UID-AAA", 0x2886, 0x802F)
ns:bind_uuid("site.cellB.pump", "UID-BBB", 0x2886, 0x802F)
print("cellA: " .. table.concat(ns:list_subtree("site.cellA"), ", "))
check("estop bound to UID-AAA", ns:resolve_uuid("site.cellA.estop") == "UID-AAA")

-- engine UNIQUE: rebinding UID-AAA elsewhere must error
local dup_ok = pcall(function() ns:bind_uuid("site.cellA.axis", "UID-AAA") end)
check("duplicate uuid rejected", not dup_ok)

-- subtree-scoped regen: drop estop from cellA -> deleted + cascade-unbound;
-- axis kept; cellB untouched
local r = ns:regen_subtree("site.cellA", {
    ["site.cellA"]      = { kind = "CELL" },
    ["site.cellA.axis"] = { kind = "DEV", attrs = { cls = "SAMD21", i2c = 0x40, mode = "SERVO" } },
})
print("regen cellA: deleted=" .. table.concat(r.deleted, ",") .. " kept=" .. table.concat(r.kept, ","))
check("estop node deleted", ns:get_node("site.cellA.estop") == nil)
check("estop binding cascade-unbound", ns:resolve_uuid("site.cellA.estop") == nil)
check("axis preserved", ns:get_node("site.cellA.axis") ~= nil)
check("cellB.pump untouched (subtree-scoped)", ns:resolve_uuid("site.cellB.pump") == "UID-BBB")

-- status: write + merge + counter
ns:set_status("site.cellB.pump", { state = "online", last_seen = "t0" })
ns:set_status("site.cellB.pump", { last_seen = "t1" })          -- merge: state preserved
ns:bump_status("site.cellB.pump", "reconnects")
ns:bump_status("site.cellB.pump", "reconnects")
local st = ns:get_status("site.cellB.pump")
print("pump status: state=" .. tostring(st.state) .. " last_seen=" .. tostring(st.last_seen) ..
      " reconnects=" .. tostring(st.reconnects))
check("status merge preserved state", st.state == "online")
check("status last_seen updated", st.last_seen == "t1")
check("status counter bumped to 2", st.reconnects == 2)

-- rebind (chip swap) + unbind
ns:rebind("site.cellB.pump", "UID-CCC")
check("rebind -> UID-CCC", ns:resolve_uuid("site.cellB.pump") == "UID-CCC")
check("old UID-BBB released", ns:resolve_path("UID-BBB") == nil)
ns:unbind("site.cellB.pump")
check("unbind clears binding", ns:resolve_uuid("site.cellB.pump") == nil)
check("node survives unbind", ns:get_node("site.cellB.pump") ~= nil)

ns:close()
print("RESULT: " .. (ok and "PASS" or "FAIL"))
os.exit(ok and 0 or 1)
