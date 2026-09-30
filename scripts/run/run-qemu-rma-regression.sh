#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab="$(cd "$script_dir/../.." && pwd)"
run_root="${UBSIM_QEMU_DUAL_OUT:-$lab/run-qemu-dual}"

if [[ "${1:-}" == --help ]]; then
    cat <<EOF
usage: ./lab qemu-rma-regression [OUTPUT_DIRECTORY]

Runs the functional SEND/READ/WRITE, SQ-wrap, outstanding, and large-message
fragmentation matrix against an already shell-ready dual-QEMU environment.
QEMU conservative synchronization is disabled, so reported perftest timing is
functional evidence only and is not a virtual-time performance result.
EOF
    exit 0
fi
(( $# <= 1 )) || { echo "usage: ./lab qemu-rma-regression [OUTPUT_DIRECTORY]" >&2; exit 2; }

for component in node0/qemu node1/qemu udma-node0/udma udma-node1/udma ub-fabric/ns3; do
    pidfile="$run_root/$component.pid"
    [[ -r "$pidfile" ]] || { echo "missing live QEMU component: $pidfile" >&2; exit 2; }
    pid=$(sed -n '1p' "$pidfile")
    [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null || {
        echo "QEMU component is not running: $component" >&2; exit 2;
    }
done

for node in 0 1; do
    terminal="$run_root/node$node/system.terminal"
    grep -aq 'Official UDMA full-system guest' "$terminal" || {
        echo "node$node shell is not ready; inspect $terminal" >&2; exit 2;
    }
done

output="${1:-$lab/experiments/qemu-rma-regression-$(date -u +%Y%m%dT%H%M%SZ)}"
python3 "$lab/tools/dual_serial_command.py" \
    --ports "${UBSIM_QEMU_UART0:-3560}" "${UBSIM_QEMU_UART1:-3570}" \
    --command "ubsim-net-up" --timeout 60 --prompt-kick-after 1
python3 "$lab/tools/dual_serial_command.py" \
    --ports "${UBSIM_QEMU_UART0:-3560}" "${UBSIM_QEMU_UART1:-3570}" \
    --commands "ping -c 1 10.0.0.2" "ping -c 1 10.0.0.1" \
    --timeout 60 --prompt-kick-after 1
python3 "$lab/tools/run_modular_rma_regression.py" \
    --lab "$lab" --run-root "$run_root" --output "$output" \
    --uart0 "${UBSIM_QEMU_UART0:-3560}" \
    --uart1 "${UBSIM_QEMU_UART1:-3570}" \
    --timeout "${UBSIM_RMA_TIMEOUT:-300}" --suite full --simulator qemu
echo "QEMU RMA regression results: $output"
