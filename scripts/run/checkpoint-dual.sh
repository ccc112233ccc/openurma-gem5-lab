#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/runtime.sh
source "$script_dir/../runtime.sh"

die() { echo "checkpoint-dual.sh: $*" >&2; exit 2; }
(( $# == 1 )) || die "usage: ./lab checkpoint NAME"
name=$1
case "$name" in *[!A-Za-z0-9._-]*|.|..) die "invalid checkpoint name" ;; esac

repo_root="$(cd "$script_dir/../.." && pwd)"
lab="${UBSIM_LAB_ROOT:-$(ubsim_runtime_default_lab "$repo_root")}"
run_root="${UBSIM_DUAL_OUT:-$lab/run-dual}"
timeout="${UBSIM_CHECKPOINT_TIMEOUT:-600}"
destination="$lab/checkpoints/$name"
temporary="$lab/checkpoints/.$name.tmp.$$"
ubsim_runtime_start
ubsim_exec test -r "$run_root/run-manifest.txt" || die "no active run"
ubsim_exec test ! -e "$destination" || die "checkpoint already exists: $name"
node_count=$(ubsim_exec awk -F= '$1 == "node_count" {print $2; exit}' \
    "$run_root/run-manifest.txt")
backend=$(ubsim_exec awk -F= '$1 == "network_backend" {print $2; exit}' \
    "$run_root/run-manifest.txt")
[[ "$backend" == modular-ns3ub ]] || die "coordinated checkpoints require modular-ns3ub"
uart0="${UBSIM_DUAL_UART0:-3460}"
uart1="${UBSIM_DUAL_UART1:-3470}"
stride=$((uart1 - uart0))
ports=()
for ((node = 0; node < node_count; ++node)); do ports+=("$((uart0 + node * stride))"); done

started=$(date +%s)
timer_verified=0
if ubsim_exec grep -qx 'architected_timer_verified=1' "$run_root/sync.ready" 2>/dev/null; then
    timer_verified=1
else
    restore_checkpoint=$(ubsim_exec awk -F= '$1 == "restore_checkpoint" {print $2; exit}' \
        "$run_root/run-manifest.txt")
    if [[ -n "$restore_checkpoint" && "$restore_checkpoint" != none ]] && \
            ubsim_exec grep -qx 'architected_timer_verified=1' \
            "$lab/checkpoints/$restore_checkpoint/checkpoint-manifest.txt"; then
        timer_verified=1
    fi
fi
(( timer_verified )) || die "the active run has no validated architected timer"

# Validate live device state instead of depending on historical UART text.
# A restored checkpoint starts a fresh terminal log and therefore does not
# contain the original boot-time `urma_admin show` output.
ubsim_exec python3 "$lab/tools/dual_serial_command.py" \
    --ports "${ports[@]}" \
    --command "LD_LIBRARY_PATH=/lib:/usr/lib urma_admin show | grep -Eq 'udma0[[:space:]]+UB.*ACTIVE'" \
    --timeout "$timeout" --prompt-kick-after 1 >/dev/null ||
    die "one or more guests do not expose an ACTIVE udma0 device"
have_guest_checkpoints=1
for ((node = 0; node < node_count; ++node)); do
    ubsim_exec test -r "$run_root/node$node/cpt/m5.cpt" || have_guest_checkpoints=0
done
if (( have_guest_checkpoints )); then
    echo "Reusing the completed guest checkpoints from the interrupted coordination attempt..."
else
    echo "Stopping $node_count guests at a coordinated shell-ready checkpoint..."
    ubsim_exec python3 "$lab/tools/coordinated_checkpoint.py" \
        --ports "${ports[@]}" --timeout "$timeout"
fi

# gem5 deliberately stops immediately after writing its architectural state.
# The standalone UDMA process has no reason to exit merely because its host
# transport disconnected, so stop it only after every guest checkpoint is
# durable.  Its normal signal path drains the main loop and serializes the
# quiescent model state configured by --state-out.
for _ in $(seq 1 "$timeout"); do
    complete=1
    for ((node = 0; node < node_count; ++node)); do
        ubsim_exec test -r "$run_root/node$node/cpt/m5.cpt" || complete=0
    done
    (( complete )) && break
    sleep 1
done
(( complete )) || die "gem5 checkpoint files did not become ready"

for ((node = 0; node < node_count; ++node)); do
    pid=$(ubsim_exec cat "$run_root/udma-node$node/udma.pid")
    [[ "$pid" =~ ^[0-9]+$ ]] || die "invalid UDMA pid for node$node: $pid"
    ubsim_exec kill -TERM "$pid" 2>/dev/null || true
done

for _ in $(seq 1 "$timeout"); do
    complete=1
    for ((node = 0; node < node_count; ++node)); do
        ubsim_exec test -s "$run_root/udma-node$node/state.bin" || complete=0
    done
    (( complete )) && break
    sleep 1
done
(( complete )) || die "UDMA checkpoint files did not become ready"

ubsim_exec mkdir -p "$temporary"
ubsim_exec cp "$run_root/run-manifest.txt" "$temporary/run-manifest.txt"
for ((node = 0; node < node_count; ++node)); do
    ubsim_exec cp -R "$run_root/node$node/cpt" "$temporary/node$node-cpt"
    ubsim_exec cp "$run_root/udma-node$node/state.bin" "$temporary/udma-node$node.state"
done
elapsed=$(( $(date +%s) - started ))
ubsim_exec sh -c 'printf "created_utc=%s\ncheckpoint_wall_seconds=%s\narchitected_timer_verified=1\n" "$1" "$2" > "$3/checkpoint-manifest.txt"' \
    _ "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$elapsed" "$temporary"
ubsim_exec mv "$temporary" "$destination"
bash "$script_dir/stop-dual.sh" >/dev/null || true
echo "Checkpoint ready: checkpoints/$name (wall ${elapsed}s)"
echo "Restore with: ./lab start --restore-checkpoint $name"
