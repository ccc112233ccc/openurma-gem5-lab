#!/usr/bin/env bash
set -euo pipefail

container="${OPENURMA_CONTAINER:-openurma-repro-20260909}"
lab="${OPENURMA_LAB_ROOT:-/workspace/openurma-gem5-lab}"
run_root="${OPENURMA_DUAL_OUT:-$lab/run-dual}"
uart0="${OPENURMA_DUAL_UART0:-3460}"
uart1="${OPENURMA_DUAL_UART1:-3470}"
marker="$run_root/sync.ready"

die() {
    echo "sync-dual.sh: $*" >&2
    exit 2
}

# A usable architected timer is part of the experiment contract.  Without it,
# OLK falls back to a 250 Hz sched_clock and the 4 ms quantization dominates the
# reported tail even though distributed causality is still correct.
for node in node0 node1; do
    terminal="$run_root/$node/system.terminal"
    docker exec "$container" test -f "$terminal" ||
        die "$node terminal log is missing; wait for the guest to boot"
    if ! docker exec "$container" grep -Eq \
        'arch_timer: .*timer\(s\) running at [0-9.]+MHz' "$terminal"; then
        die "$node has no working architected timer; restart the dual run"
    fi
    if docker exec "$container" grep -Eq \
        'sched_clock: .* at 250 Hz|arch_timer: Unable to find' "$terminal"; then
        die "$node fell back to the 250 Hz clock; this run is invalid"
    fi
    echo "$node architected timer: OK"
done

if docker exec "$container" test -e "$marker"; then
    echo "Dual-node measurement setup is already marked ready."
    exit 0
fi

echo "Configuring both guests' host-relayed OOB control interfaces..."
echo "Both UARTs must be detached; use ~. at the start of a line first."
docker exec "$container" python3 "$lab/tools/dual_serial_command.py" \
    --ports "$uart0" "$uart1" --command /usr/local/bin/ou-net-up --timeout 60

cpu_mode="$(docker exec "$container" awk -F= \
    '$1 == "cpu_mode" { print $2; exit }' "$run_root/run-manifest.txt" 2>/dev/null || true)"
docker exec "$container" touch "$marker"
echo "Both OOB IPv4 addresses are active."
if [[ "$cpu_mode" == server_o3 ]]; then
    echo "The guests remain on their fast boot CPUs while idle."
    echo "Each synchronized send_lat run switches to ArmO3 only for its warm-up and measured loop, then switches back."
fi
echo "The ou-lat-* wrappers will enter and leave conservative synchronization collectively around the measured loop."
