#!/usr/bin/env bash
set -euo pipefail

lab_dir="${UBSIM_LAB_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "$lab_dir"
python3 tools/build_udma_optimized.py
