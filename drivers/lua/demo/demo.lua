-- demo.lua -- live RTC clock on the OLED. Composes the two mirrored drivers
-- (pcf8563 + ssd1306) over one i2c_host bus, proving they coexist on the wire.
--
--   luajit demo.lua [--port /dev/ttyACMx] [--kind pico|samd21]
--                   [--set] [--secs N]
--     --set    force-set the RTC from the host clock before running
--     --secs N run for N seconds then clear + exit (default: run forever)
--
-- If the RTC reports lost power (VL set), the time is auto-set from the host.

local _dir = (debug.getinfo(1, "S").source:sub(2)):match("(.*/)") or "./"
local _root = _dir .. "../"                        -- drivers/lua/ (each driver is one level under)
package.path = _root .. "i2c/?.lua;" .. _root .. "pcf8563/?.lua;" .. _root .. "ssd1306/?.lua;"
    .. _root .. "../../tools/commission/lua/?.lua;" .. package.path
local i2c    = require("i2c_host")
local pcf    = require("pcf8563")
local ssd    = require("ssd1306")
local font   = require("font5x7")
local bit    = require("bit")
local ffi    = require("ffi")

ffi.cdef [[ int poll(void *fds, unsigned long nfds, int timeout); ]]
local function msleep(ms) ffi.C.poll(nil, 0, ms) end   -- portable sub-second sleep

local WDAY = { [0] = "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }

-- ---- scaled text: same 5x7 glyphs as the driver, drawn as s*s blocks --------
local function draw_char(oled, x, y, ch, s)
    local c = ch:byte()
    if c < 0x20 or c > 0x7E then c = 0x20 end
    local g = font[c - 0x20 + 1]
    for col = 0, 4 do
        local bits = g[col + 1]
        for row = 0, 6 do
            if bit.band(bits, bit.lshift(1, row)) ~= 0 then
                for dx = 0, s - 1 do for dy = 0, s - 1 do
                    oled:pixel(x + col * s + dx, y + row * s + dy, true)
                end end
            end
        end
    end
end
local function text_w(str, s) return #str * 6 * s end
local function draw_text(oled, x, y, str, s)
    for i = 1, #str do draw_char(oled, x, y, str:sub(i, i), s); x = x + 6 * s end
end
local function centered(oled, y, str, s)
    draw_text(oled, math.floor((oled.w - text_w(str, s)) / 2), y, str, s)
end

local function set_from_host(rtc)
    local d = os.date("*t")
    rtc:set({ sec = d.sec, min = d.min, hour = d.hour, mday = d.day,
              wday = d.wday - 1, month = d.month, year = d.year })   -- os wday 1=Sun -> 0=Sun
end

local function render(oled, t)
    oled:clear()
    centered(oled, 0, "slow_bus RTC", 1)
    centered(oled, 16, string.format("%02d:%02d:%02d", t.hour, t.min, t.sec), 2)   -- big
    centered(oled, 42, string.format("%04d-%02d-%02d", t.year, t.month, t.mday), 1)
    centered(oled, 54, WDAY[t.wday] or "?", 1)
    oled:show()
end

-- ---- CLI --------------------------------------------------------------------
local port, kind, do_set, secs
local i = 1
while arg[i] do
    if     arg[i] == "--port" then i = i + 1; port = arg[i]
    elseif arg[i] == "--kind" then i = i + 1; kind = arg[i]
    elseif arg[i] == "--set"  then do_set = true
    elseif arg[i] == "--secs" then i = i + 1; secs = tonumber(arg[i]) end
    i = i + 1
end

local bus  = i2c.open(port, kind)
local rtc  = pcf.new(bus, 0x51)
local oled = ssd.new(bus, 0x3C)
print(string.format("%s front-end on %s -- RTC 0x51 + OLED 0x3C", kind or "pico", bus.port))

oled:init()
rtc:init()
if do_set or rtc:lost_power() then
    set_from_host(rtc)
    print("RTC set from host clock: " .. os.date("%Y-%m-%d %H:%M:%S"))
end

local start, last = os.time(), nil
while true do
    local t = rtc:get()
    local stamp = string.format("%02d:%02d:%02d", t.hour, t.min, t.sec)
    if stamp ~= last then                       -- redraw once per second
        render(oled, t)
        io.write("\r" .. stamp); io.flush()
        last = stamp
    end
    if secs and os.time() - start >= secs then break end
    msleep(100)
end

oled:clear(); oled:show()                        -- leave the panel blank on exit
print("\ndone.")
bus:close()
