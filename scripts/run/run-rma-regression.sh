#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/runtime.sh
source "$script_dir/../runtime.sh"
repo_root="$(cd "$script_dir/../.." && pwd)"
lab="${OPENURMA_LAB_ROOT:-$(ou_runtime_default_lab "$repo_root")}"
run_root="${OPENURMA_DUAL_OUT:-$lab/run-dual}"
suite=full
if [[ "${1:-}" == --sync-smoke ]]; then
    suite=sync-smoke
    shift
fi
output="${1:-$lab/experiments/rma-regression-$suite-$(date -u +%Y%m%dT%H%M%SZ)}"
ou_runtime_start
ou_exec test -e "$run_root/sync.ready" || {
    echo "run './lab sync' after both shells are ready" >&2; exit 2;
}
ou_exec python3 "$lab/tools/run_modular_rma_regression.py" \
    --lab "$lab" --run-root "$run_root" --output "$output" \
    --uart0 "${OPENURMA_DUAL_UART0:-3460}" \
    --uart1 "${OPENURMA_DUAL_UART1:-3470}" \
    --timeout "${OPENURMA_RMA_TIMEOUT:-1800}" --suite "$suite"
echo "RMA regression results: $output"
