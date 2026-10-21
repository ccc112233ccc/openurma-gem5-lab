#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab="$(cd "$script_dir/../.." && pwd)"
run_dir="${UBSIM_QEMU_DUAL_OUT:-$lab/run-qemu-dual}"
node_count=$(awk -F= '$1 == "node_count" {print $2; exit}' "$run_dir/run-manifest.txt" 2>/dev/null || true)
[[ "$node_count" =~ ^[2-8]$ ]] || node_count=2

stop_one() {
    local label=$1 file=$2 pid command
    [[ -r "$file" ]] || { echo "$label: no pid file"; return; }
    pid=$(sed -n '1p' "$file")
    [[ "$pid" =~ ^[0-9]+$ ]] || { echo "$label: invalid pid file"; return; }
    kill -0 "$pid" 2>/dev/null || { echo "$label: already stopped"; return; }
    command=$(ps -p "$pid" -o command= 2>/dev/null || true)
    case "$command" in
        *qemu-system-aarch64*|*udma-device-sim*|*ub-net-adapter*|*ethernet_relay.py*) ;;
        *) echo "$label: refusing to signal unexpected/reused pid $pid" >&2; return 3 ;;
    esac
    kill -TERM "$pid"
    echo "$label: sent TERM to pid $pid"
}

for ((node = 0; node < node_count; ++node)); do
    stop_one "node$node" "$run_dir/node$node/qemu.pid"
done
stop_one ns3-fabric "$run_dir/ub-fabric/ns3.pid"
stop_one oob-switch "$run_dir/oob-switch/relay.pid"
for ((node = 0; node < node_count; ++node)); do
    stop_one "udma$node" "$run_dir/udma-node$node/udma.pid"
    rm -f "/tmp/ubsim-qemu-dual.node$node.host.sock" \
          "/tmp/ubsim-qemu-dual.node$node.net.sock" \
          "/tmp/ubsim-qemu-dual.node$node.shm"
done
