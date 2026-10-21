#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab="$(cd "$script_dir/../.." && pwd)"
run_dir="${UBSIM_QEMU_DUAL_OUT:-$lab/run-qemu-dual}"
node_count=$(awk -F= '$1 == "node_count" {print $2; exit}' "$run_dir/run-manifest.txt" 2>/dev/null || true)
[[ "$node_count" =~ ^[2-8]$ ]] || node_count=2

pid_live() {
    local file=$1 pid
    [[ -r "$file" ]] || return 1
    pid=$(sed -n '1p' "$file")
    [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null
}

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

for ((node = 0; node < node_count; ++node)); do
    show_process "node$node" "$run_dir/node$node/qemu.pid"
done
for ((node = 0; node < node_count; ++node)); do
    show_process "udma$node" "$run_dir/udma-node$node/udma.pid"
done
show_process ns3-fabric "$run_dir/ub-fabric/ns3.pid"
if (( node_count > 2 )); then show_process oob-switch "$run_dir/oob-switch/relay.pid"; fi
for ((node = 0; node < node_count; ++node)); do
    terminal="$run_dir/node$node/system.terminal"
    pidfile="$run_dir/node$node/qemu.pid"
    if pid_live "$pidfile" && [[ -r "$terminal" ]] && grep -aq 'Official UDMA full-system guest' "$terminal"; then
        echo "node$node      guest shell ready"
    elif pid_live "$pidfile" && [[ -r "$terminal" ]]; then
        echo "node$node      booting (see $terminal)"
    fi
done
uart0=$(awk -F= '$1 == "uart0" {print $2; exit}' "$run_dir/run-manifest.txt" 2>/dev/null || true)
uart1=$(awk -F= '$1 == "uart1" {print $2; exit}' "$run_dir/run-manifest.txt" 2>/dev/null || true)
ssh0=$(awk -F= '$1 == "ssh0_port" {print $2; exit}' "$run_dir/run-manifest.txt" 2>/dev/null || true)
ssh1=$(awk -F= '$1 == "ssh1_port" {print $2; exit}' "$run_dir/run-manifest.txt" 2>/dev/null || true)
uart0=${uart0:-3560}; uart1=${uart1:-3570}; ssh0=${ssh0:-2220}; ssh1=${ssh1:-2221}
for ((node = 0; node < node_count; ++node)); do
    echo "node$node UART: localhost:$((uart0 + node * (uart1 - uart0))), SSH: localhost:$((ssh0 + node * (ssh1 - ssh0)))"
done
echo "Logs:  $run_dir"
