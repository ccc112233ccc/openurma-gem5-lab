#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab="$(cd "$script_dir/../.." && pwd)"
run_dir="${UBSIM_QEMU_DUAL_OUT:-$lab/run-qemu-dual}"

stop_one() {
    local label=$1 file=$2 pid command
    [[ -r "$file" ]] || { echo "$label: no pid file"; return; }
    pid=$(sed -n '1p' "$file")
    [[ "$pid" =~ ^[0-9]+$ ]] || { echo "$label: invalid pid file"; return; }
    kill -0 "$pid" 2>/dev/null || { echo "$label: already stopped"; return; }
    command=$(ps -p "$pid" -o command= 2>/dev/null || true)
    case "$command" in
        *qemu-system-aarch64*|*udma-device-sim*|*ub-net-adapter*) ;;
        *) echo "$label: refusing to signal unexpected/reused pid $pid" >&2; return 3 ;;
    esac
    kill -TERM "$pid"
    echo "$label: sent TERM to pid $pid"
}

stop_one node0 "$run_dir/node0/qemu.pid"
stop_one node1 "$run_dir/node1/qemu.pid"
stop_one ns3-fabric "$run_dir/ub-fabric/ns3.pid"
stop_one udma0 "$run_dir/udma-node0/udma.pid"
stop_one udma1 "$run_dir/udma-node1/udma.pid"
rm -f /tmp/ubsim-qemu-dual.node{0,1}.{host.sock,net.sock,shm}
