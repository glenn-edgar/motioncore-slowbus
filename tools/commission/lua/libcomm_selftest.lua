-- libcomm_selftest.lua -- codec checks (no hardware): crc8, m2s encode, s2m decode.
local _dir = (debug.getinfo(1, "S").source:sub(2)):match("(.*/)") or "./"
package.path = _dir .. "?.lua;" .. package.path
local lc = require("libcomm")
local bit = require("bit")

local function hex(s) local t = {}; for i = 1, #s do t[i] = string.format("%02x", s:byte(i)) end; return table.concat(t) end

-- 1) crc8 over the sample header+payload (for cross-check with Python)
local sample = { 1, 0x09, 0x01, 7, 4, 0x34, 0x12, 0x27, 0x02 }   -- addr,cmd,seq,len,reg-read body
print("crc8 = " .. string.format("0x%02x", lc._crc8(sample, #sample)))

-- 2) m2s encode of OP_SHELL_EXEC seq=7 body=<reqid 0x1234><cmd 0x0127 REG_READ><reg 0x02>
local body = string.char(0x34, 0x12, 0x27, 0x01, 0x02)
print("m2s  = " .. hex(lc._encode_m2s(0x0109, 7, body)))

-- 3) s2m decode round-trip: build an OP_SHELL_REPLY frame and decode it.
--    s2m body: addr cmd_lo cmd_hi seq ack_seq ack_status len <payload> crc
local function build_s2m(cmd, seq, payload)
    local b = { 1, bit.band(cmd, 0xFF), bit.rshift(cmd, 8), seq, 0, 0, #payload }
    for i = 1, #payload do b[#b + 1] = payload[i] end
    b[#b + 1] = lc._crc8(b, #b)
    -- SLIP wrap
    local w = { 0xC0 }
    for _, x in ipairs(b) do
        if x == 0xC0 then w[#w+1]=0xDB; w[#w+1]=0xDC
        elseif x == 0xDB then w[#w+1]=0xDB; w[#w+1]=0xDD else w[#w+1]=x end
    end
    w[#w + 1] = 0xC0
    return w
end
-- reply: req_id 0x1234, status 0, result {0x5A}  (a whoami-style reply)
local frame = build_s2m(0x0011, 7, { 0x34, 0x12, 0x00, 0x5A })
local dec = lc._make_decoder()
local got
for _, x in ipairs(frame) do lc._decoder_feed(dec, x, function(f) got = f end) end
local ok = got and got.cmd == 0x0011 and got.payload[1] == 0x34 and got.payload[2] == 0x12
          and got.payload[3] == 0x00 and got.payload[4] == 0x5A
print("decode = " .. (ok and "OK (cmd=0x0011 req=0x1234 status=0 result=0x5A)" or "FAIL"))
os.exit(ok and 0 or 1)
