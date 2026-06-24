#!/usr/bin/env luajit
-- pico_app_echo.lua -- Thread-3 chain-tree app echo tests (drive via the master @ 0xFB).
--
--   Local (C1):  luajit pico_app_echo.lua <bc_port> [message]
--       host -> BC -> Thread 1 -> engine (kbapp) -> reply over USB.
--       reply (after picolink strips [req_id][status]): [ver u8][echo...].
--
--   To node (C2): luajit pico_app_echo.lua <bc_port> --to <addr> [message]
--       host -> BC -> engine (kbapp) ORIGINATES a bus echo to node <addr> ->
--       slave echoes -> master correlates -> reply over USB.
--       reply (after strip): [echo...] (the slave's NODE_CMD_ECHO payload).

package.path = (arg[0]:match("^(.*/)") or "./") .. "?.lua;" .. package.path
local pl = require("picolink")

local CMD_APP_ECHO    = 0x0300
local CMD_APP_ECHO_TO = 0x0301

local bc_port = arg[1]
if not bc_port then
    io.stderr:write("usage: pico_app_echo.lua <bc_port> [--to <addr>] [message]\n"); os.exit(1)
end

local to_addr, msg
if arg[2] == "--to" then
    to_addr = tonumber(arg[3]); msg = arg[4] or "hello-node!"
    if not to_addr then io.stderr:write("--to needs an address\n"); os.exit(1) end
else
    msg = arg[2] or "hello-kbapp!"
end

local lk = pl.open(bc_port, 2.0)
local ok, st, res
if to_addr then
    ok, st, res = pcall(lk.exec, lk, pl.ADDR_APPCORE, CMD_APP_ECHO_TO,
                        string.char(to_addr) .. msg, 2.0)
else
    ok, st, res = pcall(lk.exec, lk, pl.ADDR_APPCORE, CMD_APP_ECHO, msg, 2.0)
end
lk:close()
if not ok then io.write("[app-echo] FAILED: " .. tostring(st) .. "\n"); os.exit(2) end

local echoed, tag
if to_addr then
    echoed = res                                    -- slave echo, no ver byte
    tag = ("CMD_APP_ECHO_TO addr=0x%02X"):format(to_addr)
else
    local ver = #res >= 1 and res:byte(1) or -1     -- kbapp reply carries a ver byte
    echoed = #res >= 1 and res:sub(2) or ""
    tag = ("CMD_APP_ECHO ver=%d"):format(ver)
end

io.write(("[app-echo] %s: status=%d echoed=%q\n"):format(tag, st, echoed))
local good = (st == 0 and echoed == msg)
if to_addr then
    io.write(good and "[app-echo] RESULT: engine-originated node-to-node echo OK ✓\n"
                  or  "[app-echo] RESULT: FAILED (status/echo mismatch)\n")
else
    io.write(good and "[app-echo] RESULT: Thread1->Thread3(kbapp)->reply OK ✓\n"
                  or  "[app-echo] RESULT: FAILED (status/echo mismatch)\n")
end
os.exit(good and 0 or 1)
