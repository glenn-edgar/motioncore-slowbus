# vendored: chain_tree static-link runtime (subset)

`engine/src/` + `engine/include/` are a vendored **subset** of the canonical
chain_tree static runtime, compiled into the firmware with the Pico SDK toolchain
(ABI-safe) and `--gc-sections` (the linker drops everything KB0 doesn't reach).

Source of truth: `~/knowledge_base_assembly/c_programs_and_containers/build_blocks/
chain_tree_c/` — `runtime_h/src` (engine core) + `runtime_functions/src` (node types).
**Do not edit `engine/src` here — re-lift upstream.**

What's included vs the full runtime:
- INCLUDED: cfl_runtime, cfl_engine, cfl_event_queue, cfl_blackboard, cfl_heap,
  cfl_heap_arena_allocate, cfl_perm, CT_Tree_Walker, cfl_common_functions, fnv1a,
  json_node_decoder (node-data), cfl_main/one_shot/boolean_functions,
  cfl_node_control_support, cfl_sm_functions.
- REPLACED for the target:
  - `cfl_exception.c` — embed version here (host one uses execinfo/backtrace).
  - the timer — `port/rp2040/cfl_timer_rp2040.c` (host one uses Linux calendar time);
    it also calls `cfl_embed_pre_tick()` each tick = the inter-core event-injection seam.
- EXCLUDED (KB0 doesn't use; would drag deps or not cross-compile): cfl_function_loader
  (binary-path registrar), cfl_file_loader (fopen), cfl_image_loader (binary image path),
  cfl_cbor_*/cfl_json_* (packet subsystem), cfl_se_* (s_engine bridge), cfl_streaming,
  avro, cfl_exception_support, cfl_supervisor_support. `chaintree_support.c` comes from
  the generated `kb0/incr/` (not runtime_h/src) to avoid a duplicate.

`cfl_embed_stubs.h` is force-included (CMake) when compiling these so the runtime's
diagnostic printf/fprintf/puts can't corrupt the binary USB frame stream.

The KB0 chain (`kb0/incr/`) is generated from `chain_tree_c/dsl_tests/dsl_tests_c/kb0/
kb0.lua` (regen without `KB0_HOST_TEST` = no host stimulus).
