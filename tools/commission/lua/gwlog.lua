-- gwlog.lua -- host API for the SAMD21 gateway's SD-card text logger.
--
-- Speaks the gateway shell (libcomm) directly: the CMD_LOG_* surface plus a
-- CMD_I2C_WRITE passthrough to set the PCF8563 RTC. Sets the RTC from the host
-- clock ON CONNECT by default (the XIAO base RTC has no battery backup, so it
-- resets to garbage on every power cycle).
--
--   local gw = require("gwlog").open("/dev/ttyACM0")   -- connects + sets the RTC
--   gw:write("hello")                                  -- append "<ts> hello\n"
--   io.write(gw:read_all("LOG.TXT"))                   -- read a whole file
--   gw:disconnect()
--
-- CLI:
--   luajit gwlog.lua [--port P] [--no-rtc] scan
--   luajit gwlog.lua [--port P] settime
--   luajit gwlog.lua [--port P] write FILE "message"
--   luajit gwlog.lua [--port P] cat   FILE
--   luajit gwlog.lua [--port P] rm    FILE
--   luajit gwlog.lua [--port P] demo            -- set RTC + 3 writes + cat

local _dir = (debug.getinfo(1, "S").source:sub(2)):match("(.*/)") or "./"
package.path = _dir .. "?.lua;" .. package.path
local lc = require("libcomm")

-- gateway shell opcodes (app/samd21_client/shell_commands.h)
local CMD_I2C_WRITE = 0x0130
local CMD_I2C_SCAN  = 0x0133
local CMD_LOG_OPEN, CMD_LOG_CLOSE, CMD_LOG_WRITE, CMD_LOG_READ, CMD_LOG_DELETE =
      0x0134, 0x0135, 0x0136, 0x0137, 0x0138
local OK = 0
local READ_CHUNK = 60   -- gateway result-budget cap per LOG_READ

local Gateway = {}
Gateway.__index = Gateway
local M = {}

local function bcd(n) return math.floor(n / 10) * 16 + (n % 10) end
local function le32(v)
    return string.char(v % 256, math.floor(v / 256) % 256,
                       math.floor(v / 65536) % 256, math.floor(v / 16777216) % 256)
end

-- open the gateway and (unless opts.set_rtc == false) set its RTC from host time
function M.open(port, opts)
    opts = opts or {}
    if not port then
        local ch = lc.enumerate()
        if #ch ~= 1 then error(string.format("pass a port -- %d devices on USB", #ch)) end
        port = ch[1].port
    end
    local gw = setmetatable({ dg = lc.open(port), port = port }, Gateway)
    if opts.set_rtc ~= false then gw:set_rtc() end
    return gw
end

function Gateway:disconnect() self.dg:close() end

-- set the PCF8563 (0x51) from host time (or a given os.date table), over the
-- gateway's I2C passthrough. Clears STOP/VL so the clock runs and is trusted.
function Gateway:set_rtc(t)
    t = t or os.date("*t")
    local st = self.dg:shell_exec(CMD_I2C_WRITE, string.char(0x51, 0x00, 0x00, 0x00))  -- CTRL1/2 = run
    if st ~= OK then error("set_rtc CTRL i2c_write status " .. st) end
    st = self.dg:shell_exec(CMD_I2C_WRITE, string.char(0x51, 0x02,                      -- reg 0x02 = seconds
        bcd(t.sec), bcd(t.min), bcd(t.hour), bcd(t.day),
        (t.wday - 1) % 8, bcd(t.month), bcd(t.year % 100)))
    if st ~= OK then error("set_rtc time i2c_write status " .. st) end
    return t
end

function Gateway:scan()
    local st, r = self.dg:shell_exec(CMD_I2C_SCAN, "")
    if st ~= OK then error("i2c_scan status " .. st) end
    local a = {}; for i = 1, #r do a[i] = r:byte(i) end; return a
end

-- select (create) the active file; returns its current size in bytes
function Gateway:open(name)
    local st, r = self.dg:shell_exec(CMD_LOG_OPEN, name)
    if st ~= OK then error(string.format("log_open '%s' status %d", name, st)) end
    return r:byte(1) + (r:byte(2) or 0) * 256 + (r:byte(3) or 0) * 65536 + (r:byte(4) or 0) * 16777216
end

function Gateway:close()   -- deselect the active file (LOG_CLOSE)
    local st = self.dg:shell_exec(CMD_LOG_CLOSE)
    if st ~= OK then error("log_close status " .. st) end
end

-- append a record: the gateway prepends "<YYYY-MM-DD HH:MM:SS> " and a newline
function Gateway:write(text)
    if #text > 200 then error("log text > 200 bytes") end
    local st = self.dg:shell_exec(CMD_LOG_WRITE, text)
    if st ~= OK then error("log_write status " .. st) end
end

-- raw offset read; returns up to len bytes (short/zero = EOF)
function Gateway:read(off, len)
    if len > READ_CHUNK then len = READ_CHUNK end
    local st, r = self.dg:shell_exec(CMD_LOG_READ, le32(off) .. string.char(len))
    if st ~= OK then error("log_read status " .. st) end
    return r
end

-- open `name` and read the whole file (paginated to EOF)
function Gateway:read_all(name)
    local sz = self:open(name)
    local parts, off = {}, 0
    while off < sz do
        local b = self:read(off, READ_CHUNK)
        if #b == 0 then break end
        parts[#parts + 1] = b; off = off + #b
    end
    return table.concat(parts)
end

function Gateway:delete(name)
    local st = self.dg:shell_exec(CMD_LOG_DELETE, name)
    if st ~= OK then error(string.format("log_delete '%s' status %d", name, st)) end
end

M.Gateway = Gateway

-- ---- CLI --------------------------------------------------------------------
if arg and (arg[0] or ""):match("gwlog%.lua$") then
    local port, set_rtc, rest = nil, true, {}
    local i = 1
    while arg[i] do
        if     arg[i] == "--port"   then i = i + 1; port = arg[i]
        elseif arg[i] == "--no-rtc" then set_rtc = false
        else rest[#rest + 1] = arg[i] end
        i = i + 1
    end
    local cmd = rest[1] or "scan"
    local gw = M.open(port, { set_rtc = set_rtc })
    print(string.format("gateway on %s%s", gw.port, set_rtc and " (RTC set from host)" or ""))

    if cmd == "scan" then
        io.write("i2c devices:"); for _, a in ipairs(gw:scan()) do io.write(string.format(" 0x%02X", a)) end; print()
    elseif cmd == "settime" then
        print("RTC set -> " .. os.date("%Y-%m-%d %H:%M:%S"))
    elseif cmd == "write" then
        local f, msg = rest[2], rest[3]
        if not f or not msg then io.stderr:write("usage: write FILE \"message\"\n"); os.exit(2) end
        gw:open(f); gw:write(msg); gw:close()
        print(string.format("appended to %s", f))
    elseif cmd == "cat" then
        if not rest[2] then io.stderr:write("usage: cat FILE\n"); os.exit(2) end
        io.write(gw:read_all(rest[2]))
    elseif cmd == "rm" then
        if not rest[2] then io.stderr:write("usage: rm FILE\n"); os.exit(2) end
        gw:delete(rest[2]); print("deleted " .. rest[2])
    elseif cmd == "demo" then
        pcall(function() gw:delete("DEMO.LOG") end)   -- best-effort clean start (ok if absent)
        gw:open("DEMO.LOG")
        for _, m in ipairs({ "host wrappers online", "rtc set on connect", "gateway logging" }) do gw:write(m) end
        gw:close()
        print("--- DEMO.LOG ---"); io.write(gw:read_all("DEMO.LOG")); print("---")
    else
        io.stderr:write("unknown cmd: " .. cmd .. "\n"); os.exit(2)
    end
    gw:disconnect()
end

return M
