-- cbor.lua -- minimal, deterministic CBOR encoder (RFC 8949 subset) for config
-- payloads: unsigned ints, text strings, arrays, maps (sorted keys), booleans.
-- Enough for the sam_slaves roster the Pico reads; cross-checked against Python
-- cbor2 in dsl tests. Decoding is not needed host-side (the Pico decodes).

local bit = require("bit")
local M = {}

-- Wrap a Lua string so enc() emits it as a CBOR byte string (major 2) instead of
-- a text string (major 3) -- e.g. the idnt `id` UUID, which the Pico reads via
-- cbor_str(CBOR_BYTES). Usage: cbor.encode({ id = cbor.bytes(raw8) }).
M._bytes_mt = {}
function M.bytes(s) return setmetatable({ s }, M._bytes_mt) end

local function head(major, n)
    local mb = major * 32                       -- major type in the top 3 bits
    if n < 24 then return string.char(mb + n)
    elseif n < 0x100 then return string.char(mb + 24, n)
    elseif n < 0x10000 then
        return string.char(mb + 25, bit.band(bit.rshift(n, 8), 0xff), bit.band(n, 0xff))
    else
        return string.char(mb + 26,
            bit.band(bit.rshift(n, 24), 0xff), bit.band(bit.rshift(n, 16), 0xff),
            bit.band(bit.rshift(n, 8), 0xff), bit.band(n, 0xff))
    end
end

local function is_array(t)
    local n = 0
    for _ in pairs(t) do n = n + 1 end
    return n == #t                              -- contiguous 1..#t and no string keys
end

local function enc(v)
    local t = type(v)
    if t == "number" then
        if v < 0 or v ~= math.floor(v) then error("cbor: only non-negative integers") end
        return head(0, v)                       -- major 0: unsigned int
    elseif t == "string" then
        return head(3, #v) .. v                 -- major 3: text string
    elseif t == "boolean" then
        return string.char(v and 0xf5 or 0xf4)  -- simple: true/false
    elseif t == "table" then
        if getmetatable(v) == M._bytes_mt then
            return head(2, #v[1]) .. v[1]        -- major 2: byte string
        elseif is_array(v) then
            local parts = { head(4, #v) }       -- major 4: array
            for i = 1, #v do parts[#parts + 1] = enc(v[i]) end
            return table.concat(parts)
        else
            local keys = {}                     -- major 5: map, deterministic (sorted keys)
            for k in pairs(v) do keys[#keys + 1] = k end
            table.sort(keys)
            local parts = { head(5, #keys) }
            for _, k in ipairs(keys) do parts[#parts + 1] = enc(k) .. enc(v[k]) end
            return table.concat(parts)
        end
    end
    error("cbor: unsupported type " .. t)
end

M.encode = enc
return M
