--[[
    kb0.lua — ChainTree DSL source for the bus_controller chain-tree engine.

    This is the AUTHORITATIVE source for kb0/incr/*.c. It is reconstructed from
    (and reproduces) the committed kb0.json IR. Regenerate with:

        tools/gen_kb.sh

    which runs this file to (re)produce kb0.json, then runs the json->C codegen
    pipeline into kb0/incr/.

    KB model (Thread 3 — chain-tree application):
      Each engine command is a column that loops: WAIT-for-event -> one-shot
      handler -> reset. Thread 1 (cfl_embed_pre_tick) injects the event that the
      column's WAIT leaf is parked on; the one-shot handler (a C "user function"
      in main.c) does the work and pushes the reply up to USB.

      kb0   — monitor: CMD_MON_PING (event 20), CMD_MON_SNAPSHOT (event 21)
      kb1   — api:     CMD_ADC_READ  (event 22)
      kbapp — Thread-3 application: CMD_APP_ECHO (23), CMD_APP_ECHO_TO (24),
              CMD_APP_IL_CLEAR (25)

    Event ids are assigned in registration order after the 20 core CFL_* events
    (0..19), so the build order kb0 -> kb1 -> kbapp (ping, snapshot, adc, echo,
    echo_to, il_clear) keeps the ids stable (20..25) — mirrored by EVENT_CMD_* in main.c.
]]

local ChainTreeMaster = require("chain_tree_master")

-- Shared WAIT-leaf parameters (mirror the committed IR).
local CMD_TIMEOUT_S   = 3600                 -- effectively "wait forever"
local CMD_TIMEOUT_EVT = "CFL_SECOND_EVENT"   -- 1 Hz tick that decrements the timeout
local CMD_TIMEOUT_FN  = "MON_CMD_TIMEOUT"    -- one-shot fired if the wait times out
local CMD_TIMEOUT_DAT = { error_message = "cmd timeout" }

-- A command column: park on `event_name`, run one-shot `handler`, then reset (loop).
local function command_column(ct, col_name, event_name, handler)
    local col = ct:define_column(col_name, nil, nil, nil, nil, nil, true)
    ct:asm_wait_for_event(event_name, 1, true, CMD_TIMEOUT_S,
        CMD_TIMEOUT_FN, CMD_TIMEOUT_EVT, CMD_TIMEOUT_DAT)
    ct:asm_one_shot_handler(handler, {})
    ct:asm_reset()
    ct:end_column(col)
end

local function build_kb0(ct)
    ct:start_test("kb0")
    command_column(ct, "cmd_", "CMD_MON_PING",     "MON_PING_REPLY")
    command_column(ct, "cmd_", "CMD_MON_SNAPSHOT", "MON_SNAPSHOT")
    ct:end_test()
end

local function build_kb1(ct)
    ct:start_test("kb1")
    command_column(ct, "api_", "CMD_ADC_READ", "ADC_READ")
    ct:end_test()
end

-- Thread-3 application KB.
--   C1: CMD_APP_ECHO (event 23) — local echo, proving Thread 1 -> engine -> reply.
--   C2: CMD_APP_ECHO_TO (event 24) — the engine ORIGINATES a bus message to a slave
--       node and the master correlates the reply (node-to-node, master-initiated).
--   IL: CMD_APP_IL_CLEAR (event 25) — the engine (Thread 3) clears Thread 2's latched
--       interlock trips (interlock_request_global_clear); fail-safe re-latch if still violated.
local function build_kbapp(ct)
    ct:start_test("kbapp")
    command_column(ct, "app_", "CMD_APP_ECHO",     "APP_ECHO")
    command_column(ct, "app_", "CMD_APP_ECHO_TO",  "APP_ECHO_TO")
    command_column(ct, "app_", "CMD_APP_IL_CLEAR", "APP_IL_CLEAR")
    ct:end_test()
end

-- 10 Hz ADC streams the engine can read from the shared blackboard.
local function build_blackboard(ct)
    ct:define_blackboard("adc_streams")
    for ch = 0, 2 do
        ct:bb_field("adc" .. ch .. "_mean", "int32", 0)
        ct:bb_field("adc" .. ch .. "_max",  "int32", 0)
        ct:bb_field("adc" .. ch .. "_rms",  "int32", 0)
    end
    ct:end_blackboard()
end

local out = arg[1] or "kb0.json"
local ct = ChainTreeMaster.new(out)
build_kb0(ct)
build_kb1(ct)
build_kbapp(ct)
build_blackboard(ct)
ct:check_and_generate()
