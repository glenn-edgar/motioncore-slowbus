-- fleet.lua -- control a SET of SAMD21 dongles directly, BY NAMESPACE NAME.
--
-- The Fleet resolves a namespace path -> UID (namespace_db) -> a reconnecting
-- Link (link.lua) -> the chip. ttyACM numbers are never used or stored. This is
-- step 1 of the controller plan: direct USB fan-out. Step 2 wraps this same Fleet
-- API behind Zenoh; step 3 swaps the Link transport for the Pico/I2C relay.
--
--   luajit fleet.lua --db <db> roster              # all nodes: bound + live presence
--   luajit fleet.lua --db <db> scan                # whoami/mode/status of every present chip
--   luajit fleet.lua --db <db> read  <path> <reg>  # read a register by name
--   luajit fleet.lua --db <db> write <path> <reg> <val>
--   luajit fleet.lua --db <db> int   <path>        # read the D6 interlock line (reg 0x3A)

local _dir = (debug.getinfo(1, "S").source:sub(2)):match("(.*/)") or "./"
package.path = _dir .. "?.lua;" .. package.path
local ndb   = require("namespace_db")
local Link  = require("link")
local lc    = require("libcomm")
local Bench = require("bench")

local Fleet = {}
Fleet.__index = Fleet
local M = {}

function M.open(db, root)
    local ns = ndb.open(db)
    return setmetatable({ ns = ns, root = root or "slow_bus", links = {} }, Fleet)
end

function Fleet:resolve(path)
    local uid = self.ns:resolve_uuid(path)
    if not uid then error("no UID binding for node " .. path) end
    return uid
end

-- name -> Link (one cached Link per UID, shared across paths if ever aliased)
function Fleet:link(path)
    local uid = self:resolve(path)
    if not self.links[uid] then self.links[uid] = Link.new(uid) end
    return self.links[uid]
end

-- name-based ops
function Fleet:read(path, reg)       return self:link(path):reg_read(reg) end
function Fleet:write(path, reg, val) return self:link(path):reg_write(reg, val) end
function Fleet:mode(path)            return self:link(path):mode() end
function Fleet:status(path)          return self:link(path):status() end
function Fleet:int_line(path)        return self:link(path):reg_read(0x3A) end   -- D6 interlock line
function Fleet:bench(path)           return Bench.new(self:link(path)) end       -- per-pin bench by name

-- roster + reconcile: every DEV node, its binding, and live presence.
function Fleet:roster()
    local present = {}
    for _, c in ipairs(lc.enumerate()) do present[c.serial] = c.port end
    local out = {}
    for _, p in ipairs(self.ns:list_subtree(self.root)) do
        local n = self.ns:get_node(p)
        if n.kind == "DEV" then
            local uid = self.ns:resolve_uuid(p)
            out[#out + 1] = {
                path = p, mode = n.attrs.sub, uid = uid,
                present = (uid and present[uid]) and true or false,
                port = uid and present[uid] or nil,
            }
        end
    end
    return out
end

function Fleet:close()
    for _, l in pairs(self.links) do l:close() end
    self.ns:close()
end

-- ---- CLI ------------------------------------------------------------------
if arg and (arg[0] or ""):match("fleet%.lua$") then
    local opt, pos = {}, {}
    local i = 1
    while arg[i] do
        if arg[i] == "--db" then i = i + 1; opt.db = arg[i]
        elseif arg[i] == "--root" then i = i + 1; opt.root = arg[i]
        else pos[#pos + 1] = arg[i] end
        i = i + 1
    end
    if not opt.db then io.stderr:write("fleet: --db <namespace.db> required\n"); os.exit(2) end
    local cmd = pos[1]
    local f = M.open(opt.db, opt.root)
    local function num(s) return tonumber(s, (s:match("^0[xX]")) and 16 or 10) end

    if cmd == "roster" then
        for _, r in ipairs(f:roster()) do
            print(string.format("  %-26s %-7s %-10s uid=%s", r.path, r.mode,
                r.present and ("UP " .. r.port) or "missing", tostring(r.uid or "(unbound)")))
        end
    elseif cmd == "scan" then
        for _, r in ipairs(f:roster()) do
            if r.present then
                local l = f:link(r.path)
                local ok, info = pcall(function()
                    return string.format("whoami=0x%02X mode=%d status=0x%02X int(D6)=%s",
                        l:whoami(), l:mode(), l:status(),
                        pcall(function() return l:reg_read(0x3A) end) and tostring(l:reg_read(0x3A)) or "n/a")
                end)
                print(string.format("  %-26s %s", r.path, ok and info or ("ERR " .. tostring(info))))
            else
                print(string.format("  %-26s missing", r.path))
            end
        end
    elseif cmd == "read" then
        print(string.format("0x%02X", f:read(pos[2], num(pos[3]))))
    elseif cmd == "write" then
        f:write(pos[2], num(pos[3]), num(pos[4])); print("ok")
    elseif cmd == "int" then
        local v = f:int_line(pos[2])
        print(string.format("%s D6=%d (%s)", pos[2], v, v == 1 and "released/clear" or "asserted/FIRED"))
    else
        io.stderr:write("usage: fleet.lua --db <db> {roster|scan|read <path> <reg>|write <path> <reg> <val>|int <path>}\n")
        os.exit(2)
    end
    f:close()
end

return M
