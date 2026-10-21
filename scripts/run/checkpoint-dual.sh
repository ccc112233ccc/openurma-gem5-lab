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
timeout="${UBSIM_CHECKPOINT_TIMEOUT:-1800}"
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
mem_size=$(ubsim_exec awk -F= '$1 == "memory_size" {print $2; exit}' \
    "$run_root/run-manifest.txt")
ubsim_exec python3 "$lab/tools/host_memory_guard.py" check \
    --phase checkpoint --nodes "$node_count" --mem-size "$mem_size" ||
    die "host memory preflight failed; no checkpoint was started"
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
for ((node = 0; node < node_count; ++node)); do
    if ubsim_exec test -e "$run_root/node$node/cpt/m5.cpt"; then
        die "node$node has an interrupted checkpoint; restart the run before retrying"
    fi
done
echo "Stopping $node_count guests at a coordinated shell-ready checkpoint..."
ubsim_exec python3 "$lab/tools/coordinated_checkpoint.py" \
    --ports "${ports[@]}" --timeout "$timeout"

# gem5 deliberately stops immediately after writing its architectural state.
# The standalone UDMA process has no reason to exit merely because its host
# transport disconnected, so stop it only after every guest checkpoint is
# durable.  Its normal signal path drains the main loop and serializes the
# quiescent model state configured by --state-out.
for _ in $(seq 1 "$timeout"); do
    complete=1
    for ((node = 0; node < node_count; ++node)); do
        ubsim_exec test -s "$run_root/node$node/cpt/m5.cpt" || complete=0
        pmem_count=$(ubsim_exec find "$run_root/node$node/cpt" \
            -maxdepth 1 -name '*.pmem' -type f -size +0c 2>/dev/null | wc -l) ||
            pmem_count=0
        (( pmem_count > 0 )) || complete=0
        pid=$(ubsim_exec cat "$run_root/node$node/gem5.pid")
        if ubsim_exec kill -0 "$pid" 2>/dev/null; then complete=0; fi
    done
    (( complete )) && break
    sleep 1
done
(( complete )) || die "gem5 checkpoint files did not become ready"

# gem5's std::vector<bool> iterator exposes a proxy. On this build its
# ShowParam path writes flash lock flags as 0/1, while ParseParam<bool> accepts
# only true/false. Normalize that field before publishing a checkpoint.
for ((node = 0; node < node_count; ++node)); do
    ubsim_exec python3 "$lab/tools/normalize_gem5_checkpoint.py" \
        "$run_root/node$node/cpt/m5.cpt"
done

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
initrd=$(ubsim_exec awk -F= '$1 == "initrd_path" {print $2; exit}' \
    "$run_root/run-manifest.txt")
initrd=${initrd:-$lab/out/official-udma-64k-20261015.cpio.gz}
initrd_hash=$(ubsim_exec sha256sum "$initrd" | awk '{print $1}')
[[ "$initrd_hash" =~ ^[0-9a-f]{64}$ ]] || die "cannot hash active initramfs: $initrd"
ubsim_exec sh -c 'printf "created_utc=%s\ncheckpoint_wall_seconds=%s\narchitected_timer_verified=1\ninitramfs_sha256=%s\n" "$1" "$2" "$3" > "$4/checkpoint-manifest.txt"' \
    _ "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$elapsed" "$initrd_hash" "$temporary"
ubsim_exec mv "$temporary" "$destination"
bash "$script_dir/stop-dual.sh" >/dev/null || true
echo "Checkpoint ready: checkpoints/$name (wall ${elapsed}s)"
echo "Restore with: ./lab start --restore-checkpoint $name"
