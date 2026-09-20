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

show_process node0 "$run_root/node0/gem5.pid"
show_process node1 "$run_root/node1/gem5.pid"
show_process switch "$run_root/switch/gem5.pid"
show_process relay "$run_root/relay/relay.pid"

for node in 0 1; do
    transcript="$run_root/node$node/system.terminal"
    if docker exec "$container" test -r "$transcript"; then
        if docker exec "$container" grep -aq 'OpenURMA Tier-G interactive guest' "$transcript"; then
            echo "node$node   guest shell ready"
        else
            echo "node$node   booting (see $transcript)"
        fi
    fi
done

echo "UARTs: node0 localhost:$uart0, node1 localhost:$uart1"
echo "Logs:  $run_root/node{0,1}/gem5.log, system.terminal, switch/gem5.log, and relay/relay.log"
