-- bench.lua -- LuaJIT port of bench.py: the mode-aware, per-PIN bench across
-- SAMD21 modes. role/read/write/read_adc by Seeed pin name (D0..D10, A-aliases),
-- hiding the per-mode register maps (GPIO expander vs the COUNTER/SERVO per-pad
-- bench vs ADC-mode windows). Takes anything with reg_read/reg_write -- a
-- libcomm.Dongle or a reconnecting Link -- so `fleet:bench(path)` gives a per-pin
-- bench on any chip, by name.

local lc = require("libcomm")   -- for sleep()

local REG_MODE = 0x02
local MODE_IDLE, MODE_GPIO, MODE_ADC, MODE_MIXED, MODE_SERVO, MODE_COUNTER = 0, 1, 2, 3, 4, 5

-- GPIO expander: channel bitmap order (bit i = channel i).
local GPIO_CH = { "D0", "D1", "D2", "D3", "D7", "D8", "D9", "D10" }
local GPIO_CH_IDX = {}; for i, n in ipairs(GPIO_CH) do GPIO_CH_IDX[n] = i - 1 end
local G_IODIR, G_GPPU, G_OLAT, G_GPIO, G_OD = 0x10, 0x11, 0x14, 0x13, 0x18

-- COUNTER/SERVO per-pad bench: Seeed pad index + the 0x15-0x1E registers.
local SEEED = { "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8", "D9", "D10" }
local SEEED_IDX = {}; for i, n in ipairs(SEEED) do SEEED_IDX[n] = i - 1 end
local B_SEL, B_GPO, B_GPI, B_STAT, B_ROLE = 0x15, 0x16, 0x17, 0x1D, 0x1E
local B_ADCRQ, B_ADCST, B_ADCV = 0x18, 0x19, 0x1B
local BENCH_OK, BENCH_BAD_PIN = 0, 1
local ROLE_NAME = { [0]="none", [1]="in", [2]="out", [3]="adc", [4]="dac", [6]="oc", [7]="oc:up" }

-- ADC mode (single channel A1): windowed stats + instantaneous.
local A_WIN_SEL, A_SEQ, A_MIN, A_MAX, A_AVG, A_RMS, A_LATEST = 0x11, 0x12, 0x14, 0x16, 0x18, 0x1A, 0x1E
local ADC_WINDOWS = { khz1 = 0, hz100 = 1, hz10 = 2 }

local A_ALIAS = { A0="D0", A1="D1", A2="D2", A3="D3", A6="D6", A7="D7", A8="D8", A9="D9", A10="D10" }

local Bench = {}
Bench.__index = Bench
local M = {}

-- M.new(dg) -- dg = a libcomm.Dongle or a Link (reg_read/reg_write). Reads MODE
-- at construction; call :refresh() after a mode switch.
function M.new(dg)
    local self = setmetatable({ dg = dg }, Bench)
    return self:refresh()
end

function Bench:refresh() self.mode = self.dg:reg_read(REG_MODE); return self end

local function norm(pin) local p = tostring(pin):upper(); return A_ALIAS[p] or p end
local function seeed(p)
    local idx = SEEED_IDX[p]; if not idx then error("bench: unknown pin " .. p) end; return idx
end
local function bit_get(v, i) return math.floor(v / 2 ^ i) % 2 end

-- -- role: 'in'/'out'/'adc'/'dac'/'oc'/'oc:up'/'none', or 'int'/'i2c'/'reserved'
function Bench:role(pin)
    local p = norm(pin)
    if self.mode == MODE_GPIO then
        if p == "D6" then return "int" end
        if p == "D4" or p == "D5" then return "i2c" end
        local bit = GPIO_CH_IDX[p]; if not bit then error("bench: " .. p .. " not a GPIO channel") end
        if bit_get(self.dg:reg_read(G_IODIR), bit) == 1 then return "in" end
        if bit_get(self.dg:reg_read(G_OD), bit) == 1 then
            return bit_get(self.dg:reg_read(G_GPPU), bit) == 1 and "oc:up" or "oc"
        end
        return "out"
    elseif self.mode == MODE_COUNTER or self.mode == MODE_SERVO then
        self.dg:reg_write(B_SEL, seeed(p))
        if self.dg:reg_read(B_STAT) == BENCH_BAD_PIN then return "reserved" end
        return ROLE_NAME[self.dg:reg_read(B_ROLE)] or "?"
    end
    error("bench: role not supported in mode " .. self.mode)
end

-- -- read a digital level (0/1)
function Bench:read(pin)
    local p = norm(pin)
    if self.mode == MODE_GPIO then
        local bit = GPIO_CH_IDX[p]; if not bit then error("bench: " .. p .. " not a readable GPIO channel") end
        return bit_get(self.dg:reg_read(G_GPIO), bit)
    elseif self.mode == MODE_COUNTER or self.mode == MODE_SERVO then
        self.dg:reg_write(B_SEL, seeed(p))
        local v = self.dg:reg_read(B_GPI)
        local st = self.dg:reg_read(B_STAT)
        if st ~= BENCH_OK then error("bench: read " .. p .. " status " .. st) end
        return v
    end
    error("bench: read not supported in mode " .. self.mode)
end

-- -- drive an output pin (never the interlock D6)
function Bench:write(pin, val)
    local p = norm(pin)
    val = (val ~= 0 and val ~= false and val ~= nil) and 1 or 0
    if self.mode == MODE_GPIO then
        if p == "D6" then error("bench: D6 is the interrupt pin, not writable") end
        local bit = GPIO_CH_IDX[p]; if not bit then error("bench: " .. p .. " not a writable GPIO channel") end
        local olat = self.dg:reg_read(G_OLAT)
        local m = 2 ^ bit
        olat = (val == 1) and (olat + (bit_get(olat, bit) == 0 and m or 0))
                          or (olat - (bit_get(olat, bit) == 1 and m or 0))
        self.dg:reg_write(G_OLAT, olat % 256)
        return
    elseif self.mode == MODE_COUNTER or self.mode == MODE_SERVO then
        self.dg:reg_write(B_SEL, seeed(p))
        self.dg:reg_write(B_GPO, val)
        local st = self.dg:reg_read(B_STAT)
        if st ~= BENCH_OK then error("bench: write " .. p .. " status " .. st) end
        return
    end
    error("bench: write not supported in mode " .. self.mode)
end

-- -- ADC counts (0..4095). COUNTER/SERVO: 16x one-shot on an `adc`-role pad
-- (request -> poll -> get). ADC mode: the instantaneous A1 sample.
function Bench:read_adc(pin, timeout)
    timeout = timeout or 0.3
    local p = norm(pin)
    if self.mode == MODE_COUNTER or self.mode == MODE_SERVO then
        self.dg:reg_write(B_SEL, seeed(p))
        self.dg:reg_write(B_ADCRQ, 1)
        local n = math.floor(timeout / 0.002)
        local done = false
        for _ = 1, n do
            if self.dg:reg_read(B_ADCST) == 0 then done = true; break end
            lc.sleep(0.002)
        end
        if not done then error("bench: read_adc " .. p .. " conversion timeout") end
        local st = self.dg:reg_read(B_STAT)
        if st ~= BENCH_OK then error("bench: read_adc " .. p .. " status " .. st) end
        return self.dg:reg_read(B_ADCV) + self.dg:reg_read(B_ADCV + 1) * 256
    elseif self.mode == MODE_ADC then
        if p ~= "D1" then error("bench: ADC mode samples only A1 (=D1), got " .. p) end
        return self.dg:reg_read(A_LATEST) + self.dg:reg_read(A_LATEST + 1) * 256
    end
    error("bench: read_adc not supported in mode " .. self.mode)
end

-- -- ADC mode only: {seq,min,max,avg,rms} of a downsample window (khz1/hz100/hz10).
function Bench:read_adc_stats(window)
    window = window or "hz10"
    if self.mode ~= MODE_ADC then error("bench: read_adc_stats is ADC-mode only (mode " .. self.mode .. ")") end
    local w = ADC_WINDOWS[window]; if not w then error("bench: unknown window " .. window) end
    self.dg:reg_write(A_WIN_SEL, w)
    lc.sleep(0.02)
    local function u16(r) return self.dg:reg_read(r) + self.dg:reg_read(r + 1) * 256 end
    return { seq = u16(A_SEQ), min = u16(A_MIN), max = u16(A_MAX), avg = u16(A_AVG), rms = u16(A_RMS) }
end

return M
