#!/bin/bash
# gen_kb.sh - Regenerate the chain-tree engine C from the DSL source.
#
#   DSL (app/bus_controller/kb0/kb0.lua)
#     -> JSON IR  (app/bus_controller/kb0/kb0.json)
#     -> C files  (app/bus_controller/kb0/incr/*.c)
#
# Run from the repo root. Requires luajit (on the dev box and the Pi).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF="$ROOT/reference/chain_tree_c"
KB_DIR="$ROOT/app/bus_controller/kb0"
DSL="$KB_DIR/kb0.lua"
JSON="$KB_DIR/kb0.json"
INCR="$KB_DIR/incr"

# Resolve require("chain_tree_master") and require("lua_support.xxx").
export LUA_PATH="$REF/lua_dsl/?.lua;$REF/lua_dsl/?/init.lua;;"

echo "[gen_kb] DSL -> JSON: $DSL -> $JSON"
luajit "$DSL" "$JSON"

# --no-support: the authoritative generic runtime header (chaintree_support.{h,c})
# lives in engine/include/ (it carries the blackboard bb_table field this vendored
# codegen copy predates). Don't let the pipeline emit a stale incr/ copy.
echo "[gen_kb] JSON -> C: $JSON -> $INCR"
bash "$REF/s_build_headers_luajit.sh" "$JSON" "$INCR" chaintree_handle --no-support

echo "[gen_kb] done."
