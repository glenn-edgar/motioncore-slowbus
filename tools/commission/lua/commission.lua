-- commission.lua -- namespace-driven SAMD21 commissioning (pure LuaJIT).
--
--   luajit commission.lua --db <db> <namespace.path> [options]
--
-- Flow (per spec):
--   * scan ttyACM for SAMD21s: 0 -> error; >1 -> error + show each chip's state;
--     exactly 1 -> proceed.
--   * determine the chip's commission state (mode != IDLE = commissioned) and its
--     namespace binding.
--   * guards: an already-commissioned chip warns + y/N confirm; a REPURPOSE (this
--     UID is bound to another node, or the target node holds another UID) requires
--     the explicit --rebind flag and flags the orphaned source node needs_chip.
--   * push (offline -> format -> file_put), READ EACH FILE BACK and byte-compare,
--     reboot, verify the mode applied, then bind_uuid/rebind + set_status.
--
-- Options: --rebind  --no-erase  --yes (skip confirm)  --settle N  --port P
--
-- The compiled on-chip bytes come from the Lua slave_dsl, recompiled from the
-- node's stored config (one source of truth).

local _dir = (debug.getinfo(1, "S").source:sub(2)):match("(.*/)") or "./"
package.path = _dir .. "?.lua;" .. package.path
local lc    = require("libcomm")
local ndb   = require("namespace_db")
local sd    = require("slave_dsl")
local flash = require("flash")

-- ---- args -----------------------------------------------------------------
local opt = { settle = 9 }
local path
do
    local i = 1
    while arg[i] do
        local a = arg[i]
        if a == "--db" then i = i + 1; opt.db = arg[i]
        elseif a == "--rebind" then opt.rebind = true
        elseif a == "--reflash" then opt.reflash = true
        elseif a == "--no-erase" then opt.no_erase = true
        elseif a == "--yes" then opt.yes = true
        elseif a == "--port" then i = i + 1; opt.port = arg[i]
        elseif a == "--settle" then i = i + 1; opt.settle = tonumber(arg[i])
        elseif a:sub(1, 2) == "--" then io.stderr:write("unknown option " .. a .. "\n"); os.exit(2)
        else path = a end
        i = i + 1
    end
end
local function die(...) io.stderr:write("commission: " .. string.format(...) .. "\n"); os.exit(1) end
if not opt.db then die("--db <namespace.db> required") end
if not path then die("usage: commission.lua --db <db> <namespace.path> [--rebind] [--no-erase] [--yes]") end

local function sleep(s) os.execute("sleep " .. tostring(s)) end

local FILE_ORDER = { "idnt", "gpmp", "cntr", "srvo", "ilcf" }   -- idnt first (sets mode)
local function ordered(files)
    local seen, out = {}, {}
    for _, k in ipairs(FILE_ORDER) do if files[k] then out[#out+1] = k; seen[k] = true end end
    for k in pairs(files) do if not seen[k] then out[#out+1] = k end end
    return out
end

local function compile_files(a)
    local u = sd.Unit(a.i2c, a.sub)
    if a.sub == "COUNTER" then u:counter(a.rate or 1000) end
    if a.sub == "SERVO" then u:servo() end
    if a.pins then u:pins(a.pins) end
    if a.interlock then u:interlock(a.interlock.name, a.interlock.expr, a.interlock.drive) end
    return u:files()
end

local function hex(s) local t = {}; for i = 1, #s do t[i] = string.format("%02x", s:byte(i)) end; return table.concat(t) end

-- open a chip, read its commission state, close. Returns a table (best-effort).
local function probe(port, uid, ns)
    local st = { port = port, uid = uid }
    local ok, dg = pcall(lc.open, port, 1.0)
    if not ok then st.error = tostring(dg); return st end
    pcall(function()
        st.who = dg:whoami()
        st.mode = dg:mode()
        st.mode_name = lc.MODE_NAME[st.mode] or ("?" .. st.mode)
        st.commissioned = (st.mode ~= 0)
        local fl, names = dg:file_list(), {}
        for _, f in ipairs(fl) do names[#names+1] = f.name end
        st.files = names
    end)
    dg:close()
    st.bound_node = ns:resolve_path(uid)
    return st
end

-- ---- main -----------------------------------------------------------------
local ns = ndb.open(opt.db)

local node = ns:get_node(path)
if not node then die("no namespace node %q (build the DSL first)", path) end
if (node.attrs.cls or "") ~= "SAMD21" then die("node %q is cls=%s, not SAMD21", path, tostring(node.attrs.cls)) end
local files = compile_files(node.attrs)
local expect_mode = sd.MODES[node.attrs.sub]
io.write(string.format("target %s : %s i2c=0x%02X files=[%s]\n",
    path, node.attrs.sub, node.attrs.i2c, table.concat(ordered(files), ",")))

-- scan
local chips = lc.enumerate()
if opt.port then
    local keep = {}
    for _, c in ipairs(chips) do if c.port == opt.port then keep[#keep+1] = c end end
    chips = keep
end
if #chips == 0 then
    -- no app-mode chip, but one may be sitting in the bootloader (002f + drive)
    -- after a double-tap -- flash it and continue.
    local c = flash.flash_from_bootloader()
    if c then chips = { c }
    else die("no SAMD21 found (enumerated as 2886:802f, or double-tapped into the bootloader?)") end
end
if #chips > 1 then
    io.stderr:write(string.format("commission: %d SAMD21 chips present -- connect exactly one.\n", #chips))
    for _, c in ipairs(chips) do
        local s = probe(c.port, c.serial, ns)
        io.stderr:write(string.format("  %s  uid=%s  %s  files=[%s]  bound=%s\n",
            c.port, tostring(c.uid or c.serial),
            s.commissioned and ("commissioned:" .. tostring(s.mode_name)) or "fresh",
            table.concat(s.files or {}, ","), tostring(s.bound_node or "-")))
    end
    os.exit(1)
end

local chip = chips[1]
-- ensure the chip runs the current firmware (real unique serial + register
-- interface); reflash from the vendored UF2 if needed or if --reflash. May
-- prompt for a physical RST double-tap on old/blank chips, and may change the
-- port/serial. The post-flash serial is the UID we bind.
chip = flash.ensure(chip, opt.reflash)
local uid = chip.serial
local dg = lc.open(chip.port, 1.5)
if dg:whoami() ~= lc.WHO_AM_I_EXPECTED then dg:close(); die("whoami != 0x5A on %s -- wrong device?", chip.port) end
local cur_mode = dg:mode()
local committed = (cur_mode ~= 0)
local cur_files = dg:file_list()

-- guards: repurpose (KB binding move) requires --rebind
local x_node = ns:resolve_path(uid)               -- this chip currently bound to?
local n_uid  = ns:resolve_uuid(path)              -- target node currently holds?
local repurpose = (x_node and x_node ~= path) or (n_uid and n_uid ~= uid)
if repurpose and not opt.rebind then
    dg:close()
    io.stderr:write("commission: this is a REPURPOSE -- pass --rebind to proceed.\n")
    if x_node and x_node ~= path then
        io.stderr:write(string.format("  chip %s is currently the %q unit (it would be moved here, leaving %q with no chip)\n", uid, x_node, x_node))
    end
    if n_uid and n_uid ~= uid then
        io.stderr:write(string.format("  node %q currently holds chip %s (it would be unbound/freed)\n", path, n_uid))
    end
    os.exit(1)
end

-- guard: already-commissioned chip -> warn + confirm
if committed and not opt.yes then
    local fl = {}; for _, f in ipairs(cur_files) do fl[#fl+1] = f.name end
    io.write(string.format("\nWARNING: chip %s is already commissioned: mode=%s, files=[%s], bound=%s\n",
        uid, lc.MODE_NAME[cur_mode] or cur_mode, table.concat(fl, ","), tostring(x_node or "-")))
    io.write("Its flash will be reformatted and rewritten as " .. node.attrs.sub .. ". Continue? [y/N] ")
    io.flush()
    local ans = io.read("*l")
    if not ans or not ans:lower():match("^y") then dg:close(); die("aborted") end
end

-- push
io.write("pushing...\n")
dg:offline()
if not opt.no_erase then dg:file_format() end
for _, k in ipairs(ordered(files)) do dg:file_put(k, files[k]) end

-- read back + byte-compare (still offline, same connection)
io.write("verifying (read-back)...\n")
local verify_ok = true
for _, k in ipairs(ordered(files)) do
    local got = dg:file_get(k)
    local ok = (got == files[k])
    io.write(string.format("  %-5s %s\n", k, ok and "OK" or ("MISMATCH got=" .. hex(got) .. " want=" .. hex(files[k]))))
    verify_ok = verify_ok and ok
end
dg:close()                       -- reboot -> apply config
if not verify_ok then die("read-back mismatch -- NOT binding") end

-- reopen by UID (port may renumber) and verify the mode applied
io.write(string.format("settling %ds, reopening by uid...\n", opt.settle))
sleep(opt.settle)
local newport
for _ = 1, 6 do
    for _, c in ipairs(lc.enumerate()) do if c.serial == uid then newport = c.port; break end end
    if newport then break end
    sleep(0.5)
end
if not newport then die("chip %s did not re-enumerate after reboot", uid) end
local dg2 = lc.open(newport, 1.5)
local applied = dg2:mode()
dg2:close()
local mode_ok = (applied == expect_mode)
io.write(string.format("applied mode=%s (expect %s) -> %s\n",
    lc.MODE_NAME[applied] or applied, lc.MODE_NAME[expect_mode] or expect_mode, mode_ok and "OK" or "MISMATCH"))

-- KB update
if repurpose then
    if x_node and x_node ~= path then
        ns:set_status(x_node, { state = "uncommissioned", needs_chip = true })   -- orphaned source
    end
    ns:rebind(path, uid, 0x2886, 0x802F)
else
    ns:bind_uuid(path, uid, 0x2886, 0x802F)
end
ns:set_status(path, { state = mode_ok and "commissioned" or "mode-mismatch",
                      mode = applied, files = ordered(files), verified = verify_ok,
                      uid = uid, last_seen = os.date("%Y-%m-%dT%H:%M:%S") })
ns:close()

io.write(string.format("\nDONE: %s <- chip %s  (%s)\n", path, uid,
    (verify_ok and mode_ok) and "verified" or "WARNING: check above"))
os.exit((verify_ok and mode_ok) and 0 or 1)
