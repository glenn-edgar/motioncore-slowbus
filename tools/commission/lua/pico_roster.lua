#!/usr/bin/env luajit
-- pico_roster.lua -- show the BC's live roster (LIST_SLAVES), READ-ONLY. Used to
-- verify a 'slvr' config roster that loaded at boot (no host registration).
--   luajit pico_roster.lua [bc_port]

package.path = (arg[0]:match("^(.*/)") or "./") .. "?.lua;" .. package.path
local pl = require("picolink")
local CMD_BUS_LIST = 0x0162
local ST = { [0] = "UNKNOWN", [1] = "ALIVE", [2] = "DEAD" }

local port = arg[1] or (pl.enumerate()[1] or {}).port
if not port then io.stderr:write("no BC port\n"); os.exit(1) end

local reps = tonumber(arg[2]) or 1   -- LIST this many times within ONE connection
                                     -- (a disconnect re-arms/reloads -> resets state)
local lk = pl.open(port, 1.0)
for rep = 1, reps do
    pl.sleep(0.4)   -- let the poll sweep run before sampling
    local st, l = lk:exec(pl.ADDR_LOCAL_SHELL, CMD_BUS_LIST, "")
    if st ~= 0 or not l or #l < 1 then io.write("[roster] LIST failed st=" .. tostring(st) .. "\n"); os.exit(1) end
    local count = l:byte(1)
    io.write(("[roster] %s (sample %d): %d slave(s)\n"):format(port, rep, count))
    local pos = 3   -- after [count, count]
    for _ = 1, count do
        if pos + 9 > #l then break end
        local addr   = l:byte(pos)
        local variant = l:byte(pos + 1) + l:byte(pos + 2) * 256 + l:byte(pos + 3) * 65536 + l:byte(pos + 4) * 16777216
        local flags, state, misses = l:byte(pos + 5), l:byte(pos + 6), l:byte(pos + 7)
        local ago = l:byte(pos + 8) + l:byte(pos + 9) * 256
        io.write(("  addr=0x%02X variant=%d flags=0x%02X state=%-7s misses=%d seen=%dms_ago\n")
                 :format(addr, variant, flags, ST[state] or tostring(state), misses, ago))
        pos = pos + 10
    end
end
lk:close()
