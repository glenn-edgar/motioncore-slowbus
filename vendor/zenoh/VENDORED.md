# vendored zenoh (from ~/xiao_blocks/vendor/zenoh, 2026-06-26)

LuaJIT zenoh-pico RPC/pubsub bindings + prebuilt **aarch64** shared libs (run on the
Pi only). Used by host/zenoh_agent/ to bridge the BC's libcomm/host_link surface onto a
zenoh key. zenoh-pico stays on Linux — never on the MCU.

- `zenoh_rpc.lua` / `zenoh_token.lua` / `zenoh_pubsub.lua` / `mini_json.lua`
- `lib/*.so` (aarch64): set `LD_LIBRARY_PATH=vendor/zenoh/lib` to load.
