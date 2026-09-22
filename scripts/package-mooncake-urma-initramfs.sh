#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab_dir="$(cd "$script_dir/.." && pwd)"
container="${OPENURMA_CONTAINER:-openurma-gem5-lab}"
artifact_rel=artifacts/mooncake-urma
binary="$lab_dir/$artifact_rel/bin/transfer_engine_bench"
library_dir="$lab_dir/$artifact_rel/lib"

[[ -x "$binary" ]] || {
    printf 'error: %s is missing; run scripts/build-mooncake-urma.sh first\n' "$binary" >&2
    exit 2
}

docker start "$container" >/dev/null
docker exec \
    -e EXTRA_BINS="/workspace/openurma-gem5-lab/$artifact_rel/bin/transfer_engine_bench" \
    -e EXTRA_LIBRARY_DIRS="/workspace/openurma-gem5-lab/$artifact_rel/lib" \
    "$container" bash -lc '
set -euo pipefail
cd /workspace/openurma-gem5-lab
./official-udma/build_initramfs.sh
'

printf '[mooncake-urma] packaged transfer_engine_bench into %s\n' \
    "$lab_dir/out/official-udma.cpio.gz"
