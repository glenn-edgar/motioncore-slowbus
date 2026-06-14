-- link.lua -- a reconnecting, UID-keyed connection to one SAMD21 dongle.
--
-- The seam between the Fleet and the transport. Today the transport is direct
-- USB-CDC (libcomm); a future PicoLink (I2C via the Pico) implements the same
-- surface so Fleet/Zenoh above don't change. A Link is identified by the chip's
-- stable UID, NOT a ttyACM path -- it re-resolves the port on every (re)connect,
-- so unplug / WDT-reset / reboot (which renumber the port) self-heal.
--
-- Policy: ops auto-retry across one reconnect. Reads are idempotent; a write that
-- raced a disconnect could double-apply on retry -- fine for bench register pokes,
-- revisit for anything with side effects.

local lc = require("libcomm")

local Link = {}
Link.__index = Link

local M = {}

-- M.new(uid, opts)  opts: {timeout=1.0, tries=1}
function M.new(uid, opts)
    opts = opts or {}
    return setmetatable({
        uid = uid, dg = nil, port = nil, up = false,
        timeout = opts.timeout or 1.0,
        tries   = opts.tries or 1,             -- reconnect attempts per op
    }, Link)
end

function Link:_find_port()
    for _, c in ipairs(lc.enumerate()) do
        if c.serial == self.uid then return c.port end
    end
    return nil
end

-- ensure an open connection; returns true if up
function Link:_ensure()
    if self.dg then return true end
    local port = self:_find_port()
    if not port then self.up = false; return false end
    local ok, dg = pcall(lc.open, port, self.timeout)
    if not ok then self.up = false; return false end
    self.dg, self.port, self.up = dg, port, true
    return true
end

function Link:_drop()
    if self.dg then pcall(function() self.dg:close() end); self.dg = nil end
    self.up = false
end

-- run a libcomm Dongle method by name, with reconnect+retry
function Link:_call(method, ...)
    local args = { n = select("#", ...), ... }
    for attempt = 0, self.tries do
        if self:_ensure() then
            local ok, res = pcall(function() return self.dg[method](self.dg, unpack(args, 1, args.n)) end)
            if ok then return res end
            self:_drop()                       -- op failed -> link likely lost
            if attempt >= self.tries then
                error(string.format("link %s: %s failed: %s", self.uid:sub(1, 8), method, tostring(res)))
            end
        elseif attempt >= self.tries then
            error(string.format("link %s DOWN (not enumerated)", self.uid:sub(1, 8)))
        end
        lc.sleep(0.2)
    end
end

-- proxied register/file ops (same surface as libcomm.Dongle)
function Link:reg_read(r)     return self:_call("reg_read", r) end
function Link:reg_write(r, v) return self:_call("reg_write", r, v) end
function Link:reg_readn(r, n) return self:_call("reg_readn", r, n) end
function Link:whoami()        return self:_call("whoami") end
function Link:mode(set)       return self:_call("mode", set) end
function Link:status()        return self:_call("status") end
function Link:chip_uid()      return self:_call("chip_uid") end
function Link:file_list()     return self:_call("file_list") end
function Link:file_get(name)  return self:_call("file_get", name) end

-- is the chip currently present (enumerated) without forcing an open?
function Link:present() return self:_find_port() ~= nil end

function Link:close() self:_drop() end

return M
