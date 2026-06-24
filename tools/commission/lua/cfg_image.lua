#!/usr/bin/env luajit
-- cfg_image.lua -- build the Pico config-region image (the "second" of the
-- two-step flash) as a UF2 targeted at the top-64KB config region.
--
--   luajit cfg_image.lua [--uid HEX] [--out cfg.uf2]
--                        [--chip 0] [--variant 1] [--addr 0] [--speed BAUD]
--                        [--flash-size 2097152] [--port /dev/ttyACMx]
--
-- With no --uid it auto-detects the connected Pico's UID over libcomm (OP_REGISTER
-- via picolink), so the image is bound to the board it's flashed onto -- exactly
-- the mis-flash guard boot_identity enforces. Emits one SAMD21-boot-store-format
-- entry (`idnt`) into a UF2 block at the region base; flash with `picotool load`.
--
-- idnt CBOR: { v:1, ch:<chip>, vr:<variant>, ad:<addr>, sp:<baud,opt>, id:<8-byte UUID> }.

package.path = (arg[0]:match("^(.*/)") or "./") .. "?.lua;" .. package.path
local bit  = require("bit")
local cbor = require("cbor")

-- ---- store-entry + UF2 codec ----------------------------------------------
local STORE_MAGIC, STORE_DATA_MAX = 0x10C0FFEE, 240
local UF2_FAMILY_RP2040 = 0xE48BFF56   -- (RP2350 differs; BC target is pico_w)

local function le32(n)
    return string.char(bit.band(n, 0xff), bit.band(bit.rshift(n, 8), 0xff),
                       bit.band(bit.rshift(n, 16), 0xff), bit.band(bit.rshift(n, 24), 0xff))
end

-- CRC-8/AUTOSAR over a byte string -- matches firmware entry_crc / SAMD21 store_crc.
local function crc8_autosar(s)
    local crc = 0xFF
    for i = 1, #s do
        crc = bit.bxor(crc, s:byte(i))
        for _ = 1, 8 do
            if bit.band(crc, 0x80) ~= 0 then crc = bit.band(bit.bxor(bit.lshift(crc, 1), 0x2F), 0xFF)
            else crc = bit.band(bit.lshift(crc, 1), 0xFF) end
        end
    end
    return bit.band(bit.bxor(crc, 0xFF), 0xFF)
end

-- One 256-B store row (cfg_entry_t): magic, seq, name[4], len, crc, pad[2], data[240].
local function build_entry(name, seq, data)
    local n4 = (name .. "    "):sub(1, 4)
    assert(#data <= STORE_DATA_MAX, "entry data > 240 B")
    local len = #data
    local crc = crc8_autosar(n4 .. string.char(len) .. data)
    local row = le32(STORE_MAGIC) .. le32(seq) .. n4 .. string.char(len, crc, 0, 0)
              .. data .. string.rep("\0", STORE_DATA_MAX - len)
    assert(#row == 256, "entry row must be 256 B, got " .. #row)
    return row
end

local function uf2_block(addr, payload, blkno, nblk, family)
    assert(#payload <= 476)
    local hdr = le32(0x0A324655) .. le32(0x9E5D5157) .. le32(0x00002000) .. le32(addr)
             .. le32(#payload) .. le32(blkno) .. le32(nblk) .. le32(family)
    return hdr .. payload .. string.rep("\0", 476 - #payload) .. le32(0x0AB16F30)
end

local function hex_to_bytes(h)
    h = (h or ""):gsub("%s", ""):gsub("^0[xX]", "")
    assert(#h % 2 == 0 and #h > 0, "UID must be non-empty even-length hex")
    local out = {}
    for i = 1, #h, 2 do out[#out + 1] = string.char(tonumber(h:sub(i, i + 1), 16)) end
    return table.concat(out)
end

-- ---- args -----------------------------------------------------------------
-- --slvr "addr:variant:flags,addr:variant:flags"   (variant default 3=SLAVE_RS485,
--                                                   flags default 2=ENABLED)
-- --poll "period_ms:max_misses:tcp_retries[:window_us]"   (default 200:3:2:0)
local opt = { out = "cfg.uf2", chip = 0, variant = 1, addr = 0, flash_size = 2 * 1024 * 1024,
              poll = "200:3:2", il = {} }
local i = 1
while i <= #arg do
    local a = arg[i]
    local function nextval() i = i + 1; return arg[i] end
    if     a == "--uid"        then opt.uid = nextval()
    elseif a == "--out"        then opt.out = nextval()
    elseif a == "--chip"       then opt.chip = tonumber(nextval())
    elseif a == "--variant"    then opt.variant = tonumber(nextval())
    elseif a == "--addr"       then opt.addr = tonumber(nextval())
    elseif a == "--speed" or a == "--baud" then opt.baud = tonumber(nextval())  -- optional RS-485 bus speed -> idnt 'sp'
    elseif a == "--slvr"       then opt.slvr = nextval()
    elseif a == "--poll"       then opt.poll = nextval()
    elseif a == "--io"         then opt.io = nextval()    -- hwio HIL roles: "r0,r1,..,r7" (GP2..GP9; HWIO_ROLE_* 0..6)
    elseif a == "--adc"        then opt.adc = nextval()   -- hwio ADC annotation: "label:unit:num:den,..." (<=3 chans)
    elseif a == "--il"         then                       -- repeatable: "N:<dsl>" arms slot N (0..9)
        local v = nextval()
        local s, dsl = v:match("^(%d+):(.*)$")            -- split on FIRST colon (DSL contains colons)
        assert(s and dsl and #dsl > 0, "bad --il (want N:<dsl>): " .. tostring(v))
        s = tonumber(s); assert(s >= 0 and s <= 9, "--il slot out of range 0..9: " .. s)
        opt.il[#opt.il + 1] = { slot = s, dsl = dsl }
    elseif a == "--flash-size" then opt.flash_size = tonumber(nextval())
    elseif a == "--port"       then opt.port = nextval()
    else error("unknown arg: " .. a) end
    i = i + 1
end

-- ---- UID: explicit or auto-detected over the link -------------------------
if not opt.uid then
    local pl = require("picolink")
    local found = pl.enumerate()
    if #found == 0 then error("no --uid given and no Pico (2e8a) connected to auto-detect") end
    local port = opt.port or found[1].port
    local lk = pl.open(port, 1.5)
    local info = lk:info(2.0); lk:close()
    if not info then error("no OP_REGISTER seen on " .. port .. " to read the UID") end
    opt.uid = info.uid
    io.write("[cfg_image] auto-detected UID " .. opt.uid .. " from " .. port .. "\n")
end

-- ---- build the config entries --------------------------------------------
-- All entries share one 4 KB flash sector, so they MUST go in one UF2 (a separate
-- picotool load of one row would erase the sector and wipe the others).
local function split(s, sep) local t = {}; for x in (s or ""):gmatch("([^" .. sep .. "]+)") do t[#t+1] = x end; return t end

local entries = {}   -- { {name=..., data=...}, ... } in row order

-- idnt (always)
local uid_raw = hex_to_bytes(opt.uid)
assert(#uid_raw == 8, "Pico UID must be 8 bytes (16 hex chars), got " .. #uid_raw)
local idnt_map = { v = 1, ch = opt.chip, vr = opt.variant, ad = opt.addr,
                   id = cbor.bytes(uid_raw) }
if opt.baud then idnt_map.sp = opt.baud end   -- optional bus speed; absent -> firmware default
local idnt = cbor.encode(idnt_map)
assert(#idnt <= STORE_DATA_MAX, "idnt CBOR too big: " .. #idnt)
entries[#entries + 1] = { name = "idnt", data = idnt }

-- slvr (optional, master roster)
local slvr_desc = ""
if opt.slvr then
    local pp = split(opt.poll, ":")
    local slaves = {}
    for _, e in ipairs(split(opt.slvr, ",")) do
        local f = split(e, ":")
        local addr = tonumber(f[1])
        assert(addr, "bad --slvr entry: " .. e)
        slaves[#slaves + 1] = { addr, tonumber(f[2]) or 3, tonumber(f[3]) or 0x02 }  -- addr,variant,flags
    end
    local slvr = cbor.encode({
        v = 1, p = tonumber(pp[1]) or 200, m = tonumber(pp[2]) or 3,
        r = tonumber(pp[3]) or 2, w = tonumber(pp[4]) or 0, s = slaves })
    assert(#slvr <= STORE_DATA_MAX, "slvr CBOR too big: " .. #slvr .. " B (>" .. STORE_DATA_MAX .. ")")
    entries[#entries + 1] = { name = "slvr", data = slvr }
    slvr_desc = (" + slvr{%d slaves, poll %s}"):format(#slaves, opt.poll)
end

-- hwio (optional, frozen HIL pin-role map + ADC annotation) -----------------
-- --io  "r0,r1,..,r7"  : up to 8 role ints for GP2..GP9 (HWIO_ROLE_*: 0=unused,
--                        1=in, 2=in_pullup, 3=in_pulldown, 4=out, 5=servo, 6=pulse).
-- --adc "L:U:N:D,.."   : up to 3 channels; each "label:unit:num:den" (any field
--                        may be empty -> omitted; value = raw*num/den).
local hwio_desc = ""
if opt.io or opt.adc then
    local hw = { v = 1 }
    if opt.io then
        local roles = {}
        for _, t in ipairs(split(opt.io, ",")) do
            local r = tonumber(t); assert(r and r >= 0 and r <= 6, "bad --io role: " .. t .. " (want 0..6)")
            roles[#roles + 1] = r
        end
        assert(#roles <= 8, "--io has " .. #roles .. " roles (max 8, GP2..GP9)")
        hw.io = roles
    end
    if opt.adc then
        -- Split on ',' PRESERVING empties so channels stay positional (ADC0/1/2).
        local clist = {}; for x in (opt.adc .. ","):gmatch("([^,]*),") do clist[#clist + 1] = x end
        local chans = {}
        for _, e in ipairs(clist) do
            local f = {}; for x in (e .. ":::"):gmatch("([^:]*):") do f[#f + 1] = x end  -- pad so trailing empties survive
            local ch = {}
            if f[1] and f[1] ~= "" then ch.l = f[1] end
            if f[2] and f[2] ~= "" then ch.u = f[2] end
            if f[3] and f[3] ~= "" then ch.n = tonumber(f[3]) or error("bad --adc num: " .. f[3]) end
            if f[4] and f[4] ~= "" then ch.d = tonumber(f[4]) or error("bad --adc den: " .. f[4]) end
            -- An empty {} would CBOR-encode as an array, not a map (the reader wants a
            -- map); require at least a label so channels stay positional + well-typed.
            assert(next(ch), "empty --adc channel #" .. #chans + 1 .. " (give at least a label, e.g. 'vbus')")
            chans[#chans + 1] = ch
        end
        assert(#chans <= 3, "--adc has " .. #chans .. " channels (max 3)")
        hw.ad = chans
    end
    local hwio = cbor.encode(hw)
    assert(#hwio <= STORE_DATA_MAX, "hwio CBOR too big: " .. #hwio .. " B (>" .. STORE_DATA_MAX .. ")")
    entries[#entries + 1] = { name = "hwio", data = hwio }
    hwio_desc = (" + hwio{io=%d,adc=%d}"):format(hw.io and #hw.io or 0, hw.ad and #hw.ad or 0)
end

-- ilcN (optional, Thread-2 interlock DSL per slot; ilc0..ilc9) ---------------
-- Raw DSL text (NOT CBOR): name;cfg[(pin):mode,..];watch[..];out_ok[..];out_err[..].
-- Pin names: gp0..gp29 (shared veto = gp0) and adc0..2 (== ain0..2 -> GP26/27/28).
-- Each slot independent; absent slots = empty. The veto is the union of all slots.
local ilcf_desc = ""
if #opt.il > 0 then
    local seen = {}
    for _, e in ipairs(opt.il) do
        assert(not seen[e.slot], "duplicate --il slot " .. e.slot); seen[e.slot] = true
        assert(#e.dsl <= 128, ("--il slot %d text %dB (max 128, IL_DSL_MAX)"):format(e.slot, #e.dsl))
        entries[#entries + 1] = { name = "ilc" .. e.slot, data = e.dsl }
    end
    ilcf_desc = (" + ilc{%d slot%s}"):format(#opt.il, #opt.il == 1 and "" or "s")
end

-- ---- entries -> rows -> multi-block UF2 ------------------------------------
local base = 0x10000000 + opt.flash_size - 0x10000   -- top 64 KB of flash
local blocks = {}
for n, e in ipairs(entries) do
    local row = build_entry(e.name, n, e.data)        -- seq = row index (all distinct names anyway)
    blocks[#blocks + 1] = uf2_block(base + (n - 1) * 256, row, n - 1, #entries, UF2_FAMILY_RP2040)
end
local uf2 = table.concat(blocks)

local f = assert(io.open(opt.out, "wb")); f:write(uf2); f:close()
io.write(string.format(
    "[cfg_image] %s: idnt{v=1,ch=%d,vr=%d,ad=%d,%sid=%s}%s -> %d entr%s -> UF2 @ 0x%08X (%d B)\n",
    opt.out, opt.chip, opt.variant, opt.addr,
    opt.baud and string.format("sp=%d,", opt.baud) or "", opt.uid, slvr_desc .. hwio_desc .. ilcf_desc,
    #entries, #entries == 1 and "y" or "ies", base, #uf2))
