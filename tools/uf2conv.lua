#!/usr/bin/env luajit
-- uf2conv.lua -- minimal bin -> UF2 converter, a drop-in for the vendored tinyusb
-- python `uf2conv.py` so the SAMD21 build needs NO python (LuaJIT-only toolchain).
--
-- Supports only the flags the build uses:
--   luajit uf2conv.lua -c -b <baseaddr> -f <familyid> -o <out.uf2> <in.bin>
--   -c  convert (the default / only mode)   -b  flash base address (e.g. 0x2000)
--   -f  UF2 family id (e.g. 0x68ed2b88)      -o  output .uf2        <in.bin> = input
--
-- 512-byte UF2 blocks with 256-byte payloads (what the SAMD21 UF2 bootloader writes
-- per block) -> byte-identical to `uf2conv.py -c -b -f -o`. See the UF2 spec.

local function le32(v)
    v = v % 0x100000000
    return string.char(v % 256, math.floor(v / 256) % 256,
                       math.floor(v / 65536) % 256, math.floor(v / 16777216) % 256)
end

local base, family, out, inp = 0x2000, 0, nil, nil
local i = 1
while i <= #arg do
    local a = arg[i]
    if     a == "-c" then              -- convert (default; no-op flag)
    elseif a == "-b" then i = i + 1; base   = tonumber(arg[i])
    elseif a == "-f" then i = i + 1; family = tonumber(arg[i])
    elseif a == "-o" then i = i + 1; out    = arg[i]
    else inp = a end
    i = i + 1
end
assert(inp and out, "usage: uf2conv.lua -c -b <base> -f <family> -o <out.uf2> <in.bin>")
assert(base and family, "missing/invalid -b or -f")

local fh = assert(io.open(inp, "rb")); local data = fh:read("*a") or ""; fh:close()

local UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
local UF2_FLAG_FAMILY = 0x00002000          -- familyID present
local PAYLOAD = 256
local nblk = math.ceil(#data / PAYLOAD); if nblk == 0 then nblk = 1 end
-- pad the image to a full multiple of 256 so EVERY block carries a 256-byte payload
-- (payloadSize=256), byte-identical to uf2conv.py (whose output the SAMD21 bootloader expects).
data = data .. string.rep("\0", nblk * PAYLOAD - #data)

local blocks = {}
for b = 0, nblk - 1 do
    local chunk = data:sub(b * PAYLOAD + 1, b * PAYLOAD + PAYLOAD)   -- always 256 now
    local hdr = le32(UF2_MAGIC0) .. le32(UF2_MAGIC1) .. le32(UF2_FLAG_FAMILY)
             .. le32(base + b * PAYLOAD) .. le32(#chunk) .. le32(b) .. le32(nblk) .. le32(family)
    blocks[#blocks + 1] = hdr .. chunk .. string.rep("\0", 476 - #chunk) .. le32(UF2_MAGIC_END)
end

local oh = assert(io.open(out, "wb")); oh:write(table.concat(blocks)); oh:close()
io.write(("[uf2conv.lua] %s -> %s: %d bytes, %d blocks @ 0x%X (family 0x%08X)\n")
         :format(inp, out, #data, nblk, base, family))
