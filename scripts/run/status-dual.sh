#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/runtime.sh
source "$script_dir/../runtime.sh"
container="$OPENURMA_CONTAINER"
repo_root="$(cd "$script_dir/../.." && pwd)"
lab="${OPENURMA_LAB_ROOT:-$(ou_runtime_default_lab "$repo_root")}"
run_root="${OPENURMA_DUAL_OUT:-$lab/run-dual}"
uart0="${OPENURMA_DUAL_UART0:-3460}"
uart1="${OPENURMA_DUAL_UART1:-3470}"

if [[ "$OPENURMA_EXECUTION_MODE" == docker ]]; then
    if ! docker inspect "$container" >/dev/null 2>&1; then
        echo "Container does not exist: $container" >&2
        exit 2
    fi
    if [ "$(docker inspect -f '{{.State.Running}}' "$container")" != true ]; then
        echo "Container is stopped: $container"
        exit 1
    fi
else
    ou_runtime_validate
fi

show_process() {
    label=$1
    pidfile=$2
    if ! ou_exec test -r "$pidfile"; then
        printf '%-8s %s\n' "$label" "not started"
        return
    fi
    pid=$(ou_exec sed -n '1p' "$pidfile")
    if [[ "$pid" =~ ^[0-9]+$ ]] && ou_exec kill -0 "$pid" 2>/dev/null; then
        printf '%-8s running (%s pid %s)\n' "$label" "$OPENURMA_EXECUTION_MODE" "$pid"
    else
        printf '%-8s stopped (stale pid %s)\n' "$label" "$pid"
    fi
}

node_count="$(ou_exec awk -F= \
    '$1 == "node_count" { print $2; exit }' \
    "$run_root/run-manifest.txt" 2>/dev/null || true)"
node_count=${node_count:-2}
for ((node = 0; node < node_count; ++node)); do
    show_process "node$node" "$run_root/node$node/gem5.pid"
done
show_process switch "$run_root/switch/gem5.pid"
show_process ub-switch "$run_root/ub-switch/gem5.pid"
if ou_exec test -r "$run_root/oob-switch/relay.pid"; then
    show_process oob-switch "$run_root/oob-switch/relay.pid"
elif ou_exec test -r "$run_root/relay/relay.pid"; then
    show_process relay "$run_root/relay/relay.pid"
else
    for ((pair = 0; pair < node_count / 2; ++pair)); do
        show_process "relay$pair" "$run_root/relay$pair/relay.pid"
    done
fi

for ((node = 0; node < node_count; ++node)); do
    transcript="$run_root/node$node/system.terminal"
    if ou_exec test -r "$transcript"; then
        if ou_exec grep -aq 'OpenURMA Tier-G interactive guest' "$transcript"; then
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
