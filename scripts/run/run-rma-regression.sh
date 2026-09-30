#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/runtime.sh
source "$script_dir/../runtime.sh"
repo_root="$(cd "$script_dir/../.." && pwd)"
lab="${UBSIM_LAB_ROOT:-$(ubsim_runtime_default_lab "$repo_root")}"
run_root="${UBSIM_DUAL_OUT:-$lab/run-dual}"
suite=full
if [[ "${1:-}" == --sync-smoke ]]; then
    suite=sync-smoke
    shift
fi
output="${1:-$lab/experiments/rma-regression-$suite-$(date -u +%Y%m%dT%H%M%SZ)}"
ubsim_runtime_start
ubsim_exec test -e "$run_root/sync.ready" || {
    echo "run './lab sync' after both shells are ready" >&2; exit 2;
}
ubsim_exec python3 "$lab/tools/run_modular_rma_regression.py" \
    --lab "$lab" --run-root "$run_root" --output "$output" \
    --uart0 "${UBSIM_DUAL_UART0:-3460}" \
    --uart1 "${UBSIM_DUAL_UART1:-3470}" \
    --timeout "${UBSIM_RMA_TIMEOUT:-1800}" --suite "$suite"
echo "RMA regression results: $output"
