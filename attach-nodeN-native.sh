#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
node="${1:-}"
[[ "$node" =~ ^[0-7]$ ]] || { echo "usage: $0 NODE(0..7)" >&2; exit 2; }
uart0="${OPENURMA_DUAL_UART0:-3460}"
uart1="${OPENURMA_DUAL_UART1:-3470}"
stride=$((uart1 - uart0))
export OPENURMA_EXECUTION_MODE=native
export OPENURMA_LAB_ROOT="${OPENURMA_LAB_ROOT:-$script_dir}"
export OPENURMA_M5TERM_PORT="$((uart0 + node * stride))"
exec bash "$script_dir/attach.sh"
