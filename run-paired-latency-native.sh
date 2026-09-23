#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export OPENURMA_EXECUTION_MODE=native
export OPENURMA_LAB_ROOT="${OPENURMA_LAB_ROOT:-$script_dir}"
exec bash "$script_dir/run-paired-latency.sh" "$@"
