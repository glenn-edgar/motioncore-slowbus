#!/usr/bin/env luajit
-- pico_app_echo.lua -- Thread-3 C1 proof: host -> BC (USB) -> Thread 1 routes the
-- app opcode up to the chain-tree engine (kbapp) -> engine one-shot echoes the
-- payload -> reply back over USB. Exercises the master alone (no slave).
--   luajit pico_app_echo.lua <bc_port> [message]
--
-- CMD_APP_ECHO (0x0300) reply payload (after picolink strips [req_id][status]):
--   [ver u8][echoed bytes...]

package.path = (arg[0]:match("^(.*/)") or "./") .. "?.lua;" .. package.path
local pl = require("picolink")

local CMD_APP_ECHO = 0x0300

local bc_port = arg[1]
local msg     = arg[2] or "hello-kbapp!"
if not bc_port then
    io.stderr:write("usage: pico_app_echo.lua <bc_port> [message]\n"); os.exit(1)
end

local lk = pl.open(bc_port, 1.5)
local ok, st, res = pcall(lk.exec, lk, pl.ADDR_APPCORE, CMD_APP_ECHO, msg, 1.5)
lk:close()
if not ok then io.write("[app-echo] FAILED: " .. tostring(st) .. "\n"); os.exit(2) end

local ver    = #res >= 1 and res:byte(1) or -1
local echoed = #res >= 1 and res:sub(2) or ""
io.write(("[app-echo] CMD_APP_ECHO @ 0x%02X: status=%d ver=%d echoed=%q\n")
         :format(pl.ADDR_APPCORE, st, ver, echoed))

local good = (st == 0 and echoed == msg)
io.write(good and "[app-echo] RESULT: Thread1->Thread3(kbapp)->reply OK ✓\n"
              or  "[app-echo] RESULT: FAILED (status/echo mismatch)\n")
os.exit(good and 0 or 1)
