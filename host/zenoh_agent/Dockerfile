# syntax=docker/dockerfile:1.6
#
# slow_bus zenoh device agent -- bridges the bus controller's libcomm/host_link
# command surface onto a zenoh key. The BC dials in over WiFi (UPLINK=wifi); the
# agent accepts it (picolink TCP-server) and is a CLIENT of the zenoh router (no
# zenohd here). zenoh-pico stays on Linux -- never on the MCU.
#
# The prebuilt zenoh-pico .so need glibc >= 2.34; the bookworm base provides it
# (the Pi host glibc may be older -- that's why this is containerised). Build on
# the Pi (aarch64 native). Build context = repo root:
#   docker build -f host/zenoh_agent/Dockerfile -t slow_bus/zenoh-agent:dev .
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends luajit tini \
    && rm -rf /var/lib/apt/lists/*

# zenoh-pico + RPC/token FFI shims (prebuilt aarch64; need glibc >= 2.34)
COPY vendor/zenoh/lib/*.so /usr/local/lib/
RUN ldconfig

# Mirror the repo layout so agent.lua's ../../ package.path resolves in-container.
WORKDIR /app
COPY vendor/zenoh/*.lua                 /app/vendor/zenoh/
COPY tools/commission/lua/picolink.lua  /app/tools/commission/lua/
COPY host/zenoh_agent/agent.lua         /app/host/zenoh_agent/agent.lua

# Env (override at `docker run -e`): ZENOH_LOCATOR, BC_PORT (BC UDP port), RPC_KEY.
ENTRYPOINT ["/usr/bin/tini", "--", "luajit", "/app/host/zenoh_agent/agent.lua"]
