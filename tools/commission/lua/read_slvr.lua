-- read_slvr.lua -- read the `slvr` sam_slaves roster from the SOLE attached chip
-- using the register-based FILE bank (0x50..0x55) -- the EXACT path the Pico runs
-- over I2C -- then CBOR-decode + print the roster. Read-only; works in any mode.
--
--   luajit read_slvr.lua [name]      (name defaults to "slvr")

local _dir = (debug.getinfo(1, "S").source:sub(2)):match("(.*/)") or "./"
package.path = _dir .. "?.lua;" .. package.path
local lc = require("libcomm")

local REG_MODE = 0x02
local REG_FILE_NAME, REG_FILE_CTRL, REG_FILE_STAT = 0x50, 0x51, 0x52
local REG_FILE_SIZE, REG_FILE_SEEK, REG_FILE_DATA = 0x53, 0x54, 0x55
local CTRL_OPEN, CTRL_CLOSE = 1, 2
local MODE_NAME = { [0]="IDLE", [1]="GPIO", [2]="ADC", [3]="MIXED", [4]="SERVO", [5]="COUNTER" }

-- ---- minimal CBOR decoder (uint / text / array / map / bool) -----------------
local bit = require("bit")
local function dec(s, i)
    local b = s:byte(i); i = i + 1
    local major, n = bit.rshift(b, 5), bit.band(b, 0x1f)
    if n == 24 then n = s:byte(i); i = i + 1
    elseif n == 25 then n = s:byte(i) * 256 + s:byte(i + 1); i = i + 2
    elseif n == 26 then n = ((s:byte(i) * 256 + s:byte(i+1)) * 256 + s:byte(i+2)) * 256 + s:byte(i+3); i = i + 4 end
    if major == 0 then return n, i
    elseif major == 3 then return s:sub(i, i + n - 1), i + n
    elseif major == 4 then local t = {}; for k = 1, n do t[k], i = dec(s, i) end; return t, i
    elseif major == 5 then local t = {}; for _ = 1, n do local k, v; k, i = dec(s, i); v, i = dec(s, i); t[k] = v end; return t, i
    elseif major == 7 then return (b == 0xf5) and true or (b == 0xf4) and false or nil, i end
    error("cbor: unsupported initial byte 0x" .. string.format("%02x", b))
end

-- ---- pick the sole present chip ----------------------------------------------
local ch = lc.enumerate()
if #ch == 0 then io.stderr:write("no SAMD21 on the USB bus\n"); os.exit(1) end
if #ch > 1 then io.stderr:write("more than one chip present -- attach exactly one\n"); os.exit(1) end
local name = arg[1] or "slvr"
local dg = lc.open(ch[1].port)
print(string.format("chip %s on %s  mode=%s", ch[1].serial, ch[1].port,
    MODE_NAME[dg:reg_read(REG_MODE)] or "?"))

-- ---- FILE bank read, register by register (the Pico's exact sequence) --------
print(string.format("\n-- FILE bank read of '%s' --", name))
local n4 = (name .. "    "):sub(1, 4)
for k = 1, 4 do dg:reg_write(REG_FILE_NAME, n4:byte(k)) end   -- burst the 4-byte name -> 0x50
print(string.format("  wrote name [%s] -> 0x50", name))
dg:reg_write(REG_FILE_CTRL, CTRL_OPEN)                        -- 0x51 = 1 (OPEN)
local stat = dg:reg_read(REG_FILE_STAT)                       -- 0x52
print(string.format("  CTRL=OPEN  STAT(0x52)=%d (%s)", stat, stat == 0 and "OK/open" or "NOT_FOUND"))
if stat ~= 0 then
    print("  -> this chip has no '" .. name .. "' file (not a boot_store?)")
    dg:reg_write(REG_FILE_CTRL, CTRL_CLOSE); dg:close(); os.exit(2)
end
local size = dg:reg_read(REG_FILE_SIZE)                       -- 0x53
print(string.format("  SIZE(0x53)=%d bytes", size))
local bytes = {}
for _ = 1, size do bytes[#bytes + 1] = dg:reg_read(REG_FILE_DATA) end   -- 0x55 streamed
dg:reg_write(REG_FILE_CTRL, CTRL_CLOSE)                       -- 0x51 = 2 (CLOSE)

local raw = string.char(unpack(bytes))
local hex = {}; for k = 1, #bytes do hex[k] = string.format("%02x", bytes[k]) end
print("  DATA(0x55) raw CBOR = " .. table.concat(hex))

-- ---- decode + print the roster ----------------------------------------------
local roster = dec(raw, 1)
print(string.format("\n-- decoded roster (v=%s) --", tostring(roster.v)))
for _, s in ipairs(roster.sam_slaves or {}) do
    print(string.format("  %-8s type=%-5s addr=0x%02X  mode=%s",
        s.name, s.type, s.addr, s.mode))
end
dg:close()
