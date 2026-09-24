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
sync_timeout="${OPENURMA_SYNC_TIMEOUT:-600}"
serial_tool="${OPENURMA_DUAL_SERIAL_TOOL:-$lab/tools/dual_serial_command.py}"
marker="$run_root/sync.ready"

die() {
    echo "sync-dual.sh: $*" >&2
    exit 2
}

case "$sync_timeout" in
    ''|*[!0-9]*) die "OPENURMA_SYNC_TIMEOUT must be a positive integer" ;;
esac
(( sync_timeout > 0 )) || die "OPENURMA_SYNC_TIMEOUT must be positive"

ou_runtime_start
node_count="$(ou_exec awk -F= \
    '$1 == "node_count" { print $2; exit }' \
    "$run_root/run-manifest.txt" 2>/dev/null || true)"
node_count=${node_count:-2}
[[ "$node_count" =~ ^[0-9]+$ ]] && (( node_count >= 2 )) ||
    die "run manifest has an invalid node_count"
uart_stride=$((uart1 - uart0))
(( uart_stride > 0 )) || die "node1 UART must exceed node0 UART"
uart_ports=()
for ((node = 0; node < node_count; ++node)); do
    uart_ports+=("$((uart0 + node * uart_stride))")
done

# A usable architected timer is part of the experiment contract.  Without it,
# OLK falls back to a 250 Hz sched_clock and the 4 ms quantization dominates the
# reported tail even though distributed causality is still correct.
for ((node = 0; node < node_count; ++node)); do
    node_name="node$node"
    terminal="$run_root/$node_name/system.terminal"
    timer_ready=0
    for _ in $(seq 1 "$((sync_timeout / 2 + 1))"); do
        if ou_exec test -f "$terminal" && \
                ou_exec grep -Eq \
                'arch_timer: .*timer\(s\) running at [0-9.]+MHz' "$terminal"; then
            timer_ready=1
            break
        fi
        sleep 2
    done
    (( timer_ready )) || die "$node_name did not expose a working architected timer within ${sync_timeout}s"
    if ! ou_exec grep -Eq \
        'arch_timer: .*timer\(s\) running at [0-9.]+MHz' "$terminal"; then
        die "$node_name has no working architected timer; restart the run"
    fi
    if ou_exec grep -Eq \
        'sched_clock: .* at 250 Hz|arch_timer: Unable to find' "$terminal"; then
        die "$node_name fell back to the 250 Hz clock; this run is invalid"
    fi
    echo "$node_name architected timer: OK"
done

if ou_exec test -e "$marker"; then
    echo "$node_count-node measurement setup is already marked ready."
    exit 0
fi

echo "Configuring all guests on the shared OOB control network..."
echo "All UARTs must be detached; use ~. at the start of a line first."
ou_exec python3 "$serial_tool" \
    --ports "${uart_ports[@]}" --command /usr/local/bin/ou-net-up \
    --timeout "$sync_timeout" --prompt-kick-after 1

cpu_mode="$(ou_exec awk -F= \
    '$1 == "cpu_mode" { print $2; exit }' "$run_root/run-manifest.txt" 2>/dev/null || true)"
ou_exec touch "$marker"
echo "All unique OOB IPv4 addresses are active."
if [[ "$cpu_mode" == server_o3 ]]; then
    echo "The guests remain on their fast boot CPUs while idle."
    echo "Each synchronized send_lat run switches to ArmO3 only for its warm-up and measured loop, then switches back."
fi
echo "The ou-lat-* wrappers will enter and leave conservative synchronization collectively around the measured loop."
