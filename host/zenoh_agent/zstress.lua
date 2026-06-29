#!/usr/bin/env luajit
-- zstress.lua -- throughput/latency stress over the zenoh path. Connects ONCE, fires N
-- serial RPC calls (app_echo by default), and reports avg/min/max latency + msgs/sec.
--   luajit zstress.lua <locator> <key> <N> [op]
-- e.g. luajit zstress.lua tcp/127.0.0.1:46170 slow_bus/bc/cmd 300 app_echo
-- Needs LD_LIBRARY_PATH=<repo>/vendor/zenoh/lib.
local _dir = (debug.getinfo(1, "S").source:sub(2)):match("(.*/)") or "./"
package.path = _dir .. "../../vendor/zenoh/?.lua;" .. package.path
local ffi  = require("ffi")
local zrpc = require("zenoh_rpc")
local zt   = require("zenoh_token")

ffi.cdef[[ typedef struct { long tv_sec; long tv_nsec; } zts; int clock_gettime(int, zts*); ]]
local ts = ffi.new("zts[1]")
local function now() ffi.C.clock_gettime(1, ts); return tonumber(ts[0].tv_sec)*1000 + tonumber(ts[0].tv_nsec)/1e6 end

local locator = arg[1] or "tcp/127.0.0.1:46170"
local key     = arg[2] or "slow_bus/bc/cmd"
local N       = tonumber(arg[3] or "300")
local op      = arg[4] or "app_echo"

local payloads = {
    app_echo    = function(i) return ('{"op":"app_echo","msg":"s%d"}'):format(i) end,
    ping        = function(_) return '{"op":"ping"}' end,
    app_echo_to = function(i) return ('{"op":"app_echo_to","addr":9,"msg":"s%d"}'):format(i) end,
}
local mk = payloads[op] or error("unknown op " .. op)

local cli = zrpc.Client.new({ locators = { locator }, mode = "client", client_name = "slowbus-stress" })
cli:connect()
local tok = zt.hash(key)

-- warm up (first call pays connection/route setup)
pcall(cli.call, cli, tok, mk(0), 5000)

local lo, hi, sum, okc, errc = 1e9, 0, 0, 0, 0
local t_start = now()
for i = 1, N do
    local t0 = now()
    local ok, r = pcall(cli.call, cli, tok, mk(i), 5000)
    local dt = now() - t0
    if ok then okc = okc + 1; sum = sum + dt; if dt < lo then lo = dt end; if dt > hi then hi = dt end
    else errc = errc + 1 end
end
local wall = (now() - t_start) / 1000
pcall(cli.disconnect, cli); pcall(cli.destroy, cli)

io.write(("[zstress] op=%s N=%d  ok=%d err=%d\n"):format(op, N, okc, errc))
if okc > 0 then
    io.write(("[zstress] latency avg=%.1f ms  min=%.1f  max=%.1f\n"):format(sum/okc, lo, hi))
    io.write(("[zstress] throughput %.1f msg/s over %.2fs wall\n"):format(okc/wall, wall))
end
