#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$script_dir/../runtime.sh"
die() { echo "checkpoint-template.sh: $*" >&2; exit 2; }
(( $# == 1 )) || die "usage: ./lab checkpoint-template NAME"
name=$1
case "$name" in *[!A-Za-z0-9._-]*|.|..) die "invalid checkpoint name" ;; esac

repo_root="$(cd "$script_dir/../.." && pwd)"
lab="${UBSIM_LAB_ROOT:-$(ubsim_runtime_default_lab "$repo_root")}"
run_root="${UBSIM_DUAL_OUT:-$lab/run-dual}"
destination="$lab/checkpoints/$name"
temporary="$lab/checkpoints/.$name.tmp.$$"
timeout="${UBSIM_CHECKPOINT_TIMEOUT:-1800}"
ubsim_runtime_start
ubsim_exec test -r "$run_root/run-manifest.txt" || die "no active run"
ubsim_exec test ! -e "$destination" || die "checkpoint already exists: $name"
ubsim_exec grep -qx 'template_checkpoint=1' "$run_root/run-manifest.txt" ||
    die "active run was not started with --make-template"
node_count=$(ubsim_exec awk -F= '$1 == "node_count" {print $2; exit}' \
    "$run_root/run-manifest.txt")
mem_size=$(ubsim_exec awk -F= '$1 == "memory_size" {print $2; exit}' \
    "$run_root/run-manifest.txt")
ubsim_exec python3 "$lab/tools/host_memory_guard.py" check \
    --phase checkpoint --nodes "$node_count" --mem-size "$mem_size" ||
    die "host memory preflight failed"

started=$(date +%s)
for _ in $(seq 1 "$timeout"); do
    complete=1
    for ((node = 0; node < node_count; ++node)); do
        cpt="$run_root/node$node/cpt"
        ubsim_exec test -s "$cpt/m5.cpt" || complete=0
        pmem_count=$(ubsim_exec find "$cpt" -maxdepth 1 \
            -name '*.pmem' -type f -size +0c 2>/dev/null | wc -l) || pmem_count=0
        (( pmem_count > 0 )) || complete=0
        ubsim_exec grep -q 'checkpoint written' \
            "$run_root/node$node/gem5.log" 2>/dev/null || complete=0
        pid=$(ubsim_exec cat "$run_root/node$node/gem5.pid" 2>/dev/null || true)
        if [[ "$pid" =~ ^[0-9]+$ ]] && ubsim_exec kill -0 "$pid" 2>/dev/null; then
            complete=0
        fi
    done
    (( complete )) && break
    sleep 1
done
(( complete )) || die "node-neutral gem5 checkpoints did not finish"

# Every guest stopped before reading an identity or probing UDMA. One copy of
# node0's machine state is sufficient; each restore creates a fresh UDMA model.
ubsim_exec python3 "$lab/tools/normalize_gem5_checkpoint.py" \
    "$run_root/node0/cpt/m5.cpt"
ubsim_exec mkdir -p "$temporary"
ubsim_exec cp "$run_root/run-manifest.txt" "$temporary/run-manifest.txt"
ubsim_exec cp -R "$run_root/node0/cpt" "$temporary/template-cpt"
initrd=$(ubsim_exec awk -F= '$1 == "initrd_path" {print $2; exit}' \
    "$run_root/run-manifest.txt")
initrd_hash=$(ubsim_exec sha256sum "$initrd" | awk '{print $1}')
[[ "$initrd_hash" =~ ^[0-9a-f]{64}$ ]] || die "cannot hash guest image: $initrd"
elapsed=$(( $(date +%s) - started ))
ubsim_exec sh -c 'printf "checkpoint_kind=node-template-v1\ncreated_utc=%s\ncheckpoint_wall_seconds=%s\ninitramfs_sha256=%s\n" "$1" "$2" "$3" > "$4/checkpoint-manifest.txt"' \
    _ "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$elapsed" "$initrd_hash" "$temporary"
ubsim_exec mv "$temporary" "$destination"
bash "$script_dir/stop-dual.sh" >/dev/null || true
echo "Node template ready: checkpoints/$name (wall ${elapsed}s)"
echo "Restore any supported node count with: ./lab start-ready --profile modern --mem-size $mem_size --nodes N"
