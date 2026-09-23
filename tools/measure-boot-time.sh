#!/usr/bin/env bash
# Measure wall time until the BusyBox shell prompt appears in system.terminal.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
lab=$(cd "$script_dir/.." && pwd)
mode=${1:-kvm}
timeout_seconds=${2:-600}
run_dir=${OPENURMA_BOOT_RUN_DIR:-$lab/run-boottime-$mode}
launch_log=${run_dir}.launch.log
[[ "$timeout_seconds" =~ ^[1-9][0-9]*$ ]] || {
    echo "measure-boot-time.sh: timeout must be a positive integer" >&2
    exit 2
}
[[ ! -e "$run_dir" ]] || {
    echo "measure-boot-time.sh: remove or rename existing output first: $run_dir" >&2
    exit 2
}
mkdir -p "$(dirname "$launch_log")"

start=$(date +%s)
"$script_dir/kvm-boot-test.sh" "$mode" "$run_dir" >"$launch_log" 2>&1 &
launcher=$!
cleanup() {
    kill "$launcher" 2>/dev/null || true
    wait "$launcher" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

while kill -0 "$launcher" 2>/dev/null; do
    now=$(date +%s)
    if [[ -f "$run_dir/system.terminal" ]] &&
       grep -Eq '\(openurma-(gem5|node0)\).*#' "$run_dir/system.terminal"; then
        printf 'boot_to_shell_seconds=%d cpu_mode=%s output=%s\n' \
            "$((now - start))" "$mode" "$run_dir"
        exit 0
    fi
    (( now - start < timeout_seconds )) || {
        echo "measure-boot-time.sh: timed out after ${timeout_seconds}s; see $launch_log" >&2
        exit 1
    }
    sleep 1
done
wait "$launcher"
echo "measure-boot-time.sh: gem5 exited before the shell prompt; see $launch_log" >&2
exit 1
