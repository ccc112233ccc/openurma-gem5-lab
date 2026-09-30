#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab_dir="$(cd "$script_dir/.." && pwd)"
# shellcheck source=runtime.sh
source "$script_dir/runtime.sh"
container="$UBSIM_CONTAINER"
artifact_rel=artifacts/mooncake-urma
binary="$lab_dir/$artifact_rel/bin/transfer_engine_bench"
library_dir="$lab_dir/$artifact_rel/lib"

[[ -x "$binary" ]] || {
    printf 'error: %s is missing; run scripts/build-mooncake-urma.sh first\n' "$binary" >&2
    exit 2
}

ubsim_runtime_start
runtime_lab="${UBSIM_LAB_ROOT:-$(ubsim_runtime_default_lab "$lab_dir")}"
if [[ "$UBSIM_EXECUTION_MODE" == docker ]]; then
    docker exec \
        -e EXTRA_BINS="$runtime_lab/$artifact_rel/bin/transfer_engine_bench" \
        -e EXTRA_LIBRARY_DIRS="$runtime_lab/$artifact_rel/lib" \
        "$container" bash -lc "cd '$runtime_lab' && ./official-udma/build_initramfs.sh"
else
    EXTRA_BINS="$runtime_lab/$artifact_rel/bin/transfer_engine_bench" \
    EXTRA_LIBRARY_DIRS="$runtime_lab/$artifact_rel/lib" \
    UBSIM_LAB_ROOT="$runtime_lab" \
        "$runtime_lab/official-udma/build_initramfs.sh"
fi

printf '[mooncake-urma] packaged transfer_engine_bench into %s\n' \
    "$lab_dir/out/official-udma.cpio.gz"
