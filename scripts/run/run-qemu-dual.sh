#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab="$(cd "$script_dir/../.." && pwd)"
run_dir="${UBSIM_QEMU_DUAL_OUT:-$lab/run-qemu-dual}"
qemu="${UBSIM_QEMU:-$lab/sources/qemu-11.1.1/build/qemu-system-aarch64}"
kernel="${UBSIM_QEMU_KERNEL:-$lab/artifacts/kernel-64k-20261015/Image}"
initrd="${UBSIM_INITRD:-$lab/out/official-udma-64k-20261015.cpio.gz}"
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
port_count="${UBSIM_QEMU_PORT_COUNT:-2}"
node_cpu=("${UBSIM_QEMU_NODE0_CPU:-cortex-a72}" "${UBSIM_QEMU_NODE1_CPU:-cortex-a72}")
node_cpus=("${UBSIM_QEMU_NODE0_CPUS:-1}" "${UBSIM_QEMU_NODE1_CPUS:-1}")
node_memory_mib=("${UBSIM_QEMU_NODE0_MEM_MIB:-1024}" "${UBSIM_QEMU_NODE1_MEM_MIB:-1024}")
link_delay_ns="${UBSIM_PEER_LATENCY_NS:-100}"
node_link_delay_ns=("${UBSIM_QEMU_NODE0_LINK_LATENCY_NS:-$link_delay_ns}" "${UBSIM_QEMU_NODE1_LINK_LATENCY_NS:-$link_delay_ns}")
node_link_rate_gbps=("${UBSIM_QEMU_NODE0_LINK_RATE_GBPS:-$rate_gbps}" "${UBSIM_QEMU_NODE1_LINK_RATE_GBPS:-$rate_gbps}")
switch_delay_ns="${UBSIM_QEMU_SWITCH_DELAY_NS:-50}"
ub_fabric_layout="${UBSIM_UB_FABRIC_LAYOUT:-switch}"
host_link_delay_ns="${UBSIM_QEMU_HOST_LATENCY_NS:-500}"
dma_max_outstanding="${UBSIM_QEMU_DMA_MAX_OUTSTANDING:-256}"
sq_prefetch_wqebbs="${UBSIM_SQ_PREFETCH_WQEBBS:-64}"
sq_fetch_burst_wqebbs="${UBSIM_SQ_FETCH_BURST_WQEBBS:-16}"
sq_fetch_max_outstanding="${UBSIM_SQ_FETCH_MAX_OUTSTANDING:-4}"
completion_event_slots="${UBSIM_COMPLETION_EVENT_SLOTS:-64}"
max_jfs_depth="${UBSIM_UDMA_MAX_JFS_DEPTH:-8192}"
max_jfr_depth="${UBSIM_UDMA_MAX_JFR_DEPTH:-8192}"
max_jfc_depth="${UBSIM_UDMA_MAX_JFC_DEPTH:-524288}"
max_jettys="${UBSIM_UDMA_MAX_JETTYS:-47}"
public_jettys="${UBSIM_UDMA_PUBLIC_JETTYS:-1}"
iotlb_entries="${UBSIM_QEMU_IOTLB_ENTRIES:-4096}"
icount_shift="${UBSIM_QEMU_ICOUNT_SHIFT:-0}"
ctp_retransmission="${UBSIM_CTP_RETRANSMISSION:-auto}"
ctp_rto_ps="${UBSIM_CTP_RTO_PS:-25600000}"
ctp_max_retransmissions="${UBSIM_CTP_MAX_RETRANSMISSIONS:-7}"
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
for value in "$uart0" "$uart1" "$oob_port" "$ssh0" "$ssh1" "$rate_gbps" "$port_count" "$link_delay_ns" "$switch_delay_ns" "$host_link_delay_ns" "$dma_max_outstanding" "$iotlb_entries"; do
    [[ "$value" =~ ^[0-9]+$ ]] || die "ports, rates and delays must be decimal integers"
done
[[ "$icount_shift" =~ ^[0-9]+$ ]] || die "UBSIM_QEMU_ICOUNT_SHIFT must be a non-negative integer"
for value in "$sq_prefetch_wqebbs" "$sq_fetch_burst_wqebbs" "$sq_fetch_max_outstanding"; do
    [[ "$value" =~ ^[0-9]+$ ]] && (( value > 0 && value <= 4096 )) ||
        die "SQ prefetch settings must be integers in 1..4096"
done
for value in "$max_jfs_depth" "$max_jfr_depth" "$max_jfc_depth"; do
    [[ "$value" =~ ^[0-9]+$ ]] && (( value > 0 && (value & (value - 1)) == 0 )) ||
        die "UDMA queue capacities must be positive powers of two"
done
(( max_jfs_depth <= 8192 && max_jfr_depth <= 32768 && max_jfc_depth <= 2097152 )) ||
    die "UDMA queue capacity exceeds the modeled context encoding"
[[ "$completion_event_slots" =~ ^[0-9]+$ ]] && (( completion_event_slots > 0 && completion_event_slots <= 4096 )) ||
    die "completion event slots must be an integer in 1..4096"
[[ "$max_jettys" =~ ^[0-9]+$ && "$public_jettys" =~ ^[0-9]+$ ]] &&
    (( max_jettys >= 2 && max_jettys <= 65535 && public_jettys >= 1 && public_jettys < max_jettys )) ||
    die "Jetty counts require 2 <= max-jettys <= 65535 and 1 <= public-jettys < max-jettys"
[[ "$ctp_rto_ps" =~ ^[0-9]+$ ]] && (( ctp_rto_ps > 0 )) ||
    die "UBSIM_CTP_RTO_PS must be a positive integer"
[[ "$ctp_max_retransmissions" =~ ^[0-9]+$ ]] && (( ctp_max_retransmissions > 0 )) ||
    die "UBSIM_CTP_MAX_RETRANSMISSIONS must be a positive integer"
case "$ctp_retransmission" in
    auto) ctp_retransmission=off ;;
    on|off) ;;
    *) die "UBSIM_CTP_RETRANSMISSION must be auto, on, or off" ;;
esac
(( uart0 > 1023 && uart1 > 1023 && oob_port > 1023 )) || die "ports must exceed 1023"
(( uart0 != uart1 && ssh0 != ssh1 && rate_gbps > 0 && link_delay_ns > 0 )) || die "invalid UART, SSH port, rate or delay"
(( port_count > 0 && port_count <= 8 )) || die "UBSIM_QEMU_PORT_COUNT must be in 1..8"
case "$ub_fabric_layout" in
    switch) ;;
    direct)
        (( port_count == 1 && switch_delay_ns == 0 )) ||
            die "direct UB link requires one port and zero switch delay"
        (( node_link_rate_gbps[0] == node_link_rate_gbps[1] )) ||
            die "direct UB link requires equal endpoint rates"
        ;;
    *) die "UBSIM_UB_FABRIC_LAYOUT must be switch or direct" ;;
esac
for node in 0 1; do
    case "${node_cpu[$node]}" in
        cortex-a53|cortex-a57|cortex-a72|cortex-a76|cortex-a710|neoverse-n1|neoverse-n2|neoverse-v1|max) ;;
        *) die "unsupported CPU model for node$node: ${node_cpu[$node]}" ;;
    esac
    [[ "${node_cpus[$node]}" =~ ^[0-9]+$ ]] &&
        (( node_cpus[node] >= 1 && node_cpus[node] <= 16 )) ||
        die "node$node core count must be in 1..16"
    [[ "${node_memory_mib[$node]}" =~ ^[0-9]+$ ]] &&
        (( node_memory_mib[node] >= 512 && node_memory_mib[node] <= 8192 )) ||
        die "node$node memory must be in 512..8192 MiB"
    [[ "${node_link_delay_ns[$node]}" =~ ^[0-9]+$ ]] &&
        (( node_link_delay_ns[node] > 0 )) ||
        die "node$node UB link delay must be positive nanoseconds"
    [[ "${node_link_rate_gbps[$node]}" =~ ^[0-9]+$ ]] &&
        (( node_link_rate_gbps[node] > 0 )) ||
        die "node$node UB link rate must be positive Gbps"
done
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

largest_memory_mib=${node_memory_mib[0]}
(( node_memory_mib[1] <= largest_memory_mib )) || largest_memory_mib=${node_memory_mib[1]}
python3 "$lab/tools/host_memory_guard.py" check --phase qemu-start --nodes 2 \
    --mem-size "${largest_memory_mib}MiB" || die "host memory preflight failed"
rm -f "$run_dir/run-manifest.txt" "$run_dir/memory-guard.pid" \
    "$run_dir/memory-guard.log" "$run_dir/memory-guard.stopped" \
    "$run_dir/memory-guard.stderr.log"

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
        --net-link-latency-ps "$((node_link_delay_ns[node] * 1000))" \
        --host-sync-interval-ps "$((host_link_delay_ns * 1000))" \
        --net-sync-interval-ps "$((node_link_delay_ns[node] * 1000))" \
        --eid "$((0x100 + node))" --ports "$port_count" \
        --dma-max-outstanding "$dma_max_outstanding" \
        --sq-prefetch-wqebbs "$sq_prefetch_wqebbs" \
        --sq-fetch-burst-wqebbs "$sq_fetch_burst_wqebbs" \
        --sq-fetch-max-outstanding "$sq_fetch_max_outstanding" \
        --completion-event-slots "$completion_event_slots" \
        --max-jfs-depth "$max_jfs_depth" --max-jfr-depth "$max_jfr_depth" \
        --max-jfc-depth "$max_jfc_depth" \
        --max-jettys "$max_jettys" --public-jettys "$public_jettys" \
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

ns3_layout_args=()
[[ "$ub_fabric_layout" != direct ]] || ns3_layout_args+=(--direct-link)
start_bg "$run_dir/ub-fabric/ns3.pid" "$run_dir/ub-fabric/ns3.log" \
    "$ns3" "${ns3_layout_args[@]}" --ports "$port_count" --link-delay-ps "$((link_delay_ns * 1000))" \
    --switch-delay-ps "$((switch_delay_ns * 1000))" --rate-gbps "$rate_gbps" \
    --sync "$sync_mode" --sync-interval-ps "$((link_delay_ns * 1000))" \
    --ctp-retransmission "$ctp_retransmission" --ctp-rto-ps "$ctp_rto_ps" \
    --ctp-max-retransmissions "$ctp_max_retransmissions" \
    --endpoint "$net0,256,$((node_link_delay_ns[0] * 1000)),${node_link_rate_gbps[0]}" \
    --endpoint "$net1,257,$((node_link_delay_ns[1] * 1000)),${node_link_rate_gbps[1]}"

launch_qemu() {
    local node=$1 host=$2 uart=$3 netdev=$4 mac=$5 ssh_port=$6 out="$run_dir/node$1"
    local cpu="${node_cpu[$node]}" cpus="${node_cpus[$node]}" memory_mib="${node_memory_mib[$node]}"
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
        -machine virt,accel=tcg,gic-version=2,highmem=off -cpu "$cpu" \
        ${timing_args[@]+"${timing_args[@]}"} \
        -smp "$cpus" -m "$memory_mib" -kernel "$kernel" -initrd "$initrd" \
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

cpu_type="${node_cpu[0]}"
cpu_count="${node_cpus[0]}"
memory_mib="${node_memory_mib[0]}"
[[ "${node_cpu[0]}" == "${node_cpu[1]}" ]] || cpu_type=mixed
[[ "${node_cpus[0]}" == "${node_cpus[1]}" ]] || cpu_count=mixed
[[ "${node_memory_mib[0]}" == "${node_memory_mib[1]}" ]] || memory_mib=mixed

cat >"$run_dir/run-manifest.txt" <<EOF
runtime=qemu-tcg-$mode
node_count=2
network_backend=modular-ns3ub
ub_fabric_layout=$ub_fabric_layout
synchronization=$sync_mode
uart0=$uart0
uart1=$uart1
oob_port=$oob_port
ssh0_port=$ssh0
ssh1_port=$ssh1
peer_link_rate_gbps=$rate_gbps
node0_link_rate_gbps=${node_link_rate_gbps[0]}
node0_link_latency_ns=${node_link_delay_ns[0]}
node1_link_rate_gbps=${node_link_rate_gbps[1]}
node1_link_latency_ns=${node_link_delay_ns[1]}
host_link_latency_ns=$host_link_delay_ns
dma_max_outstanding=$dma_max_outstanding
sq_prefetch_wqebbs=$sq_prefetch_wqebbs
sq_fetch_burst_wqebbs=$sq_fetch_burst_wqebbs
sq_fetch_max_outstanding=$sq_fetch_max_outstanding
completion_event_slots=$completion_event_slots
max_jfs_depth=$max_jfs_depth
max_jfr_depth=$max_jfr_depth
max_jfc_depth=$max_jfc_depth
max_jettys=$max_jettys
public_jettys=$public_jettys
udma_state_format=3
qemu_binary=$qemu
udma_device_binary=$udma
ns3_binary=$ns3
kernel_image=$kernel
initrd_image=$initrd
port_count=$port_count
cpu_type=$cpu_type
cpu_count=$cpu_count
memory_mib=$memory_mib
node0_cpu_type=${node_cpu[0]}
node0_cpu_count=${node_cpus[0]}
node0_memory_mib=${node_memory_mib[0]}
node1_cpu_type=${node_cpu[1]}
node1_cpu_count=${node_cpus[1]}
node1_memory_mib=${node_memory_mib[1]}
iotlb_entries=$iotlb_entries
peer_latency_ns=$link_delay_ns
switch_delay_ns=$switch_delay_ns
icount_shift=$icount_shift
ctp_retransmission=$ctp_retransmission
ctp_rto_ps=$ctp_rto_ps
ctp_max_retransmissions=$ctp_max_retransmissions
EOF
start_bg "$run_dir/memory-guard.pid" "$run_dir/memory-guard.stderr.log" \
    python3 "$lab/tools/host_memory_guard.py" monitor \
    --run-root "$run_dir" --nodes 2 --pid-name qemu.pid \
    --stop-script "$lab/scripts/run/stop-qemu-dual.sh"
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
