#!/usr/bin/env bash
set -euo pipefail

container="${OPENURMA_CONTAINER:-openurma-gem5-lab}"
lab="${OPENURMA_LAB_ROOT:-/workspace/openurma-gem5-lab}"
run_root="${OPENURMA_DUAL_OUT:-$lab/run-dual}"

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

stop_one node0 "$run_root/node0/gem5.pid" "$run_root/node0"
stop_one node1 "$run_root/node1/gem5.pid" "$run_root/node1"
stop_one switch "$run_root/switch/gem5.pid" "$run_root/switch"
stop_one relay "$run_root/relay/relay.pid" "$lab/tools/ethernet_relay.py"
echo "The container and any separate single-node gem5 session were left untouched."
