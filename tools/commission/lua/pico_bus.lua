#!/usr/bin/env luajit
-- pico_bus.lua -- drive the BC to poll a slave over RS-485 and watch the result.
--
--   luajit pico_bus.lua <bc_port> <slave_addr> [seconds]
--
-- Registers <slave_addr> in the BC roster (FLAG_ENABLED), sets a fast poll, enables
-- polling, then listens for the BC's bus events (SLAVE_UP/DOWN/STATUS) and finally
-- dumps LIST_SLAVES. Used for two-Pico bring-up: BC on <bc_port>, slave on the wire.

package.path = (arg[0]:match("^(.*/)") or "./") .. "?.lua;" .. package.path
local pl = require("picolink")

local CMD_BUS_REGISTER, CMD_BUS_LIST, CMD_BUS_SET_POLL = 0x0160, 0x0162, 0x0163
local CMD_BUS_POLL_ENABLE, CMD_BUS_CLEAR = 0x0164, 0x0165
local FLAG_ENABLED = 0x02

local bc_port = arg[1]
local addr    = tonumber(arg[2] or "1")
local secs    = tonumber(arg[3] or "4")
if not bc_port then io.stderr:write("usage: pico_bus.lua <bc_port> <slave_addr> [secs]\n"); os.exit(1) end

local function le16(n) return string.char(n % 256, math.floor(n / 256) % 256) end

local lk = pl.open(bc_port, 1.0)
local A = pl.ADDR_LOCAL_SHELL

-- clean roster, register the slave enabled, set a 200 ms poll, enable polling
lk:exec(A, CMD_BUS_CLEAR, "")
local st = lk:exec(A, CMD_BUS_REGISTER, string.char(addr, 0,0,0,0, FLAG_ENABLED))
io.write(("[bus] register slave 0x%02X (ENABLED) -> st=%d\n"):format(addr, st))
lk:exec(A, CMD_BUS_SET_POLL, le16(200) .. string.char(3, 2))   -- period=200ms misses=3 retries=2
lk:exec(A, CMD_BUS_POLL_ENABLE, string.char(1))
io.write(("[bus] poll enabled; watching %.1fs for bus events...\n"):format(secs))

local up, down, status = 0, 0, 0
lk:listen(secs, function(f)
    local name = pl.OP_NAME[f.cmd] or ("0x%04X"):format(f.cmd)
    if f.cmd == pl.OP_DBG_LOG then return end
    if f.cmd == 0x0016 then up = up + 1
    elseif f.cmd == 0x0015 then down = down + 1
    elseif f.cmd == 0x001B then status = status + 1 end
    io.write(("  %-16s addr=0x%02X len=%d\n"):format(name, f.addr, #f.payload))
end)

-- final roster snapshot (success = the slave is ALIVE; a fresh UNKNOWN->ALIVE is
-- announced silently, so the roster state -- not a SLAVE_UP event -- is the signal)
local _, l = lk:exec(A, CMD_BUS_LIST, "")
lk:close()
local alive = false
if l and #l >= 2 and l:byte(1) >= 1 then
    local st_names = { [0]="UNKNOWN", [1]="ALIVE", [2]="DEAD" }
    local state, misses, ago = l:byte(9), l:byte(10), (l:byte(11) or 0) + (l:byte(12) or 0) * 256
    alive = (state == 1)
    io.write(("[bus] LIST: slave 0x%02X state=%s misses=%d last_seen=%dms_ago\n")
             :format(l:byte(3), st_names[state] or state, misses, ago))
end
io.write(("[bus] events: SLAVE_UP=%d SLAVE_DOWN=%d STATUS=%d\n"):format(up, down, status))
io.write(alive and "[bus] RESULT: slave is ALIVE — responding to polls over RS-485 ✓\n"
                or  "[bus] RESULT: slave NOT alive — no poll response (check wiring/addr)\n")
os.exit(alive and 0 or 1)
