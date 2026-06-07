--[[
    bc_benchmark.lua — slow_bus M0+ BC benchmark chain_tree config.

    Static-footprint benchmark for the Pico W (RP2040 / M0+) bus-controller app
    core. Multi-KB, builtins-only (no user one_shot handlers, no streaming /
    controlled-node / avro / cbor), so the per-image registration stub omits the
    packet/controlled builtins and --gc-sections drops them.

      KB0 api_measurements : API command-dispatch state machine + blackboard
      KB1 interlock_alpha  : watchdog interlock
      KB2 interlock_beta   : for-loop guarded interlock

    Stage 1 (this file, via luajit) emits the IR; stage 2 (s_build_headers_binary)
    emits .ctb + _image.h + _registration.c/.h.
--]]

local ChainTreeMaster = require("chain_tree_master")

-- Shared mutable blackboard (one per configuration). Accessed by future user
-- handlers (step 5); defined here so its cost is in the benchmark image.
local function define_bc_blackboard(ct)
    ct:define_blackboard("bc_state")
        ct:bb_field("mode",            "int32",  0)
        ct:bb_field("measurement",     "float",  0.0)
        ct:bb_field("sample_count",    "uint32", 0)
        ct:bb_field("interlock_flags", "uint32", 0)
    ct:end_blackboard()
end

-- KB0 — API measurement surface: a command-dispatch state machine.
local function api_measurements_kb(ct, kb_name)
    ct:start_test(kb_name)

    local launch = ct:define_column("api_launch", nil, nil, nil, nil, nil, true)
    ct:asm_log_message("api_measurements: launch")

    local sm = ct:define_state_machine("api_sm", "api_sm",
        {"idle", "measure", "report"}, "idle", true)

    local s_idle = ct:define_state("idle", nil)
    ct:asm_log_message("api: idle")
    ct:asm_wait_time(1)
    ct:change_state(sm, "measure")
    ct:asm_halt()
    ct:end_column(s_idle)

    local s_measure = ct:define_state("measure", nil)
    ct:asm_log_message("api: measure")
    ct:asm_event_logger("api sample events", {"API_SAMPLE"})
    ct:asm_wait_time(1)
    ct:change_state(sm, "report")
    ct:asm_halt()
    ct:end_column(s_measure)

    local s_report = ct:define_state("report", nil)
    ct:asm_log_message("api: report")
    ct:asm_send_named_event(sm, "API_SAMPLE", {})
    ct:asm_wait_time(1)
    ct:change_state(sm, "idle")
    ct:asm_halt()
    ct:end_column(s_report)

    ct:end_state_machine(sm, "api_sm")

    ct:asm_wait_time(6)
    ct:asm_log_message("api: terminating state machine")
    ct:terminate_state_machine(sm)
    ct:asm_log_message("api: shutting down benchmark")
    ct:asm_terminate_system()
    ct:end_column(launch)

    ct:end_test()
end

-- KB1 — interlock alpha: watchdog interlock.
local function interlock_alpha_kb(ct, kb_name)
    ct:start_test(kb_name)

    local wd_col = ct:define_column("ila_watch", nil, nil, nil, nil, nil, true)
    ct:asm_log_message("interlock_alpha: arming watchdog")
    local wd = ct:asm_watch_dog_node(30, true, "ILA_TIMEOUT",
        { message = "interlock alpha watchdog timeout" })
    ct:asm_enable_watch_dog(wd)
    ct:asm_wait_time(2)
    ct:asm_log_message("interlock_alpha: pat")
    ct:asm_pat_watch_dog(wd)
    ct:asm_wait_time(2)
    ct:asm_pat_watch_dog(wd)
    ct:asm_halt()
    ct:end_column(wd_col)

    ct:end_test()
end

-- KB2 — interlock beta: for-loop guarded interlock.
local function interlock_beta_kb(ct, kb_name)
    ct:start_test(kb_name)

    local launch = ct:define_column("ilb_launch", nil, nil, nil, nil, nil, true)
    local loop = ct:define_for_column("ilb_loop", 3, nil, nil, nil, nil, nil, true)

    local body = ct:define_column("ilb_body", nil, nil, nil, nil, nil, true)
    ct:asm_log_message("interlock_beta: iteration")
    ct:asm_wait_time(1)
    ct:asm_terminate()
    ct:end_column(body)

    ct:end_column(loop)
    ct:define_join_link(loop)
    ct:asm_log_message("interlock_beta: loop complete")
    ct:asm_halt()
    ct:end_column(launch)

    ct:end_test()
end

-- =========================================================================
-- Entry point (mirrors incremental_build.lua main)
-- =========================================================================

if arg then
    if #arg ~= 1 then
        print("Usage: luajit bc_benchmark.lua <output_ir_file>")
        os.exit(1)
    end

    local ir_file = arg[1]
    local ct = ChainTreeMaster.new(ir_file)

    define_bc_blackboard(ct)
    api_measurements_kb(ct, "api_measurements")
    interlock_alpha_kb(ct, "interlock_alpha")
    interlock_beta_kb(ct, "interlock_beta")

    ct:check_and_generate_yaml()
end
