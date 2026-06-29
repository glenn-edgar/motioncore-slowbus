#!/usr/bin/env luajit
-- pico_app_il_clear.lua -- Thread 3 -> Thread 2: the chain-tree engine (kbapp) clears
-- the interlock's latched trips via CMD_APP_IL_CLEAR (0x0302).
--   luajit pico_app_il_clear.lua <bc_port>
--
-- Requires the bench TEST interlock config (ilc0=watch[gp3:eq:0], ilc1=watch[gp5:eq:0])
-- with the GP2<->GP3 / GP4<->GP5 jumpers. watch[gpN:eq:0] is SATISFIED when the input
-- reads 0 (drive the paired OUTPUT low) and VIOLATED when it reads 1 (drive high).
-- Demonstrates: engine clears a latched-but-resolved trip; fail-safe re-latch otherwise.

package.path = (arg[0]:match("^(.*/)") or "./") .. "?.lua;" .. package.path
local pl = require("picolink")

local CMD_GPIO_WRITE, CMD_IL_STATUS, CMD_APP_IL_CLEAR = 0x0101, 0x0211, 0x0302
local bc_port = arg[1]
if not bc_port then io.stderr:write("usage: pico_app_il_clear.lua <bc_port>\n"); os.exit(1) end

local lk = pl.open(bc_port, 2.0)
local function drive(pin, lvl) lk:exec(pl.ADDR_APPCORE, CMD_GPIO_WRITE, string.char(0, pin, lvl), 1.5) end
local function settle() os.execute("sleep 0.2") end
local function resolve() drive(2, 0); drive(4, 0); settle() end   -- inputs->0 = satisfied
local function violate() drive(2, 1); drive(4, 1); settle() end   -- inputs->1 = violated
local function gveto() local _, r = lk:exec(pl.ADDR_APPCORE, CMD_IL_STATUS, "", 1.5); return r:byte(2) end
local function kbclear() local _, r = lk:exec(pl.ADDR_APPCORE, CMD_APP_IL_CLEAR, "", 1.5); settle(); return r:byte(1) end

local steps = {}
violate();          steps[1] = (gveto() == 1)              -- trip
resolve();          steps[2] = (gveto() == 1)              -- still latched (held)
local ack = kbclear(); steps[3] = (ack == 1 and gveto() == 0)  -- engine clears the latch
violate();          steps[4] = (gveto() == 1)              -- re-trip
kbclear();          steps[5] = (gveto() == 1)              -- FAIL-SAFE: re-latches
resolve(); kbclear(); steps[6] = (gveto() == 0)            -- cleared

for i, ok in ipairs(steps) do io.write(("step%d %s  "):format(i, ok and "OK" or "FAIL")) end
io.write("\n")
local good = true; for _, ok in ipairs(steps) do good = good and ok end
io.write(good and "[il-clear] RESULT: engine (kbapp) clears Thread-2 interlock; fail-safe holds ✓\n"
              or  "[il-clear] RESULT: FAILED\n")
lk:close()
os.exit(good and 0 or 1)
