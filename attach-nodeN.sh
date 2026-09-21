#!/usr/bin/env bash
set -euo pipefail

if (( $# != 1 )) || [[ ! "$1" =~ ^[0-9]+$ ]]; then
    echo "usage: attach-nodeN.sh NODE" >&2
    exit 2
fi

node=$1
uart0="${OPENURMA_DUAL_UART0:-3460}"
uart1="${OPENURMA_DUAL_UART1:-3470}"
stride=$((uart1 - uart0))
(( stride > 0 )) || { echo "node1 UART must exceed node0 UART" >&2; exit 2; }

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export OPENURMA_M5TERM_PORT="$((uart0 + node * stride))"
exec bash "$script_dir/attach.sh"
