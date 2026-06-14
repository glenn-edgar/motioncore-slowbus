-- namespace_db.lua -- the namespace DB layer (LuaJIT FFI + system libsqlite3 + ltree).
--
-- The full-LuaJIT port of the Python namespace_db: the one authoritative store
-- the host tooling (standalone AND the Zenoh container) is a client of. No pip,
-- no pysqlite3, no version gate -- LuaJIT FFI binds the host's libsqlite3.so.0
-- (present on every Linux), JSON is encoded in Lua (TEXT columns), and ltree.so
-- gives the subtree-scoping operators. Names are the handle; the UID is an
-- internal column resolved name->uuid here, uuid->ttyACM live by the link layer.
--
-- Tables (mirrors the Python layer):
--   ns_node       path -> JSON config attrs (TEXT). DSL-constructed, regenerable.
--   uuid_binding  PK(path) + UNIQUE(uuid), FK->ns_node ON DELETE CASCADE.
--   node_status   JSON runtime telemetry (TEXT), FK-cascaded.

local _src = debug.getinfo(1, "S").source:sub(2)        -- strip leading '@'
local _dir = _src:match("(.*/)") or "./"
package.path = _dir .. "?.lua;" .. package.path

local h = require("sqlite3_helpers")
local ffi = h.ffi
local lib = h.sqlite3_lib
local json = h.json

local LTREE_SO = _dir .. "../vendor/ltree/ltree.so"
local SCHEMA_VERSION = 1

local M = {}
local NS = {}
NS.__index = NS

-- ---- helpers --------------------------------------------------------------
local function now() return os.date("%Y-%m-%dT%H:%M:%S") end

local function startswith(s, prefix) return s:sub(1, #prefix) == prefix end

local function depth(path)                              -- ltree depth = #components
    local n = 1
    for _ in path:gmatch("%.") do n = n + 1 end
    return n
end

local function q(self, sql, params) return h.sql_query(self.db, sql, params) end

-- ---- open / schema --------------------------------------------------------
function M.open(path)
    local pdb = ffi.new("sqlite3*[1]")
    if lib.sqlite3_open(path, pdb) ~= 0 then
        error("sqlite3_open(" .. path .. ") failed")
    end
    local self = setmetatable({ db = pdb[0] }, NS)

    -- load the ltree extension (entry point derived from the filename)
    lib.sqlite3_enable_load_extension(self.db, 1)
    local errp = ffi.new("char*[1]")
    if lib.sqlite3_load_extension(self.db, LTREE_SO, nil, errp) ~= 0 then
        local msg = errp[0] ~= nil and ffi.string(errp[0]) or "?"
        error("load ltree.so failed: " .. msg .. " (" .. LTREE_SO .. ")")
    end
    lib.sqlite3_enable_load_extension(self.db, 0)
    if q(self, "SELECT ltree_descendant('a','a.b') AS d")[1].d ~= 1 then
        error("ltree loaded but ltree_descendant misbehaves")
    end

    h.sql_exec(self.db, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA busy_timeout=5000;")
    self:_init_schema()
    return self
end

function NS:_init_schema()
    h.sql_exec(self.db, [[
        CREATE TABLE IF NOT EXISTS ns_meta (key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE IF NOT EXISTS ns_node (
            path TEXT PRIMARY KEY, depth INTEGER NOT NULL, kind TEXT, attrs TEXT);
        CREATE INDEX IF NOT EXISTS idx_ns_node_depth ON ns_node(depth);
        CREATE TABLE IF NOT EXISTS uuid_binding (
            path TEXT PRIMARY KEY REFERENCES ns_node(path) ON DELETE CASCADE,
            uuid TEXT NOT NULL UNIQUE, vid INTEGER, pid INTEGER, bound_at TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS node_status (
            path TEXT PRIMARY KEY REFERENCES ns_node(path) ON DELETE CASCADE,
            status TEXT, updated_at TEXT);
    ]])
    local r = q(self, "SELECT value FROM ns_meta WHERE key='schema_version'")
    if not r[1] then
        q(self, "INSERT INTO ns_meta(key,value) VALUES('schema_version',?)", { tostring(SCHEMA_VERSION) })
    elseif tonumber(r[1].value) ~= SCHEMA_VERSION then
        error("schema_version " .. r[1].value .. " != " .. SCHEMA_VERSION .. " (migration needed)")
    end
end

function NS:close() lib.sqlite3_close(self.db) end

-- ---- namespace construction (DSL-facing) ----------------------------------
-- declared = { [path] = { kind = str, attrs = table }, ... }; every path must be
-- root or a descendant of root. Subtree-scoped: deletes only orphans under root
-- (cascade -> unbind), adds new nodes blank, preserves+updates the rest.
function NS:regen_subtree(root, declared)
    for p in pairs(declared) do
        if p ~= root and not startswith(p, root .. ".") then
            error("declared path outside subtree " .. root .. ": " .. p)
        end
    end
    local existing = {}
    for _, r in ipairs(q(self,
        "SELECT path FROM ns_node WHERE path=? OR ltree_descendant(?,path)=1", { root, root })) do
        existing[r.path] = true
    end
    local to_delete, to_add, to_keep = {}, {}, {}
    for p in pairs(existing) do if not declared[p] then to_delete[#to_delete + 1] = p end end
    for p in pairs(declared) do
        if existing[p] then to_keep[#to_keep + 1] = p else to_add[#to_add + 1] = p end
    end
    for _, p in ipairs(to_delete) do
        q(self, "DELETE FROM ns_node WHERE path=?", { p })   -- FK cascade unbinds
    end
    table.sort(to_add, function(a, b) return depth(a) < depth(b) end)
    for _, p in ipairs(to_add) do
        local s = declared[p]
        q(self, "INSERT INTO ns_node(path,depth,kind,attrs) VALUES(?,?,?,?)",
            { p, depth(p), s.kind or "", json.encode(s.attrs or {}) })
    end
    for _, p in ipairs(to_keep) do
        local s = declared[p]
        q(self, "UPDATE ns_node SET kind=?, attrs=? WHERE path=?",
            { s.kind or "", json.encode(s.attrs or {}), p })
    end
    table.sort(to_delete); table.sort(to_keep)
    return { added = to_add, deleted = to_delete, kept = to_keep }
end

function NS:get_node(path)
    local r = q(self, "SELECT path,depth,kind,attrs FROM ns_node WHERE path=?", { path })
    if not r[1] then return nil end
    local row = r[1]
    return { path = row.path, depth = row.depth, kind = row.kind,
             attrs = row.attrs and json.decode(row.attrs) or {} }
end

function NS:list_subtree(root)
    local out = {}
    for _, r in ipairs(q(self,
        "SELECT path FROM ns_node WHERE path=? OR ltree_descendant(?,path)=1 ORDER BY depth,path",
        { root, root })) do
        out[#out + 1] = r.path
    end
    return out
end

-- ---- uuid binding ---------------------------------------------------------
function NS:resolve_uuid(path)
    local r = q(self, "SELECT uuid FROM uuid_binding WHERE path=?", { path })
    return r[1] and r[1].uuid or nil
end

function NS:resolve_path(uuid)
    local r = q(self, "SELECT path FROM uuid_binding WHERE uuid=?", { uuid })
    return r[1] and r[1].path or nil
end

function NS:bind_uuid(path, uuid, vid, pid)
    if not self:get_node(path) then error("cannot bind unknown node " .. path) end
    local cur = q(self, "SELECT uuid FROM uuid_binding WHERE path=?", { path })
    if cur[1] and cur[1].uuid ~= uuid then
        error(path .. " already bound to " .. cur[1].uuid .. "; use rebind")
    end
    local owner = self:resolve_path(uuid)
    if owner and owner ~= path then
        error("uuid " .. uuid .. " already bound to " .. owner .. "; use rebind")
    end
    q(self, "INSERT INTO uuid_binding(path,uuid,vid,pid,bound_at) VALUES(?,?,?,?,?) " ..
        "ON CONFLICT(path) DO UPDATE SET uuid=excluded.uuid, vid=excluded.vid, " ..
        "pid=excluded.pid, bound_at=excluded.bound_at",
        { path, uuid, vid or 0, pid or 0, now() })
end

function NS:rebind(path, uuid, vid, pid)
    if not self:get_node(path) then error("cannot bind unknown node " .. path) end
    q(self, "DELETE FROM uuid_binding WHERE uuid=? OR path=?", { uuid, path })
    q(self, "INSERT INTO uuid_binding(path,uuid,vid,pid,bound_at) VALUES(?,?,?,?,?)",
        { path, uuid, vid or 0, pid or 0, now() })
end

function NS:unbind(path)
    q(self, "DELETE FROM uuid_binding WHERE path=?", { path })
end

-- ---- status ---------------------------------------------------------------
function NS:set_status(path, fields, merge)
    if not self:get_node(path) then error("cannot set status on unknown node " .. path) end
    local cur = {}
    if merge ~= false then cur = self:get_status(path) or {} end
    for k, v in pairs(fields or {}) do cur[k] = v end        -- Lua-side merge (no json_patch needed)
    q(self, "INSERT INTO node_status(path,status,updated_at) VALUES(?,?,?) " ..
        "ON CONFLICT(path) DO UPDATE SET status=excluded.status, updated_at=excluded.updated_at",
        { path, json.encode(cur), now() })
end

function NS:get_status(path)
    local r = q(self, "SELECT status FROM node_status WHERE path=?", { path })
    if not r[1] then return nil end
    return r[1].status and json.decode(r[1].status) or {}
end

function NS:bump_status(path, field, by)
    by = by or 1
    local cur = self:get_status(path) or {}
    local newv = (cur[field] or 0) + by
    self:set_status(path, { [field] = newv }, true)
    return newv
end

return M
