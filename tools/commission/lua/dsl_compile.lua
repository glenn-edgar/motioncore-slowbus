-- dsl_compile.lua -- read a cfg table (Lua literal) from stdin, compile via the
-- Lua slave_dsl, print "<name> <hex>" per on-chip file. Used by dsl_parity.py.
--   cfg = { i2c=.., mode="GPIO", pins={...}, interlock={name=,expr=,drive={}}, rate=.. }
local _dir = (debug.getinfo(1, "S").source:sub(2)):match("(.*/)") or "./"
package.path = _dir .. "?.lua;" .. package.path
local sd = require("slave_dsl")

local cfg = load(io.read("*a"))()
local u = sd.Unit(cfg.i2c, cfg.mode)
if cfg.mode == "COUNTER" then u:counter(cfg.rate or 1000) end
if cfg.mode == "SERVO" then u:servo() end
if cfg.pins then u:pins(cfg.pins) end
if cfg.interlock then u:interlock(cfg.interlock.name, cfg.interlock.expr, cfg.interlock.drive) end

local f = u:files()
local names = {}
for k in pairs(f) do names[#names + 1] = k end
table.sort(names)
for _, k in ipairs(names) do
    local s, hex = f[k], {}
    for i = 1, #s do hex[i] = string.format("%02x", s:byte(i)) end
    io.write(k .. " " .. table.concat(hex) .. "\n")
end
