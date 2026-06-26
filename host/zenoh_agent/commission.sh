#!/usr/bin/env bash
# commission.sh -- bring up slow_bus's zenoh stack on the Pi as managed containers.
# This repo OWNS its zenoh router (on its OWN port, distinct from xiao's :46169) AND
# the device agent. Both run with `--restart unless-stopped` so they survive reboots +
# crashes UNTIL `down`. Run ON THE PI after `build.sh` has built the agent image.
#
#   commission.sh [up|down|status|logs [name]|build]      (default: up)
#
# Config (env overrides):
#   ZENOH_PORT=46170     this repo's router port (xiao_blocks owns :46169)
#   ROUTER_BIND=0.0.0.0  0.0.0.0 = reachable off-box; 127.0.0.1 = Pi-only
#   TCP_PORT=47447       the port the BC dials over WiFi (matches 'neti')
#   RPC_KEY=slow_bus/bc/cmd
set -euo pipefail

HERE="$(cd "$(dirname "$0")/../.." && pwd)"   # repo root = build context
ZENOH_PORT="${ZENOH_PORT:-46170}"
ROUTER_BIND="${ROUTER_BIND:-0.0.0.0}"
TCP_PORT="${TCP_PORT:-47447}"
RPC_KEY="${RPC_KEY:-slow_bus/bc/cmd}"
ROUTER_IMAGE="${ROUTER_IMAGE:-eclipse/zenoh:latest}"
AGENT_IMAGE="${AGENT_IMAGE:-slow_bus/zenoh-agent:dev}"
ROUTER_NAME="${ROUTER_NAME:-slowbus-zenoh-router}"
AGENT_NAME="${AGENT_NAME:-slowbus-zenoh-agent}"
RESTART="${RESTART:-unless-stopped}"
ps_fmt='  {{.Names}}\t{{.Status}}\t{{.Ports}}'

build() {
    echo ">> build $AGENT_IMAGE (aarch64, debian:bookworm) — context $HERE"
    docker build -f "$HERE/host/zenoh_agent/Dockerfile" -t "$AGENT_IMAGE" "$HERE"
}
down() {
    docker rm -f "$AGENT_NAME" "$ROUTER_NAME" >/dev/null 2>&1 || true
    echo "down: $AGENT_NAME + $ROUTER_NAME stopped + removed"
}
up() {
    docker rm -f "$AGENT_NAME" "$ROUTER_NAME" >/dev/null 2>&1 || true
    echo ">> router '$ROUTER_NAME' on tcp/$ROUTER_BIND:$ZENOH_PORT (restart=$RESTART)"
    docker run -d --name "$ROUTER_NAME" --network=host --restart "$RESTART" \
        "$ROUTER_IMAGE" -l "tcp/$ROUTER_BIND:$ZENOH_PORT" --no-multicast-scouting >/dev/null
    echo ">> agent '$AGENT_NAME' (BC dials tcp:$TCP_PORT -> router :$ZENOH_PORT, key $RPC_KEY)"
    docker run -d --name "$AGENT_NAME" --network=host --restart "$RESTART" \
        -e "ZENOH_LOCATOR=tcp/127.0.0.1:$ZENOH_PORT" -e "TCP_PORT=$TCP_PORT" -e "RPC_KEY=$RPC_KEY" \
        "$AGENT_IMAGE" >/dev/null
    sleep 3
    echo "=== status ==="; docker ps --filter "name=$ROUTER_NAME" --filter "name=$AGENT_NAME" --format "$ps_fmt"
    echo "=== agent log ==="; docker logs "$AGENT_NAME" 2>&1 | tail -6
}
case "${1:-up}" in
    up)     up ;;
    down)   down ;;
    build)  build ;;
    status) docker ps -a --filter "name=$ROUTER_NAME" --filter "name=$AGENT_NAME" --format "$ps_fmt" ;;
    logs)   docker logs -f "${2:-$AGENT_NAME}" ;;
    *)      echo "usage: $0 [up|down|status|logs [name]|build]"; exit 1 ;;
esac
