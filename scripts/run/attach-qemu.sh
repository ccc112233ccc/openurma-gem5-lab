#!/usr/bin/env bash
set -euo pipefail

node=${1:-}
[[ "$node" == 0 || "$node" == 1 ]] || { echo "usage: ./lab attach-qemu 0|1" >&2; exit 2; }
uart0="${OPENURMA_QEMU_UART0:-3560}"
uart1="${OPENURMA_QEMU_UART1:-3570}"
if [[ "$node" == 0 ]]; then port=$uart0; else port=$uart1; fi
echo "Connecting to QEMU node$node on localhost:$port"
echo "Use Ctrl-] to close nc without stopping the guest."
exec nc 127.0.0.1 "$port"
