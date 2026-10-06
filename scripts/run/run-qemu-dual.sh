#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab="$(cd "$script_dir/../.." && pwd)"
run_dir="${UBSIM_QEMU_DUAL_OUT:-$lab/run-qemu-dual}"
qemu="${UBSIM_QEMU:-$lab/sources/qemu-11.1.1/build/qemu-system-aarch64}"
kernel="${UBSIM_QEMU_KERNEL:-$lab/out/qemu-Image}"
initrd="${UBSIM_INITRD:-$lab/out/official-udma.cpio.gz}"
runtime_tag="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
udma="${UBSIM_UDMA_DEVICE_BINARY:-$lab/artifacts/udma-device-sim-build-$runtime_tag/udma-device-sim}"
ns3="${UBSIM_NS3UB_ADAPTER:-$lab/sources/ns-3-ub/build-macos/scratch/ns3.44-ub-net-adapter}"
if [[ ! -x "$udma" && -x "$lab/artifacts/udma-device-sim-build/udma-device-sim" ]]; then
    udma="$lab/artifacts/udma-device-sim-build/udma-device-sim"
fi
uart0="${UBSIM_QEMU_UART0:-3560}"
uart1="${UBSIM_QEMU_UART1:-3570}"
oob_port="${UBSIM_QEMU_OOB_PORT:-4560}"
ssh0="${UBSIM_QEMU_SSH0_PORT:-2220}"
ssh1="${UBSIM_QEMU_SSH1_PORT:-2221}"
rate_gbps="${UBSIM_PEER_LINK_RATE_GBPS:-400}"
link_delay_ns="${UBSIM_PEER_LATENCY_NS:-100}"
switch_delay_ns="${UBSIM_QEMU_SWITCH_DELAY_NS:-50}"
host_link_delay_ns="${UBSIM_QEMU_HOST_LATENCY_NS:-500}"
dma_max_outstanding="${UBSIM_QEMU_DMA_MAX_OUTSTANDING:-32}"
iotlb_entries="${UBSIM_QEMU_IOTLB_ENTRIES:-4096}"
icount_shift="${UBSIM_QEMU_ICOUNT_SHIFT:-0}"
mode=functional
sync_mode=off

die() { echo "run-qemu-dual.sh: $*" >&2; exit 2; }

if [[ "${1:-}" == --help ]]; then
    cat <<EOF
usage: ./lab start-qemu-dual [--functional|--timing|--timing-strict]

Starts two ARM64 QEMU/TCG guests, two standalone UDMA device processes, one
native ns-3 UB fabric process, and one QEMU socket OOB link.  Functional mode
is the default. Timing mode uses deterministic TCG icount and conservative
adapter synchronization from virtual time zero. '--timing-strict' is retained
as an alias for existing automation.

Attach with './lab attach-qemu 0' and './lab attach-qemu 1'.  Stop with
'./lab stop-qemu'.  UARTs default to localhost:$uart0 and localhost:$uart1.
SSH forwards default to localhost:$ssh0 and localhost:$ssh1; log in as root
and press Enter at the empty-password prompt.
EOF
    exit 0
fi
case "${1:-}" in
    ""|--functional) ;;
    --timing) mode=timing; sync_mode=required ;;
    --timing-strict) mode=timing; sync_mode=required ;;
    *) die "unknown argument: $1" ;;
esac
(( $# <= 1 )) || die "too many arguments"

for path in "$qemu" "$kernel" "$initrd" "$udma" "$ns3" "$lab/tools/run-background.sh"; do
    [[ -x "$path" || -r "$path" ]] || die "missing required artifact: $path"
done
for value in "$uart0" "$uart1" "$oob_port" "$ssh0" "$ssh1" "$rate_gbps" "$link_delay_ns" "$switch_delay_ns" "$host_link_delay_ns" "$dma_max_outstanding" "$iotlb_entries"; do
    [[ "$value" =~ ^[0-9]+$ ]] || die "ports, rates and delays must be decimal integers"
done
[[ "$icount_shift" =~ ^[0-9]+$ ]] || die "UBSIM_QEMU_ICOUNT_SHIFT must be a non-negative integer"
(( uart0 > 1023 && uart1 > 1023 && oob_port > 1023 )) || die "ports must exceed 1023"
(( uart0 != uart1 && ssh0 != ssh1 && rate_gbps > 0 && link_delay_ns > 0 )) || die "invalid UART, SSH port, rate or delay"
(( dma_max_outstanding > 0 && dma_max_outstanding <= 4096 )) || die "invalid DMA outstanding limit"
(( iotlb_entries > 0 && iotlb_entries <= 1048576 )) || die "invalid IOTLB entry count"

pid_live() {
    local file=$1 pid command
    [[ -r "$file" ]] || return 1
    pid=$(sed -n '1p' "$file")
    [[ "$pid" =~ ^[0-9]+$ ]] || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    command=$(ps -p "$pid" -o command= 2>/dev/null || true)
    case "$command" in
        *qemu-system-aarch64*|*udma-device-sim*|*ub-net-adapter*) return 0 ;;
        *) return 1 ;;
    esac
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
host0="/tmp/ubsim-qemu-dual.node0.host.sock"
host1="/tmp/ubsim-qemu-dual.node1.host.sock"
net0="/tmp/ubsim-qemu-dual.node0.net.sock"
net1="/tmp/ubsim-qemu-dual.node1.net.sock"
shm0="/tmp/ubsim-qemu-dual.node0.shm"
shm1="/tmp/ubsim-qemu-dual.node1.shm"
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
        --sync "$sync_mode" --host-link-latency-ps "$((host_link_delay_ns * 1000))" \
        --net-link-latency-ps "$((link_delay_ns * 1000))" \
        --host-sync-interval-ps "$((host_link_delay_ns * 1000))" \
        --net-sync-interval-ps "$((link_delay_ns * 1000))" \
        --eid "$((0x100 + node))" --ports 2 \
        --dma-max-outstanding "$dma_max_outstanding" \
        --iotlb-entries "$iotlb_entries" \
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
    --sync "$sync_mode" --sync-interval-ps "$((link_delay_ns * 1000))" \
    --endpoint "$net0,256" --endpoint "$net1,257"

launch_qemu() {
    local node=$1 host=$2 uart=$3 netdev=$4 mac=$5 ssh_port=$6 out="$run_dir/node$1"
    local -a timing_args=()
    local adapter_poll_ns=1000
    if [[ "$mode" != functional ]]; then
        timing_args=(-icount "shift=$icount_shift,sleep=off")
        adapter_poll_ns=1000000
    fi
    start_bg "$out/qemu.pid" "$out/qemu.log" env \
        UBSIM_QEMU_UB_HOST_SOCKET="$host" \
        UBSIM_QEMU_UB_HOST_SYNC="$sync_mode" \
        UBSIM_QEMU_UB_HOST_POLL_NS="$adapter_poll_ns" \
        UBSIM_QEMU_UB_HOST_LATENCY_PS="$((host_link_delay_ns * 1000))" \
        UBSIM_QEMU_UB_HOST_SYNC_INTERVAL_PS="$((host_link_delay_ns * 1000))" \
        "$qemu" \
        -machine virt,accel=tcg,gic-version=2,highmem=off -cpu cortex-a72 \
        ${timing_args[@]+"${timing_args[@]}"} \
        -smp 1 -m 1024 -kernel "$kernel" -initrd "$initrd" \
        -append "console=ttyAMA0 rdinit=/init nokaslr loglevel=5 ubsim_node=$node ubsim_provider=official ubsim_auto_net=1 ubsim_ssh=1" \
        -display none -monitor none -no-reboot \
        -chardev "socket,id=uart0,host=127.0.0.1,port=$uart,server=on,wait=off,logfile=$out/system.terminal,logappend=on" \
        -serial chardev:uart0 \
        -device "e1000,netdev=oob,mac=$mac" -netdev "$netdev" \
        -device "e1000,netdev=ssh,mac=02:00:00:00:10:0$((node + 1))" \
        -netdev "user,id=ssh,hostfwd=tcp:127.0.0.1:$ssh_port-:22"
}

launch_qemu 0 "$host0" "$uart0" "socket,id=oob,listen=127.0.0.1:$oob_port" "02:00:00:00:00:01" "$ssh0"
sleep 0.5
launch_qemu 1 "$host1" "$uart1" "socket,id=oob,connect=127.0.0.1:$oob_port" "02:00:00:00:00:02" "$ssh1"

cat >"$run_dir/run-manifest.txt" <<EOF
runtime=qemu-tcg-$mode
node_count=2
network_backend=modular-ns3ub
synchronization=$sync_mode
uart0=$uart0
uart1=$uart1
oob_port=$oob_port
ssh0_port=$ssh0
ssh1_port=$ssh1
peer_link_rate_gbps=$rate_gbps
host_link_latency_ns=$host_link_delay_ns
dma_max_outstanding=$dma_max_outstanding
iotlb_entries=$iotlb_entries
peer_latency_ns=$link_delay_ns
switch_delay_ns=$switch_delay_ns
icount_shift=$icount_shift
EOF
start_complete=1

echo "Started the two-node QEMU $mode environment."
echo "  node0 UART: localhost:$uart0; EID ...:0100; OOB 10.0.0.1"
echo "  node1 UART: localhost:$uart1; EID ...:0101; OOB 10.0.0.2"
echo "  SSH: ssh -p $ssh0 root@127.0.0.1   (node0; empty password)"
echo "       ssh -p $ssh1 root@127.0.0.1   (node1; empty password)"
echo "  attach: ./lab attach-qemu 0   (and node 1 in another terminal)"
echo "  status: ./lab status-qemu"
echo "  logs:   $run_dir"
if [[ "$mode" == timing ]]; then
    echo "  timing: TCG icount shift=$icount_shift; conservative synchronization REQUIRED from time zero"
else
    echo "  timing: conservative synchronization is OFF; do not use this run for latency claims"
fi
