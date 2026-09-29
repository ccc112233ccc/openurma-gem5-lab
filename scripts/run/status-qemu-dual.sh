#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab="$(cd "$script_dir/../.." && pwd)"
run_dir="${OPENURMA_QEMU_DUAL_OUT:-$lab/run-qemu-dual}"

show_process() {
    local label=$1 file=$2 pid
    if [[ ! -r "$file" ]]; then printf '%-10s not started\n' "$label"; return; fi
    pid=$(sed -n '1p' "$file")
    if [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
        printf '%-10s running (pid %s)\n' "$label" "$pid"
    else
        printf '%-10s stopped (stale pid %s)\n' "$label" "$pid"
    fi
}

show_process node0 "$run_dir/node0/qemu.pid"
show_process node1 "$run_dir/node1/qemu.pid"
show_process udma0 "$run_dir/udma-node0/udma.pid"
show_process udma1 "$run_dir/udma-node1/udma.pid"
show_process ns3-fabric "$run_dir/ub-fabric/ns3.pid"
for node in 0 1; do
    terminal="$run_dir/node$node/system.terminal"
    if [[ -r "$terminal" ]] && grep -aq 'Official UDMA full-system guest' "$terminal"; then
        echo "node$node      guest shell ready"
    elif [[ -r "$terminal" ]]; then
        echo "node$node      booting (see $terminal)"
    fi
done
echo "UARTs: localhost:${OPENURMA_QEMU_UART0:-3560}, localhost:${OPENURMA_QEMU_UART1:-3570}"
echo "Logs:  $run_dir"
