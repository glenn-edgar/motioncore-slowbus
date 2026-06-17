#!/usr/bin/env luajit
-- pico_slave.lua -- end-to-end DATA round-trip over RS-485:
--   host -> BC (USB) -> RS-485 -> slave -> RS-485 -> BC -> host.
-- Sends CMD_ECHO to <slave_addr> via the BC and checks the echoed result.
--   luajit pico_slave.lua <bc_port> <slave_addr> [message]

package.path = (arg[0]:match("^(.*/)") or "./") .. "?.lua;" .. package.path
local pl = require("picolink")

local CMD_BUS_REGISTER, CMD_BUS_CLEAR = 0x0160, 0x0165
local FLAG_ENABLED, NODE_CMD_ECHO = 0x02, 0x0001

local bc_port = arg[1]
local addr    = tonumber(arg[2] or "9")
local msg     = arg[3] or "pico-bus-echo!"
if not bc_port then io.stderr:write("usage: pico_slave.lua <bc_port> <slave_addr> [msg]\n"); os.exit(1) end

local lk = pl.open(bc_port, 1.5)
local A = pl.ADDR_LOCAL_SHELL
-- register the slave so the BC tracks it (mark_alive needs the roster entry)
lk:exec(A, CMD_BUS_CLEAR, "")
lk:exec(A, CMD_BUS_REGISTER, string.char(addr, 0, 0, 0, 0, FLAG_ENABLED))

-- shell-exec CMD_ECHO straight at the slave addr: the BC injects it on the bus
-- and relays the slave's OP_SHELL_REPLY back to us (matched on req_id).
local ok, st, res = pcall(lk.exec, lk, addr, NODE_CMD_ECHO, msg, 1.5)
lk:close()
if not ok then io.write("[slave] FAILED: " .. tostring(st) .. "\n"); os.exit(2) end

io.write(("[slave] CMD_ECHO -> addr 0x%02X: status=%d result=%q\n"):format(addr, st, res))
local good = (st == 0 and res == msg)
io.write(good and "[slave] RESULT: DATA round-trip OK — BC<->slave over RS-485 ✓\n"
              or  "[slave] RESULT: FAILED (status/echo mismatch)\n")
os.exit(good and 0 or 1)
