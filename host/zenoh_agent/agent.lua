-- agent.lua -- slow_bus zenoh device agent (W3b).
--
-- Bridges the bus controller's libcomm/host_link command surface onto a zenoh key,
-- so the SAME commands that work over USB (picolink) work through the fleet's zenohd
-- router. The BC dials in over WiFi (UPLINK=wifi); we accept it (picolink.listen_tcp)
-- and speak host_link over that socket. zenoh-pico stays here on Linux — never on the MCU.
--
-- Requests arrive as JSON {op=...} on the key; binary payloads/results are hex.
-- Env: ZENOH_LOCATOR (tcp/127.0.0.1:46169), ZENOH_MODE (client),
--      TCP_PORT (47447, the port the BC dials), RPC_KEY (slow_bus/bc/cmd), POLL_MS (5).
-- Run on the Pi with LD_LIBRARY_PATH=<repo>/vendor/zenoh/lib (the zenoh-pico .so).

local _dir = (debug.getinfo(1, "S").source:sub(2)):match("(.*/)") or "./"
package.path = _dir .. "../../vendor/zenoh/?.lua;"
            .. _dir .. "../../tools/commission/lua/?.lua;" .. package.path

local zrpc = require("zenoh_rpc")
local zt   = require("zenoh_token")
local json = require("mini_json")
local pl   = require("picolink")

local function env(k, d) local v = os.getenv(k); if v == nil or v == "" then return d end; return v end
local LOCATORS = {}
for s in (env("ZENOH_LOCATOR", "tcp/127.0.0.1:46169")):gmatch("[^,]+") do LOCATORS[#LOCATORS+1] = s end
local MODE     = env("ZENOH_MODE", "client")
local TCP_PORT = tonumber(env("TCP_PORT", "47447"))
local KEY      = env("RPC_KEY", "slow_bus/bc/cmd")
local POLL_S   = (tonumber(env("POLL_MS", "5")) or 5) / 1000
local EXEC_TO  = (tonumber(env("EXEC_MS", "2500")) or 2500) / 1000

local function log(...) io.write("[slowbus-agent] ", ...); io.write("\n"); io.flush() end
local function to_hex(s) return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end)) end
local function from_hex(h) h = h or ""; return (h:gsub("%x%x", function(b) return string.char(tonumber(b,16)) end)) end

-- ---- device (the BC over WiFi) ---------------------------------------------
local lk
local function bc_accept()
    if lk then pcall(function() lk:close() end); lk = nil end
    log(("waiting for the BC to dial in on tcp/%d ..."):format(TCP_PORT))
    lk = pl.listen_tcp(TCP_PORT, EXEC_TO, 86400)   -- block until the BC connects
    log("BC connected.")
end
-- exec with one re-accept on transport failure (BC re-dial after a WiFi blip).
local function dev_exec(addr, cmd, args)
    if not lk then bc_accept() end
    local ok, st, r = pcall(lk.exec, lk, addr, cmd, args or "", EXEC_TO)
    if ok then return st, r end
    bc_accept()
    return lk:exec(addr, cmd, args or "", EXEC_TO)
end

-- ---- ops: JSON {op=...} -> reply table -------------------------------------
local CMD_APP_ECHO, CMD_APP_ECHO_TO = 0x0300, 0x0301
local ops = {}
function ops.ping()        local st = dev_exec(pl.ADDR_APPCORE, pl.CMD_MON_PING, ""); return { status = st } end
function ops.app_echo(a)
    local st, r = dev_exec(pl.ADDR_APPCORE, CMD_APP_ECHO, a.msg or "")
    return { status = st, echo = (r and #r >= 1) and r:sub(2) or "" }   -- reply [ver][echo]
end
function ops.app_echo_to(a)
    local st, r = dev_exec(pl.ADDR_APPCORE, CMD_APP_ECHO_TO, string.char(a.addr or 9) .. (a.msg or ""))
    return { status = st, echo = r or "" }                              -- reply [echo]
end
function ops.il_status(a)
    local st, r = dev_exec(a.addr or pl.ADDR_APPCORE, pl.CMD_INTERLOCK_STATUS, "")
    return { status = st, gveto = (r and #r >= 2) and r:byte(2) or -1, hex = to_hex(r or "") }
end
function ops.il_clear(a)   local st = dev_exec(a.addr or pl.ADDR_APPCORE, pl.CMD_INTERLOCK_CLEAR, ""); return { status = st } end
function ops.exec(a)       -- generic: {addr, cmd, hex}
    local st, r = dev_exec(a.addr or pl.ADDR_APPCORE, a.cmd, from_hex(a.hex))
    return { status = st, hex = to_hex(r or "") }
end

local function handle(payload)
    local req = json.decode(payload)
    if type(req) ~= "table" or not req.op then error("bad request (need {op:...})") end
    local fn = ops[req.op]; if not fn then error("unknown op: " .. tostring(req.op)) end
    return json.encode(fn(req))
end
local function reply(req)
    local ok, out = pcall(handle, req:payload())
    if ok then req:reply(out) else req:reply_error(tostring(out)); log("err: " .. tostring(out)) end
end

-- ---- main ------------------------------------------------------------------
bc_accept()
local srv = zrpc.Server.new({ locators = LOCATORS, mode = MODE, client_name = "slowbus-bc" })
local q = srv:register(zt.hash(KEY), 32)
srv:start()
log(("zenoh %s locator=%s  serving '%s'"):format(MODE, table.concat(LOCATORS, ","), KEY))
log("ready.")
while true do
    local req = q:poll()
    if req then
        reply(req)
    elseif lk then
        -- Drain the BC's continuous REGISTER/HEARTBEAT frames so its TX buffer never
        -- fills (a full buffer makes the BC give up + re-dial, stranding our socket).
        pcall(function() lk:listen(POLL_S, function() end) end)
    else
        pl.sleep(POLL_S)
    end
end
