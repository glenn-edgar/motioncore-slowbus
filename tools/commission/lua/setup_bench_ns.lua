-- setup_bench_ns.lua -- define the typical per-mode SAMD21 configs as a namespace.
--
-- Phase 1 (DSL/namespace), full-LuaJIT: writes one device node per functional
-- mode into the namespace DB, each carrying its config SOURCE (i2c / class / mode
-- / pins / interlock / rate). The on-chip bytes are NOT here -- they are compiled
-- at commission time. Configs are the proven ones from the per-mode test
-- harnesses.
--
--     luajit setup_bench_ns.lua [db_path]      (default /tmp/bench_ns.db)

local _dir = (debug.getinfo(1, "S").source:sub(2)):match("(.*/)") or "./"
package.path = _dir .. "?.lua;" .. package.path
local ndb = require("namespace_db")

local dbpath = arg[1] or "/tmp/bench_ns.db"

local ROOT  = "slow_bus"
local BENCH = "slow_bus.bench"
local function dev(name) return BENCH .. ".DEV." .. name end

-- Typical configs, one per mode (binary=GPIO, ADC, MIXED, COUNTER, SERVO).
local declared = {
    [ROOT]  = { kind = "KB",    attrs = { description = "slow_bus bench" } },
    [BENCH] = { kind = "BENCH", attrs = {} },

    -- BINARY / GPIO: 4 inputs (mixed pulls) + 4 outputs + a boolean interlock.
    [dev("gpio")] = { kind = "DEV", attrs = {
        cls = "SAMD21", i2c = 0x20, sub = "GPIO",
        pins = { D0 = "in:up", D1 = "in:down", D2 = "in:up", D3 = "in:none",
                 D7 = "out:0", D8 = "out:0", D9 = "out:1", D10 = "out:0" },
        interlock = { name = "safe", expr = "(D0 && D1) && (D2 || D3)", drive = { D8 = 1 } } } },

    -- ADC: single-channel A1 DSP; interlock over a downsampled stream -> oc D6.
    [dev("adc")] = { kind = "DEV", attrs = {
        cls = "SAMD21", i2c = 0x21, sub = "ADC",
        pins = { D6 = "oc" },
        interlock = { name = "rms", expr = "A1.avg.hz100 > 2.0V", drive = { D6 = 1 } } } },

    -- MIXED: one ADC pin + one debounced GPIO input + interlock -> D6.
    [dev("mixed")] = { kind = "DEV", attrs = {
        cls = "SAMD21", i2c = 0x22, sub = "MIXED",
        pins = { A1 = "adc", D8 = "in:up", D6 = "out" },
        interlock = { name = "mix", expr = "A1 > 2.0V && D8", drive = { D6 = 1 } } } },

    -- COUNTER: one rising-edge counter channel (D2 -- D0/A0 is the DAC pad) +
    -- spare bench pads (dac/adc/io).
    [dev("counter")] = { kind = "DEV", attrs = {
        cls = "SAMD21", i2c = 0x23, sub = "COUNTER", rate = 1000,
        pins = { D2 = "count:up:rising", A0 = "dac", D1 = "adc", D9 = "out", D10 = "in" } } },
}

local ns = ndb.open(dbpath)
local r = ns:regen_subtree(ROOT, declared)
print("db: " .. dbpath)
print("regen: added=" .. #r.added .. " deleted=" .. #r.deleted .. " kept=" .. #r.kept)
print("namespace:")
for _, p in ipairs(ns:list_subtree(ROOT)) do
    local n = ns:get_node(p)
    local a = n.attrs
    if n.kind == "DEV" then
        print(string.format("  %-28s  %-7s i2c=0x%02X  uid=%s",
            p, a.sub, a.i2c, ns:resolve_uuid(p) or "(unbound)"))
    else
        print("  " .. p)
    end
end
ns:close()
