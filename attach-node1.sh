#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export OPENURMA_M5TERM_PORT="${OPENURMA_DUAL_UART1:-3470}"
exec bash "$script_dir/attach.sh"
