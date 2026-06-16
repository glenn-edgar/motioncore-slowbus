#!/usr/bin/env luajit
-- pico_regress.lua -- workbench regression for the Pico bus_controller over USB.
--
-- Test #1, Step 5: exercise the libcomm API surface with NO slaves and NO HIL pin
-- activity (so it is safe on any bench), covering both frame routes:
--   * appcore (0xFB): OP_REGISTER identity + KB0 MON_PING
--   * local shell (0x00): ECHO (payload+SLIP integrity), error path, roster CRUD,
--                         SET_POLL / POLL_ENABLE (RAM-only; never drives the wire)
-- Leaves the BC clean (roster empty, poll disabled). Exit code 0 = all pass.
--
--   luajit pico_regress.lua [port]

package.path = (arg[0]:match("^(.*/)") or "./") .. "?.lua;" .. package.path
local pl = require("picolink")

-- BC-local shell command ids (app/bus_controller/main.c)
local CMD_ECHO            = 0x0001
local CMD_BUS_REGISTER    = 0x0160
local CMD_BUS_LIST        = 0x0162
local CMD_BUS_SET_POLL    = 0x0163
local CMD_BUS_POLL_ENABLE = 0x0164
local CMD_BUS_CLEAR       = 0x0165
local SHELL_OK, SHELL_UNKNOWN = 0, 1

local port = arg[1]
if not port then
    local f = pl.enumerate()
    if #f == 0 then io.stderr:write("no Pico (2e8a) ttyACM found\n"); os.exit(1) end
    port = f[1].port
end
local lk = pl.open(port, 1.0)

local pass, fail = 0, 0
local function check(name, ok, detail)
    io.write(string.format("  [%s] %s%s\n", ok and "PASS" or "FAIL", name,
             detail and ("  -- " .. detail) or ""))
    if ok then pass = pass + 1 else fail = fail + 1 end
end
-- exec wrapper: returns (status, result) or (nil, errmsg) on timeout/throw
local function ex(addr, cmd, args)
    local ok, a, b = pcall(lk.exec, lk, addr, cmd, args or "")
    if not ok then return nil, tostring(a) end
    return a, b
end

io.write("[regress] bus_controller @ " .. port .. "\n")

-- 1) identity over the link
local info = lk:info(1.5)
check("OP_REGISTER identity", info ~= nil and info.uid and #info.uid == 16,
      info and ("class=0x%08X fw=%d uid=%s"):format(info.class_id, info.fw_version, info.uid) or "no REGISTER")

-- 2) appcore KB0 liveness
local okp, p = pcall(function() return lk:ping() end)
check("appcore MON_PING", okp and p and p.kb0_ver ~= nil,
      okp and ("uptime=%dms boot#%d kb0_ver=%d"):format(p.uptime_ms, p.boot, p.kb0_ver) or tostring(p))

-- 3) ECHO payload + SLIP-escape integrity (bytes incl 0xC0/0xDB/0x00)
local payload = string.char(0x01, 0xC0, 0xDB, 0x00, 0xDC, 0xDD, 0xFF, 0x55, 0xC0)
local st, res = ex(pl.ADDR_LOCAL_SHELL, CMD_ECHO, payload)
check("ECHO round-trip + SLIP escaping", st == SHELL_OK and res == payload,
      st == nil and res or ("st=%d len=%d match=%s"):format(st, #res, tostring(res == payload)))

-- 4) error path: unknown command id -> SHELL_UNKNOWN_CMD
local st4 = ex(pl.ADDR_LOCAL_SHELL, 0x09FF, "")
check("unknown cmd -> UNKNOWN_CMD(1)", st4 == SHELL_UNKNOWN, "st=" .. tostring(st4))

-- 5) roster CRUD (RAM-only; no bus traffic)
ex(pl.ADDR_LOCAL_SHELL, CMD_BUS_CLEAR, "")
local _, l0 = ex(pl.ADDR_LOCAL_SHELL, CMD_BUS_LIST, "")
check("roster empty after CLEAR", l0 and l0:byte(1) == 0, "count=" .. tostring(l0 and l0:byte(1)))

-- register addr=0x05, class_id=0x11223344 (LE), flags=0x01
local reg = string.char(0x05, 0x44, 0x33, 0x22, 0x11, 0x01)
local st5 = ex(pl.ADDR_LOCAL_SHELL, CMD_BUS_REGISTER, reg)
check("REGISTER_SLAVE -> OK", st5 == SHELL_OK, "st=" .. tostring(st5))

local _, l1 = ex(pl.ADDR_LOCAL_SHELL, CMD_BUS_LIST, "")
-- reply: [count, count, addr, class0..3, flags, state, misses, ago_lo, ago_hi]
check("LIST shows 1 slave addr=0x05", l1 and l1:byte(1) == 1 and l1:byte(3) == 0x05,
      l1 and ("count=%d addr=0x%02X"):format(l1:byte(1), l1:byte(3)) or "no reply")
check("LIST class_id round-trip 0x11223344",
      l1 and l1:byte(4) == 0x44 and l1:byte(5) == 0x33 and l1:byte(6) == 0x22 and l1:byte(7) == 0x11,
      l1 and ("%02X%02X%02X%02X"):format(l1:byte(7), l1:byte(6), l1:byte(5), l1:byte(4)) or "")

ex(pl.ADDR_LOCAL_SHELL, CMD_BUS_CLEAR, "")
local _, l2 = ex(pl.ADDR_LOCAL_SHELL, CMD_BUS_LIST, "")
check("roster empty after final CLEAR", l2 and l2:byte(1) == 0, "count=" .. tostring(l2 and l2:byte(1)))

-- 6) SET_POLL + POLL_ENABLE toggle (roster empty -> no addr to poll -> no wire activity)
local st6a = ex(pl.ADDR_LOCAL_SHELL, CMD_BUS_SET_POLL, string.char(0xF4, 0x01, 0x03, 0x02)) -- 500ms,3,2
check("SET_POLL -> OK", st6a == SHELL_OK, "st=" .. tostring(st6a))
local st6b = ex(pl.ADDR_LOCAL_SHELL, CMD_BUS_POLL_ENABLE, string.char(1))
local st6c = ex(pl.ADDR_LOCAL_SHELL, CMD_BUS_POLL_ENABLE, string.char(0))   -- leave disabled
check("POLL_ENABLE on/off -> OK", st6b == SHELL_OK and st6c == SHELL_OK,
      ("on=%s off=%s"):format(tostring(st6b), tostring(st6c)))

lk:close()
io.write(string.format("[regress] %d passed, %d failed\n", pass, fail))
os.exit(fail == 0 and 0 or 1)
