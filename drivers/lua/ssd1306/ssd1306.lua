-- ssd1306.lua -- SSD1306 OLED driver (host). Mirror of C `ssd1306.[ch]`:
-- identical draw logic; only the transport (i2c_host Bus) and the show()
-- chunking differ (see show()).
--
-- Mono OLED on I2C (XIAO expansion board: 128x64 @ 0x3C). Every transfer is a
-- control byte then a stream: 0x00 = command, 0x40 = data. RAM = W cols x
-- (H/8) pages, one byte = 8 vertical pixels (bit0 = top). Draw into a local
-- framebuffer, then show() the whole thing.
--
--   luajit ssd1306.lua [--port /dev/ttyACMx] [--kind pico|samd21] [--addr 0x3C]

local _dir = (debug.getinfo(1, "S").source:sub(2)):match("(.*/)") or "./"
local _root = _dir .. "../"                        -- drivers/lua/ (each driver is one level under)
package.path = _root .. "i2c/?.lua;" .. _root .. "pcf8563/?.lua;" .. _root .. "ssd1306/?.lua;"
    .. _root .. "../../tools/commission/lua/?.lua;" .. package.path
local i2c  = require("i2c_host")
local font = require("font5x7")
local bit  = require("bit")

local CTRL_CMD, CTRL_DATA = 0x00, 0x40

local SSD = {}
SSD.__index = SSD
local M = {}

function M.new(bus, addr, w, h)
    local self = setmetatable({ bus = bus, addr = addr or 0x3C, w = w or 128, h = h or 64 }, SSD)
    self.pages = self.h / 8
    self.fb = {}
    self:clear()
    return self
end

-- command stream (0x00 control byte)
function SSD:_cmd(bytes) self.bus:write(self.addr, string.char(CTRL_CMD) .. bytes) end

function SSD:init()
    local mux     = self.h - 1
    local compins = (self.h == 32) and 0x02 or 0x12
    self:_cmd(string.char(
        0xAE,             -- display off
        0xD5, 0x80,       -- clock divide
        0xA8, mux,        -- multiplex = H-1
        0xD3, 0x00,       -- display offset 0
        0x40,             -- start line 0
        0x8D, 0x14,       -- charge pump on
        0x20, 0x00,       -- horizontal addressing mode
        0xA1,             -- segment remap
        0xC8,             -- COM scan reversed -> upright
        0xDA, compins,    -- COM pins config
        0x81, 0xCF,       -- contrast
        0xD9, 0xF1,       -- pre-charge
        0xDB, 0x40,       -- VCOMH deselect
        0xA4,             -- resume to RAM
        0xA6,             -- normal (not inverted)
        0xAF))            -- display on
    self:clear()
    self:show()
end

function SSD:clear()
    for i = 1, self.w * self.pages do self.fb[i] = 0 end
end

function SSD:pixel(x, y, on)
    if x < 0 or y < 0 or x >= self.w or y >= self.h then return end
    local idx  = math.floor(y / 8) * self.w + x + 1   -- 1-based framebuffer index
    local mask = bit.lshift(1, y % 8)
    if on then self.fb[idx] = bit.bor(self.fb[idx], mask)
    else       self.fb[idx] = bit.band(self.fb[idx], bit.band(bit.bnot(mask), 0xFF)) end
end

function SSD:char(x, y, ch)
    local c = ch:byte()
    if c < 0x20 or c > 0x7E then c = 0x20 end
    local g = font[c - 0x20 + 1]
    for col = 0, 4 do
        local bits = g[col + 1]
        for row = 0, 6 do
            if bit.band(bits, bit.lshift(1, row)) ~= 0 then self:pixel(x + col, y + row, true) end
        end
    end
end

function SSD:text(x, y, s)
    for i = 1, #s do self:char(x, y, s:sub(i, i)); x = x + 6 end
end

function SSD:show()
    self:_cmd(string.char(0x21, 0, self.w - 1, 0x22, 0, self.pages - 1))  -- col + page range
    -- The framebuffer ships as data (0x40) chunked to the transport's write
    -- cap (the 32/64 B USB-shell limit). Each chunk = 0x40 + up to max_write-1
    -- framebuffer bytes. (The C twin sends it all in one chip-native write.)
    local n     = self.w * self.pages
    local chunk = self.bus.max_write - 1
    local i = 1
    while i <= n do
        local last, parts = math.min(i + chunk - 1, n), {}
        for k = i, last do parts[#parts + 1] = string.char(self.fb[k]) end
        self.bus:write(self.addr, string.char(CTRL_DATA) .. table.concat(parts))
        i = last + 1
    end
end

function SSD:display_on(on) self:_cmd(string.char(on and 0xAF or 0xAE)) end
function SSD:invert(inv)    self:_cmd(string.char(inv and 0xA7 or 0xA6)) end
function SSD:contrast(c)    self:_cmd(string.char(0x81, c)) end

M.SSD = SSD

-- ---- CLI: visual self-test --------------------------------------------------
if arg and (arg[0] or ""):match("ssd1306%.lua$") then
    local port, kind, addr
    local i = 1
    while arg[i] do
        if arg[i] == "--port" then i = i + 1; port = arg[i]
        elseif arg[i] == "--kind" then i = i + 1; kind = arg[i]
        elseif arg[i] == "--addr" then i = i + 1; addr = tonumber(arg[i]) end
        i = i + 1
    end
    local function sleep(s) local t = os.time() + s; repeat until os.time() >= t end

    local bus = i2c.open(port, kind)
    print(string.format("%s front-end on %s", kind or "pico", bus.port))
    local oled = M.new(bus, addr or 0x3C)
    print(string.format("SSD1306 @ 0x%02X (%dx%d) -- visual self-test", oled.addr, oled.w, oled.h))

    oled:init()
    oled:clear()
    for x = 0, oled.w - 1 do oled:pixel(x, 0, true); oled:pixel(x, oled.h - 1, true) end   -- border
    for y = 0, oled.h - 1 do oled:pixel(0, y, true); oled:pixel(oled.w - 1, y, true) end
    oled:text(4, 4,  "slow_bus SSD1306")
    oled:text(4, 14, "0123456789 +-*/=")
    oled:text(4, 24, "ABCDEFGHIJKLMNOPQR")
    oled:text(4, 34, "abcdefghijklmnopqr")
    oled:text(4, 44, "#$%&@?!<>[]{}~^")
    oled:show()
    print("  drew border + 5 text rows")

    sleep(1); oled:invert(true);  print("  invert ON")
    sleep(1); oled:invert(false); print("  invert OFF")

    print("\n  look at the OLED: a full border with 5 rows of text, that flashed inverted once.")
    bus:close()
end

return M
