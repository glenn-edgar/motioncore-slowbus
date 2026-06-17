#!/usr/bin/env luajit
-- cfg_image.lua -- build the Pico config-region image (the "second" of the
-- two-step flash) as a UF2 targeted at the top-64KB config region.
--
--   luajit cfg_image.lua [--uid HEX] [--out cfg.uf2]
--                        [--chip 0] [--variant 1] [--addr 0]
--                        [--flash-size 2097152] [--port /dev/ttyACMx]
--
-- With no --uid it auto-detects the connected Pico's UID over libcomm (OP_REGISTER
-- via picolink), so the image is bound to the board it's flashed onto -- exactly
-- the mis-flash guard boot_identity enforces. Emits one SAMD21-boot-store-format
-- entry (`idnt`) into a UF2 block at the region base; flash with `picotool load`.
--
-- idnt CBOR: { v:1, ch:<chip>, vr:<variant>, ad:<addr>, id:<8-byte UUID> }.

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
              poll = "200:3:2" }
local i = 1
while i <= #arg do
    local a = arg[i]
    local function nextval() i = i + 1; return arg[i] end
    if     a == "--uid"        then opt.uid = nextval()
    elseif a == "--out"        then opt.out = nextval()
    elseif a == "--chip"       then opt.chip = tonumber(nextval())
    elseif a == "--variant"    then opt.variant = tonumber(nextval())
    elseif a == "--addr"       then opt.addr = tonumber(nextval())
    elseif a == "--slvr"       then opt.slvr = nextval()
    elseif a == "--poll"       then opt.poll = nextval()
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
local idnt = cbor.encode({ v = 1, ch = opt.chip, vr = opt.variant, ad = opt.addr,
                           id = cbor.bytes(uid_raw) })
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
    "[cfg_image] %s: idnt{v=1,ch=%d,vr=%d,ad=%d,id=%s}%s -> %d entr%s -> UF2 @ 0x%08X (%d B)\n",
    opt.out, opt.chip, opt.variant, opt.addr, opt.uid, slvr_desc,
    #entries, #entries == 1 and "y" or "ies", base, #uf2))
