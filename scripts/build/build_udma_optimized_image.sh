#!/usr/bin/env bash
set -euo pipefail

lab_dir="${UBSIM_LAB_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "$lab_dir"
python3 tools/build_udma_optimized.py
export STOCK_UDMA_PROVIDER="$lab_dir/artifacts/udma-optimized-provider/liburma-udma.so"
export UDMA_PROVIDER_VARIANT=optimized
export OUT="$lab_dir/out/optimized-udma.cpio.gz"
"$lab_dir/official-udma/build_initramfs.sh"
