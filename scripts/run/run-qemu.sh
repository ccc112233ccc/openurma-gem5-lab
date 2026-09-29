#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab="$(cd "$script_dir/../.." && pwd)"
qemu="${OPENURMA_QEMU:-$lab/sources/qemu-11.1.1/build/qemu-system-aarch64}"
kernel="${OPENURMA_QEMU_KERNEL:-$lab/out/qemu-Image}"
initrd="${OPENURMA_INITRD:-$lab/out/official-udma.cpio.gz}"
run_dir="${OPENURMA_QEMU_OUT:-$lab/run-qemu}"
runtime_tag="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
udma="${OPENURMA_UDMA_DEVICE_BINARY:-$lab/artifacts/udma-device-sim-build-$runtime_tag/udma-device-sim}"
peer="${OPENURMA_UDMA_PEER_BINARY:-$lab/artifacts/udma-device-sim-build-$runtime_tag/udma-transport-peer}"
if [[ ! -x "$udma" && -x "$lab/artifacts/udma-device-sim-build/udma-device-sim" ]]; then
    udma="$lab/artifacts/udma-device-sim-build/udma-device-sim"
    peer="$lab/artifacts/udma-device-sim-build/udma-transport-peer"
fi

if [[ "${1:-}" == --help ]]; then
    cat <<EOF
usage: ./lab start-qemu

Starts one interactive ARM64 QEMU/TCG guest connected through UB-HOST to the
standalone UDMA model.  The network side is terminated by a protocol peer, so
this command validates official discovery, MMIO, DMA and driver probe; use the
multi-node gem5 launcher for traffic experiments until the QEMU/ns-3 launcher
is added.

Environment: OPENURMA_QEMU, OPENURMA_QEMU_KERNEL, OPENURMA_INITRD,
             OPENURMA_QEMU_OUT, OPENURMA_UDMA_DEVICE_BINARY.
EOF
    exit 0
fi
(( $# == 0 )) || { echo "run-qemu.sh takes no positional arguments" >&2; exit 2; }

for path in "$qemu" "$kernel" "$initrd" "$udma" "$peer"; do
    [[ -e "$path" ]] || { echo "missing required artifact: $path" >&2; exit 1; }
done
mkdir -p "$run_dir"
host_socket="/tmp/openurma-qemu.$$.host.sock"
net_socket="/tmp/openurma-qemu.$$.net.sock"
shm_path="/tmp/openurma-qemu.$$.shm"
udma_pid=""
peer_pid=""
cleanup() {
    [[ -z "$peer_pid" ]] || kill "$peer_pid" 2>/dev/null || true
    [[ -z "$udma_pid" ]] || kill "$udma_pid" 2>/dev/null || true
    rm -f "$host_socket" "$net_socket" "$shm_path"
}
trap cleanup EXIT INT TERM

"$udma" --host-socket "$host_socket" --net-socket "$net_socket" \
    --shm "$shm_path" --sync off --eid 256 --ports 2 \
    >"$run_dir/udma.log" 2>&1 &
udma_pid=$!
for _ in $(seq 1 100); do
    [[ -S "$host_socket" && -S "$net_socket" ]] && break
    sleep 0.05
done
[[ -S "$host_socket" && -S "$net_socket" ]] || {
    echo "UDMA sockets did not appear; inspect $run_dir/udma.log" >&2
    exit 1
}
"$peer" net "$net_socket" >"$run_dir/net-peer.log" 2>&1 &
peer_pid=$!

echo "Starting QEMU TCG; Ctrl-a x exits.  Logs: $run_dir"
OPENURMA_QEMU_UB_HOST_SOCKET="$host_socket" "$qemu" \
    -machine virt,accel=tcg,gic-version=2,highmem=off -cpu cortex-a72 \
    -smp 1 -m 1024 -kernel "$kernel" -initrd "$initrd" \
    -append 'console=ttyAMA0 rdinit=/init nokaslr loglevel=5 openurma_node=0 openurma_provider=official' \
    -nographic -no-reboot
