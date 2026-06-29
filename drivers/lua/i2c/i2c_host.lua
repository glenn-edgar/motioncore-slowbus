-- i2c_host.lua -- host-side I2C bus over a USB->I2C front-end.
--
-- The Lua half of the driver-library mirror: it exposes the SAME three
-- primitives the C `i2c_bus.h` handle does (write / read / write_read), so a
-- Lua device driver runs the identical register logic its C twin does and ONLY
-- the transport differs. Two USB->I2C front-ends are supported behind this one
-- Bus interface, both tunnelled over USB-CDC by libcomm's OP_SHELL_EXEC:
--   "pico"   -- slow_bus Pico bus controller   (app/bus_controller, CMD_I2C_*)
--   "samd21" -- SAMD21 USB<->I2C bridge         (app/samd21_client, CMD_I2C_*)
-- They differ ONLY in opcodes, the write_read framing, and transfer caps -- the
-- device drivers above never see it.
--
--   local i2c = require("i2c_host")
--   local bus = i2c.open("/dev/ttyACM0")            -- pico (default)
--   local bus = i2c.open("/dev/ttyACM0", "samd21")  -- SAMD21 bridge
--   for _, a in ipairs(bus:scan()) do print(string.format("0x%02X", a)) end
--   local s = bus:write_read(0x51, string.char(0x02), 7)   -- write reg ptr, read 7

local _dir = (debug.getinfo(1, "S").source:sub(2)):match("(.*/)") or "./"
local _root = _dir .. "../"                        -- drivers/lua/ (each driver is one level under)
package.path = _root .. "i2c/?.lua;" .. _root .. "pcf8563/?.lua;" .. _root .. "ssd1306/?.lua;"
    .. _root .. "../../tools/commission/lua/?.lua;" .. package.path
local lc = require("libcomm")

local OK = 0   -- SHELL_OK (3 = SHELL_IO_ERROR = NACK / bus timeout)

-- Per-front-end opcode set + write_read framing + transfer caps. write_read is
-- the only payload that differs: the Pico carries [addr,rlen,wdata...]; the
-- SAMD21 carries [addr,wlen,rlen,wdata...].
local BACKENDS = {
    pico = {   -- app/bus_controller/main.c
        SCAN = 0x010C, WRITE = 0x010D, READ = 0x010E, WRITE_READ = 0x010F,
        max_write = 63, max_read = 64,
        wr = function(addr, wdata, n) return string.char(addr, n) .. wdata end,
    },
    samd21 = { -- app/samd21_client/samd21_commands.c
        SCAN = 0x0133, WRITE = 0x0130, READ = 0x0131, WRITE_READ = 0x0132,
        max_write = 32, max_read = 60,
        wr = function(addr, wdata, n) return string.char(addr, #wdata, n) .. wdata end,
    },
}

local Bus = {}
Bus.__index = Bus
local M = {}

-- open a USB->I2C front-end. kind = "pico" (default) | "samd21".
-- port nil => auto-pick when exactly one device is on USB.
function M.open(port, kind)
    local be = BACKENDS[kind or "pico"]
    if not be then error("unknown i2c front-end: " .. tostring(kind)) end
    if not port then
        local ch = lc.enumerate()
        if #ch ~= 1 then
            error(string.format("pass a port -- %d devices on USB", #ch))
        end
        port = ch[1].port
    end
    return setmetatable({ dg = lc.open(port), port = port, be = be,
                          max_write = be.max_write, max_read = be.max_read }, Bus)
end
function M.enumerate() return lc.enumerate() end
function Bus:close() self.dg:close() end

local function tobytes(s) local t = {}; for i = 1, #s do t[i] = s:byte(i) end; return t end

-- probe 0x08..0x77; returns a list of 7-bit addresses that ACKed
function Bus:scan()
    local st, r = self.dg:shell_exec(self.be.SCAN, "")
    if st ~= OK then error("i2c scan status " .. st) end
    return tobytes(r)
end

-- write a string of bytes to addr (raw; caller prepends any register pointer)
function Bus:write(addr, data)
    local st = self.dg:shell_exec(self.be.WRITE, string.char(addr) .. data)
    if st ~= OK then error(string.format("i2c write 0x%02X status %d", addr, st)) end
end

-- read n bytes from addr
function Bus:read(addr, n)
    local st, r = self.dg:shell_exec(self.be.READ, string.char(addr, n))
    if st ~= OK then error(string.format("i2c read 0x%02X status %d", addr, st)) end
    return r
end

-- write wdata (no STOP) then repeated-START read n bytes -- the register-read idiom.
function Bus:write_read(addr, wdata, n)
    local st, r = self.dg:shell_exec(self.be.WRITE_READ, self.be.wr(addr, wdata, n))
    if st ~= OK then error(string.format("i2c write_read 0x%02X status %d", addr, st)) end
    return r
end

M.Bus = Bus

-- ---- CLI: bus scan ----------------------------------------------------------
if arg and (arg[0] or ""):match("i2c_host%.lua$") then
    local port, kind
    local i = 1
    while arg[i] do
        if arg[i] == "--port" then i = i + 1; port = arg[i]
        elseif arg[i] == "--kind" then i = i + 1; kind = arg[i] end
        i = i + 1
    end
    local bus = M.open(port, kind)
    print(string.format("%s front-end on %s", kind or "pico", bus.port))
    local addrs = bus:scan()
    io.write("i2c scan: ")
    if #addrs == 0 then io.write("(no devices ACKed -- check wiring: GP10=SDA GP11=SCL, GND, pull-ups)\n")
    else for _, a in ipairs(addrs) do io.write(string.format("0x%02X ", a)) end; io.write("\n") end
    bus:close()
end

return M
