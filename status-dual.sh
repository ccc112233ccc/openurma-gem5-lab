#!/usr/bin/env bash
set -euo pipefail

container="${OPENURMA_CONTAINER:-openurma-gem5-lab}"
lab="${OPENURMA_LAB_ROOT:-/workspace/openurma-gem5-lab}"
run_root="${OPENURMA_DUAL_OUT:-$lab/run-dual}"
uart0="${OPENURMA_DUAL_UART0:-3460}"
uart1="${OPENURMA_DUAL_UART1:-3470}"

if ! docker inspect "$container" >/dev/null 2>&1; then
    echo "Container does not exist: $container" >&2
    exit 2
fi
if [ "$(docker inspect -f '{{.State.Running}}' "$container")" != true ]; then
    echo "Container is stopped: $container"
    exit 1
fi

show_process() {
    label=$1
    pidfile=$2
    if ! docker exec "$container" test -r "$pidfile"; then
        printf '%-8s %s\n' "$label" "not started"
        return
    fi
    pid=$(docker exec "$container" sed -n '1p' "$pidfile")
    if [[ "$pid" =~ ^[0-9]+$ ]] && docker exec "$container" kill -0 "$pid" 2>/dev/null; then
        printf '%-8s running (container pid %s)\n' "$label" "$pid"
    else
        printf '%-8s stopped (stale pid %s)\n' "$label" "$pid"
    fi
}

node_count="$(docker exec "$container" awk -F= \
    '$1 == "node_count" { print $2; exit }' \
    "$run_root/run-manifest.txt" 2>/dev/null || true)"
node_count=${node_count:-2}
for ((node = 0; node < node_count; ++node)); do
    show_process "node$node" "$run_root/node$node/gem5.pid"
done
show_process switch "$run_root/switch/gem5.pid"
show_process ub-switch "$run_root/ub-switch/gem5.pid"
if docker exec "$container" test -r "$run_root/oob-switch/relay.pid"; then
    show_process oob-switch "$run_root/oob-switch/relay.pid"
elif docker exec "$container" test -r "$run_root/relay/relay.pid"; then
    show_process relay "$run_root/relay/relay.pid"
else
    for ((pair = 0; pair < node_count / 2; ++pair)); do
        show_process "relay$pair" "$run_root/relay$pair/relay.pid"
    done
fi

for ((node = 0; node < node_count; ++node)); do
    transcript="$run_root/node$node/system.terminal"
    if docker exec "$container" test -r "$transcript"; then
        if docker exec "$container" grep -aq 'OpenURMA Tier-G interactive guest' "$transcript"; then
            echo "node$node   guest shell ready"
        else
            echo "node$node   booting (see $transcript)"
        fi
    fi
done

uart_stride=$((uart1 - uart0))
for ((node = 0; node < node_count; ++node)); do
    echo "node$node UART: localhost:$((uart0 + node * uart_stride))"
done
echo "Logs:  $run_root/nodeN/gem5.log, system.terminal, switch/gem5.log, ub-switch/gem5.log, and oob-switch/relay.log"
