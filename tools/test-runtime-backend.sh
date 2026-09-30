#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab_dir="$(cd "$script_dir/.." && pwd)"
# shellcheck source=../scripts/runtime.sh
source "$lab_dir/scripts/runtime.sh"

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/ubsim-runtime.XXXXXX")"
trap 'rm -rf -- "$tmp_dir"' EXIT

ubsim_exec test -f "$lab_dir/scripts/runtime.sh"
printf 'runtime-stdin\n' | ubsim_exec_i grep -qx runtime-stdin
ubsim_exec_detached_env \
    UBSIM_RUNTIME_SENTINEL=runtime-env \
    -- \
    bash "$lab_dir/tools/run-background.sh" \
    "$tmp_dir/test.pid" "$tmp_dir/test.log" \
    bash -c 'printf "%s\n" "$UBSIM_RUNTIME_SENTINEL"'

for _ in $(seq 1 50); do
    [[ -s "$tmp_dir/test.log" ]] && break
    sleep 0.1
done
grep -qx runtime-env "$tmp_dir/test.log"
grep -Eq '^[0-9]+$' "$tmp_dir/test.pid"
printf 'runtime backend smoke passed: %s\n' "$UBSIM_EXECUTION_MODE"
