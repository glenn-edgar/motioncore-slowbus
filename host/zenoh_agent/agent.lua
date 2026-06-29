-- agent.lua -- slow_bus zenoh device agent (W3b).
--
-- Bridges the bus controller's libcomm/host_link command surface onto a zenoh key,
-- so the SAME commands that work over USB (picolink) work through the fleet's zenohd
-- router. The BC sends to us over WiFi/UDP (UPLINK=wifi); we recv its datagrams
-- (picolink.recv_udp) and speak host_link over that socket. zenoh-pico stays here on
-- Linux — never on the MCU. (UDP-only; TCP mode dropped.)
--
-- Requests arrive as JSON {op=...} on the key; binary payloads/results are hex.
-- Env: ZENOH_LOCATOR (tcp/127.0.0.1:46169), ZENOH_MODE (client),
--      BC_PORT (47447, the UDP port the BC sends to), RPC_KEY (slow_bus/bc/cmd), POLL_MS (5).
-- Run on the Pi with LD_LIBRARY_PATH=<repo>/vendor/zenoh/lib (the zenoh-pico .so).

local _dir = (debug.getinfo(1, "S").source:sub(2)):match("(.*/)") or "./"
package.path = _dir .. "../../vendor/zenoh/?.lua;"
            .. _dir .. "../../tools/commission/lua/?.lua;" .. package.path

local zrpc = require("zenoh_rpc")
local zps  = require("zenoh_pubsub")   -- §17 step8: pub/sub uplink (feedback PUBLISH + command SUBSCRIBE)
local zt   = require("zenoh_token")
local json = require("mini_json")
local pl   = require("picolink")

local function env(k, d) local v = os.getenv(k); if v == nil or v == "" then return d end; return v end
local LOCATORS = {}
for s in (env("ZENOH_LOCATOR", "tcp/127.0.0.1:46169")):gmatch("[^,]+") do LOCATORS[#LOCATORS+1] = s end
local MODE     = env("ZENOH_MODE", "client")
local BC_PORT  = tonumber(env("BC_PORT", env("TCP_PORT", "47447")))  -- UDP port the BC sends to (TCP_PORT kept as a legacy alias)
local KEY      = env("RPC_KEY", "slow_bus/bc/cmd")
-- §17 step8 pub/sub keys + optional USB-serial transport (else the BC dials in over UDP).
local FEEDBACK_KEY = env("FEEDBACK_KEY", "slow_bus/bc/feedback")  -- agent PUBLISHes batched per-cycle feedback here
local CTL_KEY      = env("CTL_KEY", "slow_bus/bc/ctl")            -- agent SUBSCRIBEs; clients PUBLISH fire-and-forget commands here
local BC_SERIAL    = env("BC_SERIAL", "")                          -- if set (e.g. /dev/ttyACM1), talk to the BC over USB instead of UDP
local POLL_S   = (tonumber(env("POLL_MS", "5")) or 5) / 1000
local EXEC_TO  = (tonumber(env("EXEC_MS", "2500")) or 2500) / 1000
-- Agent-side keepalive (SAMD51 lesson): when idle, ping the BC every KEEPALIVE_S to keep
-- the tunnel warm + detect a silently-dead link faster (a failed ping forces a re-accept).
-- Safe on the Pico W (CYW43); on the RTL8720 a keepalive made the eRPC blip WORSE, so it
-- was OFF there — tune per transport. 0 disables.
local KEEPALIVE_S = tonumber(env("KEEPALIVE_S", "10")) or 10

local function log(...) io.write("[slowbus-agent] ", ...); io.write("\n"); io.flush() end
local function to_hex(s) return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end)) end
local function from_hex(h) h = h or ""; return (h:gsub("%x%x", function(b) return string.char(tonumber(b,16)) end)) end

-- ---- device (the BC over WiFi) ---------------------------------------------
local lk
local function bc_accept()
    if lk then pcall(function() lk:close() end); lk = nil end
    if BC_SERIAL ~= "" then
        log(("opening the BC over USB serial %s ..."):format(BC_SERIAL))
        lk = pl.open(BC_SERIAL, EXEC_TO)        -- §17 step8: USB transport (bench test without WiFi)
    else
        log(("waiting for the BC (udp) on port %d ..."):format(BC_PORT))
        lk = pl.recv_udp(BC_PORT, EXEC_TO, 86400)   -- UDP-only (TCP mode dropped)
    end
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

-- ---- §17 step8 pub/sub: feedback PUBLISH + command SUBSCRIBE -----------------
-- The BC emits OP_BUS_FEEDBACK (batched per-cycle feedback). We parse it and PUBLISH a
-- JSON record set to FEEDBACK_KEY so any number of subscribers fan out. Clients PUBLISH
-- fire-and-forget commands to CTL_KEY; we drain them into the BC's producer path
-- (CMD_BUS_MSG_INJECT) -- no reply (replies, like UDP, are the app's job via feedback).
local OP_BUS_FEEDBACK   = 0x001C
local CMD_BUS_MSG_INJECT = 0x0169
local ps, ctl_sub, FEEDBACK_TOK
local fb_pub_n, ctl_inj_n = 0, 0

local function on_bc_frame(f)
    if f.cmd ~= OP_BUS_FEEDBACK or not ps then return end
    local p = f.payload                       -- [n_rec][addr][len][bytes]* (1-indexed byte array)
    local nrec = p[1] or 0
    local recs, i = {}, 2
    for _ = 1, nrec do
        local addr, len = p[i], p[i + 1]; i = i + 2
        local b = {}
        for j = 1, (len or 0) do b[j] = string.char(p[i] or 0); i = i + 1 end
        recs[#recs + 1] = { addr = addr, hex = to_hex(table.concat(b)) }
    end
    ps:publish(FEEDBACK_TOK, json.encode({ n = nrec, recs = recs }))
    fb_pub_n = fb_pub_n + 1
    if fb_pub_n <= 8 then log(("feedback PUBLISH #%d n=%d"):format(fb_pub_n, nrec)) end
end

local function drain_ctl()                    -- inject queued commands into the BC producer
    if not ctl_sub then return end
    while true do
        local msg = ctl_sub:poll()
        if not msg then break end
        local ok, c = pcall(json.decode, msg.payload)
        if ok and type(c) == "table" and c.addr then
            local cnt  = c.count or 1
            local body = string.char(c.addr % 256, cnt % 256) .. from_hex(c.hex or "")
            local ok2, st = pcall(dev_exec, pl.ADDR_LOCAL_SHELL, CMD_BUS_MSG_INJECT, body)  -- fire-and-forget
            ctl_inj_n = ctl_inj_n + 1
            if ctl_inj_n <= 8 then log(("ctl INJECT #%d addr=%d ok=%s st=%s"):format(ctl_inj_n, c.addr, tostring(ok2), tostring(st))) end
        end
    end
end

-- ---- main ------------------------------------------------------------------
bc_accept()
local srv = zrpc.Server.new({ locators = LOCATORS, mode = MODE, client_name = "slowbus-bc" })
local q = srv:register(zt.hash(KEY), 32)
srv:start()
-- §17 step8: a second zenoh session for pub/sub (separate .so from the RPC server).
ps = zps.PubSub.new({ locators = LOCATORS, mode = MODE })
ps:connect()
FEEDBACK_TOK = zt.hash(FEEDBACK_KEY)
ctl_sub = ps:subscribe(zt.hash(CTL_KEY), 64)
log(("zenoh %s locator=%s  rpc='%s'  feedback->'%s'  ctl<-'%s'%s"):format(
        MODE, table.concat(LOCATORS, ","), KEY, FEEDBACK_KEY, CTL_KEY,
        BC_SERIAL ~= "" and ("  (serial " .. BC_SERIAL .. ")") or ""))
log("ready.")
local last_act = os.time()
while true do
    local req = q:poll()
    if req then
        reply(req); last_act = os.time()
    elseif lk then
        -- Drain the BC's frames (REGISTER/HEARTBEAT keep its TX buffer from filling;
        -- OP_BUS_FEEDBACK gets parsed + PUBLISHed by on_bc_frame).
        pcall(function() lk:listen(POLL_S, on_bc_frame) end)
        drain_ctl()                                  -- inject any queued CTL commands
        -- Keepalive: ping the BC if the link's been idle, to keep it warm + detect death.
        if KEEPALIVE_S > 0 and (os.time() - last_act) >= KEEPALIVE_S then
            pcall(dev_exec, pl.ADDR_APPCORE, pl.CMD_MON_PING, "")   -- failure -> dev_exec re-accepts
            last_act = os.time()
        end
    else
        pl.sleep(POLL_S)
    end
end
