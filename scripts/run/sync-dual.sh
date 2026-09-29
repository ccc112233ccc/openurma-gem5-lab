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
sync_timeout="${OPENURMA_SYNC_TIMEOUT:-1800}"
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

restore_checkpoint="$(ou_exec awk -F= \
    '$1 == "restore_checkpoint" { print $2; exit }' \
    "$run_root/run-manifest.txt" 2>/dev/null || true)"
timer_verified_by_checkpoint=0
if [[ -n "$restore_checkpoint" && "$restore_checkpoint" != none ]] && \
        ou_exec grep -qx 'architected_timer_verified=1' \
        "$lab/checkpoints/$restore_checkpoint/checkpoint-manifest.txt"; then
    timer_verified_by_checkpoint=1
fi

if ou_exec test -e "$marker"; then
    echo "$node_count-node measurement setup is already marked ready."
    exit 0
fi

echo "Configuring all guests on the shared OOB control network..."
echo "All UARTs must be detached; use ~. at the start of a line first."

# gem5 drops PL011 output while no terminal client is attached, so a cold boot
# cannot be validated by polling system.terminal for an early arch_timer line.
# Wait for the shell through the UART and query the retained kernel ring buffer
# instead.  This validates the actual running guest and works whether sync is
# invoked immediately after start or after the shell has become idle.
sync_command='ip link set eth0 up && /usr/local/bin/ou-net-up'
if (( ! timer_verified_by_checkpoint )); then
    sync_command="dmesg | grep -E 'arch_timer: .*timer\\(s\\) running at [0-9.]+MHz' && ! dmesg | grep -E 'sched_clock: .* at 250 Hz|arch_timer: Unable to find' && $sync_command"
fi
ou_exec python3 "$serial_tool" \
    --ports "${uart_ports[@]}" \
    --command "$sync_command" \
    --timeout "$sync_timeout" --prompt-kick-after 1

for ((node = 0; node < node_count; ++node)); do
    if (( timer_verified_by_checkpoint )); then
        echo "node$node architected timer: OK (checkpoint)"
    else
        echo "node$node architected timer: OK (guest dmesg)"
    fi
done

cpu_mode="$(ou_exec awk -F= \
    '$1 == "cpu_mode" { print $2; exit }' "$run_root/run-manifest.txt" 2>/dev/null || true)"
ou_exec sh -c 'printf "architected_timer_verified=1\noob_ready=1\n" > "$1"' _ "$marker"
echo "All unique OOB IPv4 addresses are active."
if [[ "$cpu_mode" == server_o3 ]]; then
    echo "The guests remain on their fast boot CPUs while idle."
    echo "Each synchronized send_lat run switches to ArmO3 only for its warm-up and measured loop, then switches back."
fi
echo "The ou-lat-* wrappers will enter and leave conservative synchronization collectively around the measured loop."
