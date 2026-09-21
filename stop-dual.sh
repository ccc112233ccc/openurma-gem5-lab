#!/usr/bin/env bash
set -euo pipefail

container="${OPENURMA_CONTAINER:-openurma-gem5-lab}"
lab="${OPENURMA_LAB_ROOT:-/workspace/openurma-gem5-lab}"
run_root="${OPENURMA_DUAL_OUT:-$lab/run-dual}"
ub_switch_binary="${OPENURMA_UB_SWITCH_BINARY:-$lab/out/ub-switch-sim}"

if ! docker inspect "$container" >/dev/null 2>&1; then
    echo "Container does not exist: $container" >&2
    exit 2
fi
if [ "$(docker inspect -f '{{.State.Running}}' "$container")" != true ]; then
    echo "Dual-node processes are already stopped with container $container."
    exit 0
fi

stop_one() {
    label=$1
    pidfile=$2
    expected=$3
    docker exec "$container" bash -c '
        label=$1
        pidfile=$2
        expected=$3
        test -r "$pidfile" || { echo "$label: no pid file"; exit 0; }
        pid=$(sed -n "1p" "$pidfile")
        case "$pid" in ""|*[!0-9]*) echo "$label: invalid pid file"; exit 0;; esac
        kill -0 "$pid" 2>/dev/null || { echo "$label: already stopped"; exit 0; }
        cmd=$(tr "\000" " " < "/proc/$pid/cmdline")
        printf "%s" "$cmd" | grep -Fq -- "$expected" || {
            echo "$label: refusing to signal reused/unexpected pid $pid" >&2
            exit 3
        }
        kill -TERM "$pid"
        echo "$label: sent TERM to container pid $pid"
    ' _ "$label" "$pidfile" "$expected"
}

node_count="$(docker exec "$container" awk -F= \
    '$1 == "node_count" { print $2; exit }' \
    "$run_root/run-manifest.txt" 2>/dev/null || true)"
node_count=${node_count:-2}
for ((node = 0; node < node_count; ++node)); do
    stop_one "node$node" "$run_root/node$node/gem5.pid" "$run_root/node$node"
done
stop_one switch "$run_root/switch/gem5.pid" "$run_root/switch"
stop_one ub-switch "$run_root/ub-switch/gem5.pid" "$ub_switch_binary"
if docker exec "$container" test -r "$run_root/oob-switch/relay.pid"; then
    stop_one oob-switch "$run_root/oob-switch/relay.pid" \
        "$lab/tools/ethernet_relay.py"
elif docker exec "$container" test -r "$run_root/relay/relay.pid"; then
    stop_one relay "$run_root/relay/relay.pid" "$lab/tools/ethernet_relay.py"
else
    for ((pair = 0; pair < node_count / 2; ++pair)); do
        stop_one "relay$pair" "$run_root/relay$pair/relay.pid" \
            "$lab/tools/ethernet_relay.py"
    done
fi
echo "The container and any separate single-node gem5 session were left untouched."
