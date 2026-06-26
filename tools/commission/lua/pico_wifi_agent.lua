#!/usr/bin/env luajit
-- pico_wifi_agent.lua -- W3a: minimal libcomm-over-TCP "agent". Accepts the bus
-- controller's WiFi dial-in and drives operational host_link round-trips over WiFi
-- (MON_PING, app echo, node-to-node echo) -- the operational test the dump server
-- couldn't do. Same host_link codec as the USB picolink; only the transport differs.
-- No zenoh yet (that's W3b). Run on the Pi (the BC dials this host:port from 'neti').
--   luajit pico_wifi_agent.lua [port] [slave_addr]
package.path = (arg[0]:match("^(.*/)") or "./") .. "?.lua;" .. package.path
local pl = require("picolink")

local CMD_APP_ECHO, CMD_APP_ECHO_TO = 0x0300, 0x0301
local port  = tonumber(arg[1] or "47447")
local slave = tonumber(arg[2] or "9")

io.write(("[agent] listening on tcp/%d for the BC dial-in...\n"):format(port))
local lk = pl.listen_tcp(port, 2.0, 45)
io.write("[agent] BC connected.\n")

local function try(label, fn)
    local ok, st, r = pcall(fn)
    if not ok then io.write(("[agent] %-14s EXC: %s\n"):format(label, tostring(st))); return false end
    return st, r
end

local ok = true

-- 1) MON_PING (kb0 liveness through the engine)
do
    local st, r = try("MON_PING", function() return lk:exec(pl.ADDR_APPCORE, pl.CMD_MON_PING, "", 2.0) end)
    local good = (st == 0)
    io.write(("[agent] MON_PING       status=%s %s\n"):format(tostring(st), good and "OK" or "FAIL"))
    ok = ok and good
end

-- 2) CMD_APP_ECHO (local engine echo; reply after strip = [ver][echo])
do
    local msg = "wifi-agent-echo"
    local st, r = try("APP_ECHO", function() return lk:exec(pl.ADDR_APPCORE, CMD_APP_ECHO, msg, 2.0) end)
    local echo = (type(r) == "string" and #r >= 1) and r:sub(2) or ""
    local good = (st == 0 and echo == msg)
    io.write(("[agent] APP_ECHO       status=%s echo=%q %s\n"):format(tostring(st), echo, good and "OK" or "FAIL"))
    ok = ok and good
end

-- 3) CMD_APP_ECHO_TO (master engine -> slave engine over RS-485; reply = [echo])
do
    local msg = "wifi-node2node"
    local st, r = try("APP_ECHO_TO", function()
        return lk:exec(pl.ADDR_APPCORE, CMD_APP_ECHO_TO, string.char(slave) .. msg, 2.5)
    end)
    local good = (st == 0 and r == msg)
    io.write(("[agent] APP_ECHO_TO %-2d status=%s echo=%q %s\n"):format(slave, tostring(st), tostring(r), good and "OK" or "FAIL"))
    ok = ok and good
end

lk:close()
io.write(ok and "[agent] RESULT: operational host_link round-trip over WiFi OK \xE2\x9C\x93\n"
            or  "[agent] RESULT: FAILED\n")
os.exit(ok and 0 or 1)
