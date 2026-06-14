-- slave_dsl.lua -- LuaJIT port of slave_dsl.py: compile a SAMD21 unit definition
-- (i2c addr + mode + pins + interlock) into the raw on-chip files idnt / gpmp /
-- ilcf / cntr / srvo. Byte-for-byte equivalent to the Python compiler (the
-- trusted, HW-verified oracle); proven by lua/dsl_parity.lua.
--
-- Emission order: where the Python uses dict insertion order (the ilcf `cfg` and
-- `drive` sections), this port emits in a canonical PAD order (D0<D1<..<D10),
-- which is deterministic and order-independent -- the right behaviour since the
-- namespace stores pins as an unordered JSON object.

local M = {}
local lshift = require("bit").lshift

-- ---- constants (mirror slave_dsl.py) --------------------------------------
local MODES = { IDLE = 0, GPIO = 1, ADC = 2, MIXED = 3, SERVO = 4, COUNTER = 5 }
local MODE_NAME = { [0]="IDLE",[1]="GPIO",[2]="ADC",[3]="MIXED",[4]="SERVO",[5]="COUNTER" }
local MODES_WITH_ILCF = { [1]=true, [2]=true, [3]=true }      -- GPIO, ADC, MIXED

local IL_MAX_INPUTS, IL_MAX_WATCHES, IL_MAX_OUTPUTS = 4, 8, 2
local IL_DSL_MAX, IL_NAME_MAX = 128, 16
local MIXED_TICK_MS = 10
local DEBOUNCE_DEPTH_MIN, DEBOUNCE_DEPTH_MAX = 2, 15

local CNTR_VERSION = 1
local COUNTER_PINS = { "D0","D1","D2","D3","D7","D8","D9","D10","D6" }
local COUNTER_PIN_IDX = {}; for i,p in ipairs(COUNTER_PINS) do COUNTER_PIN_IDX[p]=i-1 end
local COUNTER_PIN_ALIAS = { A0="D0",A1="D1",A2="D2",A3="D3",A6="D6",A7="D7",A8="D8",A9="D9",A10="D10" }
local COUNTER_PULLS = { none=0, up=1, down=2 }
local COUNTER_EDGES = { rising=0, falling=1, both=2 }
local COUNTER_BENCH_ROLES = { ["in"]=1, out=2, adc=3, dac=4, oc=6 }
local BENCH_ROLE_OC, BENCH_ROLE_OC_PU = 6, 7
local COUNTER_RATE_MIN, COUNTER_RATE_MAX = 50, 10000

local SRVO_VERSION = 1
local SERVO_PINS = { "D0","D1","D2","D3","D7","D8","D9","D10" }
local SERVO_PIN_IDX = {}; for i,p in ipairs(SERVO_PINS) do SERVO_PIN_IDX[p]=i-1 end
local SERVO_PIN_ALIAS = { A0="D0",A1="D1",A2="D2",A3="D3",A7="D7",A8="D8",A9="D9",A10="D10" }
local SERVO_ROLES = { ["in"]=1, out=2, adc=3, dac=4, servo=5, oc=6 }

local ADC_FULLSCALE = 4095
local VREF_DEFAULT = 3.3
local GPIO_PINS = { "D0","D1","D2","D3","D7","D8","D9","D10" }
local GPIO_PIN_SET = {}; for _,p in ipairs(GPIO_PINS) do GPIO_PIN_SET[p]=true end
local GPMP_VERSION = 2

local OP_FROM_SYM = { [">"]="gt", ["<"]="lt", [">="]="ge", ["<="]="le", ["=="]="eq", ["!="]="ne" }
local OP_INVERT = { gt="le", le="gt", lt="ge", ge="lt", eq="ne", ne="eq" }
local ROLE_BASES = { ["in"]=true, out=true, adc=true, dac=true, count=true, servo=true, oc=true }

local ADC_STATS = { avg=true, min=true, max=true, rms=true }
local ADC_WINDOWS = { khz1=true, hz100=true, hz10=true }
local ADC_WATCH_PINS = { A1=true, D1=true }

M.MODES, M.MODE_NAME = MODES, MODE_NAME

-- ---- small helpers --------------------------------------------------------
local function err(msg) error("DSLError: " .. msg, 0) end

local function bytestr(t) return string.char(unpack(t)) end

local function split(s, sep)
    local out = {}
    for part in (s .. sep):gmatch("([^" .. sep .. "]*)" .. sep) do out[#out + 1] = part end
    return out
end

-- canonical pad order: numeric, A-alias folds onto D; unknown labels sort last
local function pad_rank(label)
    local n = label:match("^[ADad](%d+)$")
    return n and tonumber(n) or 999
end
local function sorted_keys(map)
    local ks = {}
    for k in pairs(map) do ks[#ks + 1] = k end
    table.sort(ks, function(a, b)
        local ra, rb = pad_rank(a), pad_rank(b)
        if ra ~= rb then return ra < rb end
        return a < b
    end)
    return ks
end

local function round(x) return math.floor(x + 0.5) end
local function volts_to_count(volts, vref)
    local n = round(volts / vref * ADC_FULLSCALE)
    return math.max(0, math.min(ADC_FULLSCALE, n))
end
local function bench_role_value(label, base, mods)
    if base == "oc" then
        if #mods == 0 then return BENCH_ROLE_OC end
        if #mods == 1 and mods[1] == "up" then return BENCH_ROLE_OC_PU end
        err("oc on " .. label .. ": only :up modifier allowed")
    end
    return COUNTER_BENCH_ROLES[base]
end

-- ---- tokenizer ------------------------------------------------------------
local function tokenize(text)
    local toks, i, n = {}, 1, #text
    while i <= n do
        local c = text:sub(i, i)
        if c:match("%s") then
            i = i + 1
        else
            local two = text:sub(i, i + 1)
            if two == "&&" then toks[#toks+1] = {"and_op", two}; i = i + 2
            elseif two == "||" then toks[#toks+1] = {"or_op", two}; i = i + 2
            elseif two == ">=" or two == "<=" or two == "==" or two == "!=" then
                toks[#toks+1] = {"op", two}; i = i + 2
            elseif c == "~" then toks[#toks+1] = {"not_op", c}; i = i + 1
            elseif c == ">" or c == "<" then toks[#toks+1] = {"op", c}; i = i + 1
            elseif c == "(" then toks[#toks+1] = {"lp", c}; i = i + 1
            elseif c == ")" then toks[#toks+1] = {"rp", c}; i = i + 1
            elseif c == "." then toks[#toks+1] = {"dot", c}; i = i + 1
            elseif c:match("%d") then
                local num = text:match("^%d+%.?%d*[Vv]?", i)
                toks[#toks+1] = {"num", num}; i = i + #num
            elseif c:match("[A-Za-z_]") then
                local id = text:match("^[A-Za-z_][A-Za-z0-9_]*", i)
                toks[#toks+1] = {"ident", id}; i = i + #id
            else
                err("cannot tokenize at " .. text:sub(i, i + 11))
            end
        end
    end
    return toks
end

-- ---- recursive-descent parser -> AST --------------------------------------
-- AST: {"or",{...}} / {"and",{...}} / {"not",child} / {"lit",pin,op,thr}
local Parser = {}
Parser.__index = Parser
local function new_parser(toks, roles, vref, adc)
    return setmetatable({ toks = toks, i = 1, roles = roles, vref = vref, adc = adc }, Parser)
end
function Parser:peek() local t = self.toks[self.i]; if t then return t[1], t[2] end return nil, nil end
function Parser:next() local k, v = self:peek(); self.i = self.i + 1; return k, v end

function Parser:parse()
    local node = self:p_or()
    if self.i ~= #self.toks + 1 then err("trailing tokens in expression") end
    return node
end
function Parser:p_or()
    local ch = { self:p_and() }
    while (select(1, self:peek())) == "or_op" do self:next(); ch[#ch+1] = self:p_and() end
    if #ch == 1 then return ch[1] end
    return { "or", ch }
end
function Parser:p_and()
    local ch = { self:p_not() }
    while (select(1, self:peek())) == "and_op" do self:next(); ch[#ch+1] = self:p_not() end
    if #ch == 1 then return ch[1] end
    return { "and", ch }
end
function Parser:p_not()
    if (select(1, self:peek())) == "not_op" then self:next(); return { "not", self:p_not() } end
    return self:p_atom()
end
function Parser:p_atom()
    local kind, val = self:peek()
    if kind == "lp" then
        self:next()
        local node = self:p_or()
        if (select(1, self:next())) ~= "rp" then err("missing ')'") end
        return node
    end
    if kind == "ident" then
        self:next()
        if self.adc then return self:adc_comparison(val:upper()) end
        local pin = (val:sub(1, 1) == "_") and val or val:upper()
        return self:comparison(pin)
    end
    err("expected a pin or '(', got " .. tostring(val))
end
function Parser:adc_comparison(chan)
    if not ADC_WATCH_PINS[chan] then err("ADC channel " .. chan .. " not watchable (only A1/D1)") end
    local operand = chan
    if (select(1, self:peek())) == "dot" then
        self:next()
        local k, stat = self:next()
        if k ~= "ident" or not ADC_STATS[stat:lower()] then err("expected stat avg/min/max/rms") end
        if (select(1, self:next())) ~= "dot" then err("expected .window after ." .. stat) end
        local k2, win = self:next()
        if k2 ~= "ident" or not ADC_WINDOWS[win:lower()] then err("expected window khz1/hz100/hz10") end
        operand = chan .. "_" .. stat:lower() .. "_" .. win:lower()
    end
    local k, sym = self:peek()
    if k ~= "op" then err("ADC stream " .. operand .. " needs a comparison") end
    self:next()
    local op = OP_FROM_SYM[sym]
    local nk, nval = self:next()
    if nk ~= "num" then err("expected a number after " .. sym) end
    local thr
    local last = nval:sub(-1)
    if last == "V" or last == "v" then
        thr = volts_to_count(tonumber(nval:sub(1, -2)), self.vref)
    else
        thr = tonumber(nval)
        if not (thr >= 0 and thr <= ADC_FULLSCALE) then err("ADC count out of range") end
    end
    return { "lit", operand, op, thr }
end
function Parser:comparison(pin)
    if not self.roles[pin] then err("pin " .. pin .. " used in expression but not declared in pins()") end
    local base = self.roles[pin].base
    if base == "out" then err("output pin " .. pin .. " cannot be watched") end
    local kind, val = self:peek()
    if kind == "op" then
        self:next()
        local op = OP_FROM_SYM[val]
        local nkind, nval = self:next()
        if nkind ~= "num" then err("expected a number after " .. val) end
        return { "lit", pin, op, self:value(pin, base, nval) }
    end
    if base == "adc" then err("ADC pin " .. pin .. " needs a comparison") end
    return { "lit", pin, "eq", 1 }
end
function Parser:value(pin, base, tok)
    local last = tok:sub(-1)
    if last == "V" or last == "v" then
        if base ~= "adc" then err("voltage threshold on non-ADC pin " .. pin) end
        return volts_to_count(tonumber(tok:sub(1, -2)), self.vref)
    end
    local nn = tonumber(tok)
    if base == "adc" then
        if not (nn >= 0 and nn <= ADC_FULLSCALE) then err("ADC count out of range") end
    elseif nn ~= 0 and nn ~= 1 then
        err("GPIO threshold on " .. pin .. " must be 0 or 1")
    end
    return nn
end

-- ---- De-Morgan + DNF ------------------------------------------------------
local function push_not(node, neg)
    local t = node[1]
    if t == "lit" then
        local op = node[3]
        return { "lit", node[2], neg and OP_INVERT[op] or op, node[4] }
    elseif t == "not" then
        return push_not(node[2], not neg)
    elseif t == "and" or t == "or" then
        local flipped = t
        if neg then flipped = (t == "and") and "or" or "and" end
        local ch = {}
        for _, c in ipairs(node[2]) do ch[#ch + 1] = push_not(c, neg) end
        return { flipped, ch }
    end
    err("bad AST node")
end
local function to_dnf(node)
    local t = node[1]
    if t == "lit" then return { { node } } end
    if t == "or" then
        local terms = {}
        for _, c in ipairs(node[2]) do for _, term in ipairs(to_dnf(c)) do terms[#terms+1] = term end end
        return terms
    end
    if t == "and" then
        local terms = { {} }
        for _, c in ipairs(node[2]) do
            local ct = to_dnf(c)
            local nt = {}
            for _, pre in ipairs(terms) do
                for _, term in ipairs(ct) do
                    local merged = {}
                    for _, x in ipairs(pre) do merged[#merged+1] = x end
                    for _, x in ipairs(term) do merged[#merged+1] = x end
                    nt[#nt+1] = merged
                end
            end
            terms = nt
        end
        return terms
    end
    err("bad AST node")
end
local function compile_expr(when, roles, vref, adc)
    local ast = new_parser(tokenize(when), roles, vref, adc):parse()
    local groups = {}
    for _, term in ipairs(to_dnf(push_not(ast, false))) do
        local seen, lits = {}, {}
        for _, lit in ipairs(term) do
            local key = lit[2] .. "|" .. lit[3] .. "|" .. lit[4]
            if not seen[key] then seen[key] = true; lits[#lits+1] = { lit[2], lit[3], lit[4] } end
        end
        groups[#groups+1] = lits
    end
    return groups
end

-- ---- Unit -----------------------------------------------------------------
local Unit = {}
Unit.__index = Unit
M.Unit = function(addr, mode, vref)
    if not (addr >= 0 and addr <= 0x7F) then err("i2c address out of 7-bit range") end
    local key = tostring(mode):upper()
    if not MODES[key] then err("unknown type " .. tostring(mode)) end
    return setmetatable({ addr = addr, mode = MODES[key], vref = vref or VREF_DEFAULT,
                          roles = {}, il = nil, cntr_rate = 1000 }, Unit)
end

function Unit:counter(rate)
    if self.mode ~= MODES.COUNTER then err("counter() is COUNTER-mode only") end
    rate = rate or 1000
    if not (rate >= COUNTER_RATE_MIN and rate <= COUNTER_RATE_MAX) then err("counter rate out of range") end
    self.cntr_rate = rate
    return self
end
function Unit:servo()
    if self.mode ~= MODES.SERVO then err("servo() is SERVO-mode only") end
    return self
end
function Unit:pins(tbl)
    for label, spec in pairs(tbl) do
        local parts = split(tostring(spec), ":")
        local base = parts[1]
        local mods = {}; for j = 2, #parts do mods[#mods+1] = parts[j] end
        if not ROLE_BASES[base] then err("pin " .. label .. ": bad role base " .. base) end
        self.roles[label:upper()] = { base = base, mods = mods }
    end
    return self
end
function Unit:interlock(name, when, drive)
    if #name >= IL_NAME_MAX then err("interlock name too long") end
    local d = {}
    if drive then for k, v in pairs(drive) do d[k:upper()] = v end end
    self.il = { name = name, when = when, drive = d }
    return self
end

function Unit:idnt() return bytestr({ self.addr, self.mode }) end

function Unit:_debounce_depth(pin, mod)
    if self.mode ~= MODES.MIXED then err("debounce on " .. pin .. " is MIXED-only") end
    local body = mod:sub(#("debounce_") + 1)
    local ms = body:match("^(%d+)ms$")
    if not ms then err("debounce on " .. pin .. " must be in ms, e.g. debounce_50ms") end
    ms = tonumber(ms)
    local depth = math.floor((ms + math.floor(MIXED_TICK_MS / 2)) / MIXED_TICK_MS)
    if not (depth >= DEBOUNCE_DEPTH_MIN and depth <= DEBOUNCE_DEPTH_MAX) then err("debounce depth out of range") end
    return depth
end
function Unit:_cfg_token(pin)
    local r = self.roles[pin]
    local base, mods = r.base, r.mods
    if base == "adc" then
        local sfx = ""; for _, m in ipairs(mods) do sfx = sfx .. "," .. m end
        return "(" .. pin .. "):adc" .. sfx
    elseif base == "in" then
        local pull, deb = "none", nil
        for _, m in ipairs(mods) do
            if m == "up" or m == "down" or m == "none" then pull = m
            elseif m:sub(1, 9) == "debounce_" then deb = self:_debounce_depth(pin, m)
            else err("input " .. pin .. ": bad modifier " .. m) end
        end
        local sfx = ({ up = ",up", down = ",down", none = "" })[pull]
        if deb then sfx = sfx .. ",debounce_" .. deb end
        return "(" .. pin .. "):in" .. sfx
    elseif base == "out" then
        return "(" .. pin .. "):out"
    elseif base == "oc" then
        return self:_out_cfg_token(pin)
    end
    err("pin " .. pin .. ": bad role base " .. base)
end
function Unit:_out_cfg_token(pin)
    local r = self.roles[pin] or { base = "out", mods = {} }
    if r.base == "oc" then
        if #r.mods == 1 and r.mods[1] == "up" then return "(" .. pin .. "):oc,up" end
        if #r.mods > 0 then err("oc on " .. pin .. ": only :up allowed") end
        return "(" .. pin .. "):oc"
    end
    return "(" .. pin .. "):out"
end
local function clause(pin, op, thr)
    if op == "eq" then return pin .. ":" .. thr end
    return pin .. ":" .. op .. ":" .. thr
end

function Unit:gpmp()
    if self.mode ~= MODES.GPIO then return nil end
    for p in pairs(self.roles) do
        if not GPIO_PIN_SET[p] then err("GPIO: pin " .. p .. " is not a usable channel") end
    end
    local dir_bm, pullen_bm, out_bm, od_bm = 0, 0, 0, 0
    for i, pin in ipairs(GPIO_PINS) do
        local r = self.roles[pin]
        if not r then err("GPIO: pin " .. pin .. " undefined -- all 8 must be set") end
        local base, mods = r.base, r.mods
        local b = lshift(1, i - 1)
        if base == "out" then
            dir_bm = dir_bm + b
            local kind = mods[1] or "0"
            if kind == "1" then out_bm = out_bm + b
            elseif kind ~= "0" then err("output " .. pin .. " must be 0 or 1") end
        elseif base == "oc" then
            dir_bm = dir_bm + b; od_bm = od_bm + b; out_bm = out_bm + b
            if #mods == 1 and mods[1] == "up" then pullen_bm = pullen_bm + b
            elseif #mods > 0 then err("oc on " .. pin .. ": only :up allowed") end
        elseif base == "in" then
            local pull = mods[1] or "none"
            if pull ~= "up" and pull ~= "down" and pull ~= "none" then err("input " .. pin .. " bad pull") end
            if pull ~= "none" then
                pullen_bm = pullen_bm + b
                if pull == "up" then out_bm = out_bm + b end
            end
        else
            err("GPIO pin " .. pin .. ": role " .. base .. " not allowed (in/out/oc)")
        end
    end
    local intcfg = self.il and 0x01 or 0x00
    return bytestr({ GPMP_VERSION, dir_bm, pullen_bm, out_bm, od_bm, intcfg })
end

function Unit:cntr()
    if self.mode ~= MODES.COUNTER then return nil end
    local chbytes = {}; for k = 1, #COUNTER_PINS do chbytes[k] = 0 end
    for label, r in pairs(self.roles) do
        local pin = COUNTER_PIN_ALIAS[label] or label
        if COUNTER_PIN_IDX[pin] == nil then err("COUNTER pin " .. label .. " not a usable pad") end
        local idx = COUNTER_PIN_IDX[pin]
        local base, mods = r.base, r.mods
        if base == "count" then
            local pull, edge = "none", "rising"
            for _, m in ipairs(mods) do
                if COUNTER_PULLS[m] then pull = m
                elseif COUNTER_EDGES[m] then edge = m
                else err("counter " .. label .. ": bad modifier " .. m) end
            end
            chbytes[idx + 1] = 1 + COUNTER_PULLS[pull] * 2 + COUNTER_EDGES[edge] * 8
        elseif COUNTER_BENCH_ROLES[base] then
            local role = bench_role_value(label, base, mods)
            if base == "dac" and pin ~= "D0" then err("COUNTER " .. label .. ": dac only on D0/A0") end
            chbytes[idx + 1] = role * 2
        else
            err("COUNTER pin " .. label .. ": role " .. base .. " must be count or a bench role")
        end
    end
    local r = self.cntr_rate
    local out = { CNTR_VERSION, r % 256, math.floor(r / 256) % 256 }
    for _, b in ipairs(chbytes) do out[#out + 1] = b end
    return bytestr(out)
end

function Unit:srvo()
    if self.mode ~= MODES.SERVO then return nil end
    local rolebytes = {}; for k = 1, #SERVO_PINS do rolebytes[k] = 0 end
    for label, r in pairs(self.roles) do
        local pin = SERVO_PIN_ALIAS[label] or label
        if pin == "D6" or pin == "A6" then err("SERVO pin " .. label .. " is the e-stop; can't declare") end
        if SERVO_PIN_IDX[pin] == nil then err("SERVO pin " .. label .. " not a usable pad") end
        local base, mods = r.base, r.mods
        if not SERVO_ROLES[base] then err("SERVO pin " .. label .. ": bad role " .. base) end
        if base == "dac" and pin ~= "D0" then err("SERVO " .. label .. ": dac only on D0/A0") end
        local role
        if base == "oc" then role = bench_role_value(label, base, mods)
        elseif #mods > 0 then err("SERVO pin " .. label .. ": role takes no modifiers")
        else role = SERVO_ROLES[base] end
        rolebytes[SERVO_PIN_IDX[pin] + 1] = role
    end
    local out = { SRVO_VERSION }
    for _, b in ipairs(rolebytes) do out[#out + 1] = b end
    return bytestr(out)
end

local ILCF_OFF = "off"

function Unit:_ilcf_adc(name, when, drive)
    if not next(drive) then err("ADC interlock needs a drive output") end
    local dkeys = sorted_keys(drive)
    if #dkeys > IL_MAX_OUTPUTS then err("too many outputs") end
    local groups = compile_expr(when, {}, self.vref, true)
    local streams, seen = {}, {}
    for _, g in ipairs(groups) do for _, lit in ipairs(g) do
        if not seen[lit[1]] then seen[lit[1]] = true; streams[#streams+1] = lit[1] end
    end end
    if #streams > IL_MAX_INPUTS then err("too many ADC streams") end
    local total = 0; for _, g in ipairs(groups) do total = total + #g end
    if total > IL_MAX_WATCHES then err("too many watch clauses") end
    local cfgt = {}; for _, d in ipairs(dkeys) do cfgt[#cfgt+1] = self:_out_cfg_token(d:upper()) end
    local gtxt = {}
    for _, g in ipairs(groups) do
        local cs = {}; for _, lit in ipairs(g) do cs[#cs+1] = clause(lit[1], lit[2], lit[3]) end
        gtxt[#gtxt+1] = table.concat(cs, ",")
    end
    local ok, er = {}, {}
    for _, label in ipairs(dkeys) do
        local v = drive[label]
        if v ~= 0 and v ~= 1 then err("drive value must be 0 or 1") end
        ok[#ok+1] = label:upper() .. ":" .. v; er[#er+1] = label:upper() .. ":" .. (1 - v)
    end
    local sections = { name, "cfg[" .. table.concat(cfgt, ",") .. "]", "watch[" .. table.concat(gtxt, "|") .. "]",
                       "out_ok[" .. table.concat(ok, ",") .. "]", "out_err[" .. table.concat(er, ",") .. "]" }
    local text = table.concat(sections, ";")
    if #text > IL_DSL_MAX then err("ilcf too long") end
    return text
end

function Unit:ilcf()
    if not MODES_WITH_ILCF[self.mode] then return nil end
    if self.il == nil or not self.il.when or self.il.when == "" then return ILCF_OFF end
    local name, when, drive = self.il.name, self.il.when, self.il.drive or {}
    if self.mode == MODES.ADC then return self:_ilcf_adc(name, when, drive) end
    if not next(self.roles) then err(MODE_NAME[self.mode] .. " mode needs pins()") end

    local groups = compile_expr(when, self.roles, self.vref, false)
    local inputs, seen = {}, {}
    for _, g in ipairs(groups) do for _, lit in ipairs(g) do
        if not seen[lit[1]] then seen[lit[1]] = true; inputs[#inputs+1] = lit[1] end
    end end
    for _, p in ipairs(inputs) do
        local r = self.roles[p]
        if not r or r.base == "out" or r.base == "oc" then err("watched pin " .. p .. " is not a declared input") end
    end
    if #inputs > IL_MAX_INPUTS then err("too many watched inputs") end
    local total = 0; for _, g in ipairs(groups) do total = total + #g end
    if total > IL_MAX_WATCHES then err("too many watch clauses (after DNF)") end
    local dkeys = sorted_keys(drive)
    if #dkeys > IL_MAX_OUTPUTS then err("too many outputs") end
    for _, label in ipairs(dkeys) do
        local r = self.roles[label]
        if not r or (r.base ~= "out" and r.base ~= "oc") then err("drive pin " .. label .. " not declared out/oc") end
    end

    local cfg_pins
    if self.mode == MODES.MIXED then
        cfg_pins = sorted_keys(self.roles)
    else
        cfg_pins = {}
        for _, p in ipairs(inputs) do cfg_pins[#cfg_pins+1] = p end
        local inset = {}; for _, p in ipairs(inputs) do inset[p] = true end
        for _, p in ipairs(dkeys) do if not inset[p] then cfg_pins[#cfg_pins+1] = p end end
    end

    local cfgt = {}; for _, p in ipairs(cfg_pins) do cfgt[#cfgt+1] = self:_cfg_token(p) end
    local gtxt = {}
    for _, g in ipairs(groups) do
        local cs = {}; for _, lit in ipairs(g) do cs[#cs+1] = clause(lit[1], lit[2], lit[3]) end
        gtxt[#gtxt+1] = table.concat(cs, ",")
    end
    local sections = { name, "cfg[" .. table.concat(cfgt, ",") .. "]", "watch[" .. table.concat(gtxt, "|") .. "]" }
    if next(drive) then
        local ok, er = {}, {}
        for _, label in ipairs(dkeys) do
            local v = drive[label]
            if v ~= 0 and v ~= 1 then err("drive value must be 0 or 1") end
            ok[#ok+1] = label .. ":" .. v; er[#er+1] = label .. ":" .. (1 - v)
        end
        sections[#sections+1] = "out_ok[" .. table.concat(ok, ",") .. "]"
        sections[#sections+1] = "out_err[" .. table.concat(er, ",") .. "]"
    end
    local text = table.concat(sections, ";")
    if #text > IL_DSL_MAX then err("ilcf too long") end
    return text
end

function Unit:files()
    local out = { idnt = self:idnt() }
    local gpmp = self:gpmp(); if gpmp then out.gpmp = gpmp end
    local cntr = self:cntr(); if cntr then out.cntr = cntr end
    local srvo = self:srvo(); if srvo then out.srvo = srvo end
    local ilcf = self:ilcf(); if ilcf then out.ilcf = ilcf end   -- ascii string == bytes
    return out
end

return M
