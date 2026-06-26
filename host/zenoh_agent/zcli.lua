#!/usr/bin/env luajit
-- zcli.lua -- minimal zenoh RPC client to exercise the slow_bus agent's key.
--   luajit zcli.lua <locator> <key> '<json-op>'
-- e.g. luajit zcli.lua tcp/127.0.0.1:46170 slow_bus/bc/cmd '{"op":"app_echo","msg":"hi"}'
-- Needs LD_LIBRARY_PATH=<repo>/vendor/zenoh/lib (zenoh-pico .so).
local _dir = (debug.getinfo(1, "S").source:sub(2)):match("(.*/)") or "./"
package.path = _dir .. "../../vendor/zenoh/?.lua;" .. package.path
local zrpc = require("zenoh_rpc")
local zt   = require("zenoh_token")

local locator = arg[1] or "tcp/127.0.0.1:46170"
local key     = arg[2] or "slow_bus/bc/cmd"
local op_json = arg[3] or '{"op":"ping"}'

local cli = zrpc.Client.new({ locators = { locator }, mode = "client", client_name = "slowbus-cli" })
cli:connect()
local ok, reply = pcall(cli.call, cli, zt.hash(key), op_json, 6000)
pcall(cli.disconnect, cli); pcall(cli.destroy, cli)
if not ok then io.stderr:write("call failed: " .. tostring(reply) .. "\n"); os.exit(1) end
print(reply)
