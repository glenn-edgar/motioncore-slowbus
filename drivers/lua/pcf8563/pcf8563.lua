-- pcf8563.lua -- PCF8563 RTC driver (host). Mirror of C `pcf8563.[ch]`:
-- identical register logic, only the transport (i2c_host Bus) differs.
--
-- The PCF8563 (XIAO expansion-board RTC, fixed addr 0x51) is a BCD clock:
--   0x00 CTRL1  0x01 CTRL2  0x02 SECONDS(bit7 VL)  0x03 MIN  0x04 HOUR
--   0x05 DAY    0x06 WEEKDAY  0x07 CENTURY/MONTH  0x08 YEAR
-- Read in one burst (write reg ptr 0x02, read 7); set in one write.
--
--   luajit pcf8563.lua [--port /dev/ttyACMx] [--addr 0x51]   # API self-test

local _dir = (debug.getinfo(1, "S").source:sub(2)):match("(.*/)") or "./"
local _root = _dir .. "../"                        -- drivers/lua/ (each driver is one level under)
package.path = _root .. "i2c/?.lua;" .. _root .. "pcf8563/?.lua;" .. _root .. "ssd1306/?.lua;"
    .. _root .. "../../tools/commission/lua/?.lua;" .. package.path
local i2c = require("i2c_host")

local REG = { CTRL1 = 0x00, CTRL2 = 0x01, SECONDS = 0x02 }
local VL_BIT = 0x80

local PCF = {}
PCF.__index = PCF
local M = {}

function M.new(bus, addr) return setmetatable({ bus = bus, addr = addr or 0x51 }, PCF) end

local function bcd2dec(b) return math.floor(b / 16) * 10 + (b % 16) end
local function dec2bcd(d) return math.floor(d / 10) * 16 + (d % 10) end

function PCF:init()   -- CTRL1=0 (run), CTRL2=0 (no alarm/timer ints)
    self.bus:write(self.addr, string.char(REG.CTRL1, 0x00, 0x00))
end

function PCF:lost_power()
    local sec = self.bus:write_read(self.addr, string.char(REG.SECONDS), 1):byte(1)
    return sec % 256 >= VL_BIT       -- bit7 set => integrity lost
end

function PCF:get()
    local r = self.bus:write_read(self.addr, string.char(REG.SECONDS), 7)
    return {
        sec   = bcd2dec(r:byte(1) % 128),   -- mask VL
        min   = bcd2dec(r:byte(2) % 128),
        hour  = bcd2dec(r:byte(3) % 64),
        mday  = bcd2dec(r:byte(4) % 64),
        wday  = r:byte(5) % 8,
        month = bcd2dec(r:byte(6) % 32),
        year  = 2000 + bcd2dec(r:byte(7)),
    }
end

function PCF:set(t)
    self.bus:write(self.addr, string.char(
        REG.SECONDS,                      -- writing seconds clears VL
        dec2bcd(t.sec),
        dec2bcd(t.min),
        dec2bcd(t.hour),
        dec2bcd(t.mday),
        t.wday % 8,
        dec2bcd(t.month),
        dec2bcd(t.year % 100)))
end

M.PCF, M.REG = PCF, REG

-- ---- CLI: API validation ----------------------------------------------------
if arg and (arg[0] or ""):match("pcf8563%.lua$") then
    local port, addr, kind
    local i = 1
    while arg[i] do
        if arg[i] == "--port" then i = i + 1; port = arg[i]
        elseif arg[i] == "--kind" then i = i + 1; kind = arg[i]
        elseif arg[i] == "--addr" then i = i + 1; addr = tonumber(arg[i]) end
        i = i + 1
    end
    local bus = i2c.open(port, kind)
    print(string.format("%s front-end on %s", kind or "pico", bus.port))
    addr = addr or 0x51
    local rtc = M.new(bus, addr)
    print(string.format("PCF8563 @ 0x%02X -- API validation\n", addr))

    local pass, fail = 0, 0
    local function check(name, got, want)
        local ok = got == want
        print(string.format("  [%s] %-30s got=%s want=%s", ok and "PASS" or "FAIL", name,
            tostring(got), tostring(want)))
        if ok then pass = pass + 1 else fail = fail + 1 end
    end

    -- 1. init, then set a known time -> write path
    rtc:init()
    local set = { sec = 30, min = 45, hour = 13, mday = 16, wday = 2, month = 6, year = 2026 }
    rtc:set(set)
    -- 2. read it straight back -> read path + BCD round-trip (allow the seconds
    --    field to have ticked up by a couple while we were talking)
    local g = rtc:get()
    check("min round-trip",   g.min,   set.min)
    check("hour round-trip",  g.hour,  set.hour)
    check("mday round-trip",  g.mday,  set.mday)
    check("wday round-trip",  g.wday,  set.wday)
    check("month round-trip", g.month, set.month)
    check("year round-trip",  g.year,  set.year)
    check("sec in range",     (g.sec >= 30 and g.sec <= 35), true)
    -- 3. setting the time cleared the VL (low-voltage) flag
    check("VL cleared after set", rtc:lost_power(), false)
    -- 4. the clock is actually running: seconds advance
    local s0 = rtc:get().sec
    local t1 = os.time() + 2; repeat until os.time() >= t1     -- ~2 s busy wait (no posix sleep)
    check("clock advanced ~2 s", (rtc:get().sec - s0 + 60) % 60 >= 1, true)

    local n = rtc:get()
    print(string.format("\n  now: %04d-%02d-%02d  %02d:%02d:%02d  (wday %d)",
        n.year, n.month, n.mday, n.hour, n.min, n.sec, n.wday))
    print(string.format("\n  %d/%d API checks passed%s", pass, pass + fail,
        fail == 0 and " -- rw path validated" or ""))
    bus:close()
    os.exit(fail == 0 and 0 or 1)
end

return M
