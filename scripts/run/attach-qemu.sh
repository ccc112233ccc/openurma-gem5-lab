#!/usr/bin/env bash
set -euo pipefail

node=${1:-}
lab="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
run_dir="${UBSIM_QEMU_DUAL_OUT:-$lab/run-qemu-dual}"
node_count=$(awk -F= '$1 == "node_count" {print $2; exit}' "$run_dir/run-manifest.txt" 2>/dev/null || true)
[[ "$node_count" =~ ^[2-8]$ ]] || node_count=2
[[ "$node" =~ ^[0-7]$ ]] && (( node < node_count )) || { echo "usage: ./lab attach-qemu NODE (0..$((node_count - 1)))" >&2; exit 2; }
uart0=$(awk -F= '$1 == "uart0" {print $2; exit}' "$run_dir/run-manifest.txt" 2>/dev/null || true)
uart1=$(awk -F= '$1 == "uart1" {print $2; exit}' "$run_dir/run-manifest.txt" 2>/dev/null || true)
uart0=${uart0:-3560}; uart1=${uart1:-3570}
port=$((uart0 + node * (uart1 - uart0)))
echo "Connecting to QEMU node$node on localhost:$port"
echo "Use Ctrl-] to close nc without stopping the guest."
exec nc 127.0.0.1 "$port"
