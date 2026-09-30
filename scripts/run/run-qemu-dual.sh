#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab="$(cd "$script_dir/../.." && pwd)"
run_dir="${OPENURMA_QEMU_DUAL_OUT:-$lab/run-qemu-dual}"
qemu="${OPENURMA_QEMU:-$lab/sources/qemu-11.1.1/build/qemu-system-aarch64}"
kernel="${OPENURMA_QEMU_KERNEL:-$lab/out/qemu-Image}"
initrd="${OPENURMA_INITRD:-$lab/out/official-udma.cpio.gz}"
runtime_tag="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
udma="${OPENURMA_UDMA_DEVICE_BINARY:-$lab/artifacts/udma-device-sim-build-$runtime_tag/udma-device-sim}"
ns3="${OPENURMA_NS3UB_ADAPTER:-$lab/sources/ns-3-ub/build-macos/scratch/ns3.44-ub-net-adapter}"
if [[ ! -x "$udma" && -x "$lab/artifacts/udma-device-sim-build/udma-device-sim" ]]; then
    udma="$lab/artifacts/udma-device-sim-build/udma-device-sim"
fi
uart0="${OPENURMA_QEMU_UART0:-3560}"
uart1="${OPENURMA_QEMU_UART1:-3570}"
oob_port="${OPENURMA_QEMU_OOB_PORT:-4560}"
rate_gbps="${OPENURMA_PEER_LINK_RATE_GBPS:-400}"
link_delay_ns="${OPENURMA_PEER_LATENCY_NS:-100}"
switch_delay_ns="${OPENURMA_QEMU_SWITCH_DELAY_NS:-50}"

die() { echo "run-qemu-dual.sh: $*" >&2; exit 2; }

if [[ "${1:-}" == --help ]]; then
    cat <<EOF
usage: ./lab start-qemu-dual

Starts two ARM64 QEMU/TCG guests, two standalone UDMA device processes, one
native ns-3 UB fabric process, and one QEMU socket OOB link.  Conservative
virtual-time synchronization is intentionally disabled: this is a functional
bring-up path, not a timing-result path.

Attach with './lab attach-qemu 0' and './lab attach-qemu 1'.  Stop with
'./lab stop-qemu'.  UARTs default to localhost:$uart0 and localhost:$uart1.
EOF
    exit 0
fi
(( $# == 0 )) || die "this command takes no positional arguments"

for path in "$qemu" "$kernel" "$initrd" "$udma" "$ns3" "$lab/tools/run-background.sh"; do
    [[ -x "$path" || -r "$path" ]] || die "missing required artifact: $path"
done
for value in "$uart0" "$uart1" "$oob_port" "$rate_gbps" "$link_delay_ns" "$switch_delay_ns"; do
    [[ "$value" =~ ^[0-9]+$ ]] || die "ports, rates and delays must be decimal integers"
done
(( uart0 > 1023 && uart1 > 1023 && oob_port > 1023 )) || die "ports must exceed 1023"
(( uart0 != uart1 && rate_gbps > 0 && link_delay_ns > 0 )) || die "invalid UART, rate or delay"

pid_live() {
    local file=$1 pid
    [[ -r "$file" ]] || return 1
    pid=$(sed -n '1p' "$file")
    [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null
}
for file in "$run_dir"/*/*.pid; do
    [[ -e "$file" ]] || continue
    pid_live "$file" && die "a previous QEMU dual process is still running ($file); use './lab stop-qemu'"
done

mkdir -p "$run_dir"/{node0,node1,udma-node0,udma-node1,ub-fabric}
: >"$run_dir/node0/qemu.log"
: >"$run_dir/node0/system.terminal"
: >"$run_dir/node1/qemu.log"
: >"$run_dir/node1/system.terminal"
: >"$run_dir/udma-node0/udma.log"
: >"$run_dir/udma-node1/udma.log"
: >"$run_dir/ub-fabric/ns3.log"
host0="/tmp/openurma-qemu-dual.node0.host.sock"
host1="/tmp/openurma-qemu-dual.node1.host.sock"
net0="/tmp/openurma-qemu-dual.node0.net.sock"
net1="/tmp/openurma-qemu-dual.node1.net.sock"
shm0="/tmp/openurma-qemu-dual.node0.shm"
shm1="/tmp/openurma-qemu-dual.node1.shm"
rm -f "$host0" "$host1" "$net0" "$net1" "$shm0" "$shm1"

start_complete=0
cleanup_failed_start() {
    local pidfile pid
    (( start_complete == 0 )) || return 0
    for pidfile in "$run_dir"/*/*.pid; do
        [[ -r "$pidfile" ]] || continue
        pid=$(sed -n '1p' "$pidfile")
        [[ "$pid" =~ ^[0-9]+$ ]] || continue
        kill "$pid" 2>/dev/null || true
    done
    rm -f "$host0" "$host1" "$net0" "$net1" "$shm0" "$shm1"
}
trap cleanup_failed_start EXIT

start_bg() {
    local pidfile=$1 logfile=$2
    shift 2
    nohup bash "$lab/tools/run-background.sh" "$pidfile" "$logfile" "$@" \
        </dev/null >/dev/null 2>&1 &
}

for node in 0 1; do
    eval "host=\$host$node"
    eval "net=\$net$node"
    eval "shm=\$shm$node"
    start_bg "$run_dir/udma-node$node/udma.pid" "$run_dir/udma-node$node/udma.log" \
        "$udma" --host-socket "$host" --net-socket "$net" --shm "$shm" \
        --sync off --host-link-latency-ps "$((link_delay_ns * 1000))" \
        --net-link-latency-ps "$((link_delay_ns * 1000))" \
        --sync-interval-ps "$((link_delay_ns * 1000))" \
        --eid "$((0x100 + node))" --ports 2 \
        --state-out "$run_dir/udma-node$node/state.bin"
done
for socket_path in "$host0" "$host1" "$net0" "$net1"; do
    for _ in $(seq 1 200); do
        [[ -S "$socket_path" ]] && break
        sleep 0.05
    done
    [[ -S "$socket_path" ]] || die "UDMA socket did not appear: $socket_path"
done

start_bg "$run_dir/ub-fabric/ns3.pid" "$run_dir/ub-fabric/ns3.log" \
    "$ns3" --ports 2 --link-delay-ps "$((link_delay_ns * 1000))" \
    --switch-delay-ps "$((switch_delay_ns * 1000))" --rate-gbps "$rate_gbps" \
    --sync off --sync-interval-ps "$((link_delay_ns * 1000))" \
    --endpoint "$net0,256" --endpoint "$net1,257"

launch_qemu() {
    local node=$1 host=$2 uart=$3 netdev=$4 mac=$5 out="$run_dir/node$1"
    start_bg "$out/qemu.pid" "$out/qemu.log" env \
        OPENURMA_QEMU_UB_HOST_SOCKET="$host" "$qemu" \
        -machine virt,accel=tcg,gic-version=2,highmem=off -cpu cortex-a72 \
        -smp 1 -m 1024 -kernel "$kernel" -initrd "$initrd" \
        -append "console=ttyAMA0 rdinit=/init nokaslr loglevel=5 openurma_node=$node openurma_provider=official" \
        -display none -monitor none -no-reboot \
        -chardev "socket,id=uart0,host=127.0.0.1,port=$uart,server=on,wait=off,logfile=$out/system.terminal,logappend=on" \
        -serial chardev:uart0 -device "e1000,netdev=oob,mac=$mac" -netdev "$netdev"
}

launch_qemu 0 "$host0" "$uart0" "socket,id=oob,listen=127.0.0.1:$oob_port" "02:00:00:00:00:01"
sleep 0.5
launch_qemu 1 "$host1" "$uart1" "socket,id=oob,connect=127.0.0.1:$oob_port" "02:00:00:00:00:02"

cat >"$run_dir/run-manifest.txt" <<EOF
runtime=qemu-tcg
node_count=2
network_backend=modular-ns3ub
synchronization=disabled
uart0=$uart0
uart1=$uart1
oob_port=$oob_port
peer_link_rate_gbps=$rate_gbps
peer_latency_ns=$link_delay_ns
switch_delay_ns=$switch_delay_ns
EOF
start_complete=1

echo "Started the two-node QEMU functional environment."
echo "  node0 UART: localhost:$uart0; EID ...:0100; OOB 10.0.0.1"
echo "  node1 UART: localhost:$uart1; EID ...:0101; OOB 10.0.0.2"
echo "  attach: ./lab attach-qemu 0   (and node 1 in another terminal)"
echo "  status: ./lab status-qemu"
echo "  logs:   $run_dir"
echo "  timing: conservative synchronization is OFF; do not use this run for latency claims"
