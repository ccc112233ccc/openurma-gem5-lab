#!/usr/bin/env bash
set -euo pipefail

die() { echo "run-dual.sh: $*" >&2; exit 2; }

usage() {
    cat <<'EOF'
usage: run-dual.sh [OPTIONS]

Start two synchronized OpenURMA full-system guests. Every timing-model knob
can also be supplied through the environment variable shown below.

Profiles:
  --profile fast|server|udma400|legacy
                                OPENURMA_DUAL_PROFILE (default: fast;
                                fast is the AtomicSimpleCPU functional path)

CPU, cache, and memory:
  --cpu-mode MODE               OPENURMA_CPU_MODE (atomic_fast is fastest;
                                server_o3 uses Atomic boot + ArmO3 ROI)
  --m5ops-base HEX              OPENURMA_M5OPS_BASE (VExpress m5ops MMIO ABI)
  --cpu-freq FREQ               OPENURMA_CPU_FREQ
  --num-cpus N                  OPENURMA_NUM_CPUS
  --benchmark-cpu N             OPENURMA_BENCHMARK_CPU
  --o3-width N                  OPENURMA_O3_WIDTH
  --o3-rob-entries N            OPENURMA_O3_ROB_ENTRIES
  --o3-iq-entries N             OPENURMA_O3_IQ_ENTRIES
  --o3-lq-entries N             OPENURMA_O3_LQ_ENTRIES
  --o3-sq-entries N             OPENURMA_O3_SQ_ENTRIES
  --o3-load-ports N             OPENURMA_O3_LOAD_PORTS
  --o3-store-ports N            OPENURMA_O3_STORE_PORTS
  --o3-fetch-buffer-bytes N     OPENURMA_O3_FETCH_BUFFER_BYTES
  --o3-fetch-queue-entries N    OPENURMA_O3_FETCH_QUEUE_ENTRIES
  --o3-phys-int-regs N          OPENURMA_O3_PHYS_INT_REGS
  --o3-phys-float-regs N        OPENURMA_O3_PHYS_FLOAT_REGS
  --o3-phys-vec-regs N          OPENURMA_O3_PHYS_VEC_REGS
  --o3-phys-vec-pred-regs N     OPENURMA_O3_PHYS_VEC_PRED_REGS
  --o3-phys-mat-regs N          OPENURMA_O3_PHYS_MAT_REGS
  --cache-line-size N           OPENURMA_CACHE_LINE_SIZE
  --last-cache-level 1|2|3      OPENURMA_LAST_CACHE_LEVEL
  --l1i-size SIZE               OPENURMA_L1I_SIZE
  --l1i-assoc N                 OPENURMA_L1I_ASSOC
  --l1i-latency T,D,R           OPENURMA_L1I_LATENCY
  --l1i-mshrs N                 OPENURMA_L1I_MSHRS
  --l1i-targets N               OPENURMA_L1I_TARGETS
  --l1i-write-buffers N         OPENURMA_L1I_WRITE_BUFFERS
  --l1d-size SIZE               OPENURMA_L1D_SIZE
  --l1d-assoc N                 OPENURMA_L1D_ASSOC
  --l1d-latency T,D,R           OPENURMA_L1D_LATENCY
  --l1d-mshrs N                 OPENURMA_L1D_MSHRS
  --l1d-targets N               OPENURMA_L1D_TARGETS
  --l1d-write-buffers N         OPENURMA_L1D_WRITE_BUFFERS
  --l2-size SIZE                OPENURMA_L2_SIZE
  --l2-assoc N                  OPENURMA_L2_ASSOC
  --l2-latency T,D,R            OPENURMA_L2_LATENCY
  --l2-mshrs N                  OPENURMA_L2_MSHRS
  --l2-targets N                OPENURMA_L2_TARGETS
  --l2-write-buffers N          OPENURMA_L2_WRITE_BUFFERS
  --l3-size SIZE                OPENURMA_L3_SIZE
  --l3-assoc N                  OPENURMA_L3_ASSOC
  --l3-latency T,D,R            OPENURMA_L3_LATENCY
  --l3-mshrs N                  OPENURMA_L3_MSHRS
  --l3-targets N                OPENURMA_L3_TARGETS
  --l3-write-buffers N          OPENURMA_L3_WRITE_BUFFERS
  --fabric-freq FREQ            OPENURMA_FABRIC_FREQ
  --fabric-width-bytes N        OPENURMA_FABRIC_WIDTH_BYTES
  --coherent-bus-frontend-latency N
                                OPENURMA_COHERENT_BUS_FRONTEND_LATENCY
  --coherent-bus-forward-latency N
                                OPENURMA_COHERENT_BUS_FORWARD_LATENCY
  --coherent-bus-response-latency N
                                OPENURMA_COHERENT_BUS_RESPONSE_LATENCY
  --coherent-bus-snoop-response-latency N
                                OPENURMA_COHERENT_BUS_SNOOP_RESPONSE_LATENCY
  --coherent-bus-header-latency N
                                OPENURMA_COHERENT_BUS_HEADER_LATENCY
  --memory-bus-frontend-latency N
                                OPENURMA_MEMORY_BUS_FRONTEND_LATENCY
  --memory-bus-forward-latency N OPENURMA_MEMORY_BUS_FORWARD_LATENCY
  --memory-bus-response-latency N
                                OPENURMA_MEMORY_BUS_RESPONSE_LATENCY
  --memory-bus-snoop-response-latency N
                                OPENURMA_MEMORY_BUS_SNOOP_RESPONSE_LATENCY
  --memory-bus-header-latency N  OPENURMA_MEMORY_BUS_HEADER_LATENCY
  --core-bus-snoop-filter-capacity SIZE
                                OPENURMA_CORE_BUS_SNOOP_FILTER_CAPACITY
  --l3-bus-snoop-filter-capacity SIZE
                                OPENURMA_L3_BUS_SNOOP_FILTER_CAPACITY
  --membus-snoop-filter-capacity SIZE
                                OPENURMA_MEMBUS_SNOOP_FILTER_CAPACITY
  --io-bus-frontend-latency N   OPENURMA_IO_BUS_FRONTEND_LATENCY
  --io-bus-forward-latency N    OPENURMA_IO_BUS_FORWARD_LATENCY
  --io-bus-response-latency N   OPENURMA_IO_BUS_RESPONSE_LATENCY
  --io-bus-header-latency N     OPENURMA_IO_BUS_HEADER_LATENCY
  --io-cache-size SIZE          OPENURMA_IO_CACHE_SIZE
  --io-cache-assoc N            OPENURMA_IO_CACHE_ASSOC
  --io-cache-latency T,D,R      OPENURMA_IO_CACHE_LATENCY
  --io-cache-mshrs N            OPENURMA_IO_CACHE_MSHRS
  --io-cache-targets N          OPENURMA_IO_CACHE_TARGETS
  --io-cache-write-buffers N    OPENURMA_IO_CACHE_WRITE_BUFFERS
  --mem-size SIZE               OPENURMA_DUAL_MEM_SIZE
  --guest-mem-limit SIZE        OPENURMA_GUEST_MEM_LIMIT
  --mem-type TYPE               OPENURMA_MEM_TYPE
  --mem-channels N              OPENURMA_MEM_CHANNELS
  --mem-channels-intlv N        OPENURMA_MEM_CHANNELS_INTLV
  --mem-addr-mapping ORDER      OPENURMA_MEM_ADDR_MAPPING
  --mem-channel-xor-low-bit N   OPENURMA_MEM_CHANNEL_XOR_LOW_BIT
  --mem-ranks N                 OPENURMA_MEM_RANKS
  --mem-read-buffer-size N      OPENURMA_MEM_READ_BUFFER_SIZE
  --mem-write-buffer-size N     OPENURMA_MEM_WRITE_BUFFER_SIZE
  --mem-page-policy POLICY      OPENURMA_MEM_PAGE_POLICY
  --mem-max-accesses-per-row N  OPENURMA_MEM_MAX_ACCESSES_PER_ROW
  --mem-sched-policy POLICY     OPENURMA_MEM_SCHED_POLICY
  --mem-write-high-thresh N     OPENURMA_MEM_WRITE_HIGH_THRESH
  --mem-write-low-thresh N      OPENURMA_MEM_WRITE_LOW_THRESH
  --mem-min-writes-per-switch N OPENURMA_MEM_MIN_WRITES_PER_SWITCH
  --mem-min-reads-per-switch N  OPENURMA_MEM_MIN_READS_PER_SWITCH
  --mem-ctrl-frontend-latency T OPENURMA_MEM_CTRL_FRONTEND_LATENCY
  --mem-ctrl-backend-latency T  OPENURMA_MEM_CTRL_BACKEND_LATENCY
  --mem-ctrl-command-window T   OPENURMA_MEM_CTRL_COMMAND_WINDOW

UB link:
  --ub-port-count N             OPENURMA_UB_PORT_COUNT
  --peer-latency-ns NS          OPENURMA_PEER_LATENCY_NS
  --sync-quantum-ns NS          OPENURMA_SYNC_QUANTUM_NS (default: lookahead)
  --peer-link-rate-gbps N       OPENURMA_PEER_LINK_RATE_GBPS
  --peer-serialization-stages N OPENURMA_PEER_SERIALIZATION_STAGES
  --peer-switch-delay TIME      OPENURMA_PEER_SWITCH_DELAY
  --peer-link-overhead-bytes N  OPENURMA_PEER_LINK_OVERHEAD_BYTES

UDMA front end:
  --sq-control-bytes N          OPENURMA_SQ_CONTROL_BYTES
  --wqebb-bytes N               OPENURMA_WQEBB_BYTES
  --sq-sge-bytes N              OPENURMA_SQ_SGE_BYTES
  --direct-wqe-max-blocks N     OPENURMA_DIRECT_WQE_MAX_BLOCKS
  --direct-wqe-latency TIME     OPENURMA_DIRECT_WQE_LATENCY
  --sq-fetch-latency TIME       OPENURMA_SQ_FETCH_LATENCY
  --sq-wqebb-latency TIME       OPENURMA_SQ_WQEBB_LATENCY
  --payload-dma-latency TIME    OPENURMA_PAYLOAD_DMA_LATENCY
  --payload-dma-rate-gbps N     OPENURMA_PAYLOAD_DMA_RATE_GBPS
  --udma-poll-interval TIME     OPENURMA_UDMA_POLL_INTERVAL
  --udma-iotlb-entries N        OPENURMA_UDMA_IOTLB_ENTRIES
  --dma-max-outstanding N       OPENURMA_DMA_MAX_OUTSTANDING

Other:
  --provider legacy|udma|official
                                OPENURMA_PROVIDER (default: profile-specific;
                                official loads the unmodified OLK UDMA stack)
  --print-config                Print the resolved model without starting it
  -h, --help                    Show this help

TIME accepts a non-negative gem5 time such as 0ns, 25ns, or 1us. Command-line
options override environment variables, which override profile defaults.
EOF
}

# Parse into separate command-line overrides so a --profile appearing anywhere
# has the same precedence. Empty strings mean "not supplied"; zero is valid for
# knobs which explicitly support disabling a modeled cost.
profile="${OPENURMA_DUAL_PROFILE:-fast}"
cli_cpu_mode=""
cli_m5ops_base=""
cli_cpu_freq=""
cli_num_cpus=""
cli_benchmark_cpu=""
cli_o3_width=""
cli_o3_rob_entries=""
cli_o3_iq_entries=""
cli_o3_lq_entries=""
cli_o3_sq_entries=""
cli_o3_load_ports=""
cli_o3_store_ports=""
cli_o3_fetch_buffer_bytes=""
cli_o3_fetch_queue_entries=""
cli_o3_phys_int_regs=""
cli_o3_phys_float_regs=""
cli_o3_phys_vec_regs=""
cli_o3_phys_vec_pred_regs=""
cli_o3_phys_mat_regs=""
cli_cache_line_size=""
cli_last_cache_level=""
cli_l1i_size=""
cli_l1i_assoc=""
cli_l1i_latency=""
cli_l1i_mshrs=""
cli_l1i_targets=""
cli_l1i_write_buffers=""
cli_l1d_size=""
cli_l1d_assoc=""
cli_l1d_latency=""
cli_l1d_mshrs=""
cli_l1d_targets=""
cli_l1d_write_buffers=""
cli_l2_size=""
cli_l2_assoc=""
cli_l2_latency=""
cli_l2_mshrs=""
cli_l2_targets=""
cli_l2_write_buffers=""
cli_l3_size=""
cli_l3_assoc=""
cli_l3_latency=""
cli_l3_mshrs=""
cli_l3_targets=""
cli_l3_write_buffers=""
cli_fabric_freq=""
cli_fabric_width_bytes=""
cli_coherent_bus_frontend_latency=""
cli_coherent_bus_forward_latency=""
cli_coherent_bus_response_latency=""
cli_coherent_bus_snoop_response_latency=""
cli_coherent_bus_header_latency=""
cli_memory_bus_frontend_latency=""
cli_memory_bus_forward_latency=""
cli_memory_bus_response_latency=""
cli_memory_bus_snoop_response_latency=""
cli_memory_bus_header_latency=""
cli_core_bus_snoop_filter_capacity=""
cli_l3_bus_snoop_filter_capacity=""
cli_membus_snoop_filter_capacity=""
cli_io_bus_frontend_latency=""
cli_io_bus_forward_latency=""
cli_io_bus_response_latency=""
cli_io_bus_header_latency=""
cli_io_cache_size=""
cli_io_cache_assoc=""
cli_io_cache_latency=""
cli_io_cache_mshrs=""
cli_io_cache_targets=""
cli_io_cache_write_buffers=""
cli_mem_size=""
cli_guest_mem_limit=""
cli_mem_type=""
cli_mem_channels=""
cli_mem_channels_intlv=""
cli_mem_addr_mapping=""
cli_mem_channel_xor_low_bit=""
cli_mem_ranks=""
cli_mem_read_buffer_size=""
cli_mem_write_buffer_size=""
cli_mem_page_policy=""
cli_mem_max_accesses_per_row=""
cli_mem_sched_policy=""
cli_mem_write_high_thresh=""
cli_mem_write_low_thresh=""
cli_mem_min_writes_per_switch=""
cli_mem_min_reads_per_switch=""
cli_mem_ctrl_frontend_latency=""
cli_mem_ctrl_backend_latency=""
cli_mem_ctrl_command_window=""
cli_ub_port_count=""
cli_peer_latency_ns=""
cli_sync_quantum_ns=""
cli_peer_link_rate_gbps=""
cli_peer_serialization_stages=""
cli_peer_switch_delay=""
cli_peer_link_overhead_bytes=""
cli_sq_control_bytes=""
cli_wqebb_bytes=""
cli_sq_sge_bytes=""
cli_direct_wqe_max_blocks=""
cli_direct_wqe_latency=""
cli_sq_fetch_latency=""
cli_sq_wqebb_latency=""
cli_payload_dma_latency=""
cli_payload_dma_rate_gbps=""
cli_udma_poll_interval=""
cli_udma_iotlb_entries=""
cli_dma_max_outstanding=""
cli_provider=""
print_config=0

need_value() {
    (( $# >= 2 )) && [[ -n "$2" ]] || die "$1 requires a value"
}

while (( $# > 0 )); do
    case "$1" in
        --profile) need_value "$@"; profile=$2; shift 2 ;;
        --profile=*) profile=${1#*=}; shift ;;
        --cpu|--cpu-mode) need_value "$@"; cli_cpu_mode=$2; shift 2 ;;
        --cpu=*|--cpu-mode=*) cli_cpu_mode=${1#*=}; shift ;;
        --m5ops-base) need_value "$@"; cli_m5ops_base=$2; shift 2 ;;
        --m5ops-base=*) cli_m5ops_base=${1#*=}; shift ;;
        --cpu-freq) need_value "$@"; cli_cpu_freq=$2; shift 2 ;;
        --cpu-freq=*) cli_cpu_freq=${1#*=}; shift ;;
        --num-cpus) need_value "$@"; cli_num_cpus=$2; shift 2 ;;
        --num-cpus=*) cli_num_cpus=${1#*=}; shift ;;
        --benchmark-cpu) need_value "$@"; cli_benchmark_cpu=$2; shift 2 ;;
        --benchmark-cpu=*) cli_benchmark_cpu=${1#*=}; shift ;;
        --o3-width) need_value "$@"; cli_o3_width=$2; shift 2 ;;
        --o3-width=*) cli_o3_width=${1#*=}; shift ;;
        --o3-rob-entries) need_value "$@"; cli_o3_rob_entries=$2; shift 2 ;;
        --o3-rob-entries=*) cli_o3_rob_entries=${1#*=}; shift ;;
        --o3-iq-entries) need_value "$@"; cli_o3_iq_entries=$2; shift 2 ;;
        --o3-iq-entries=*) cli_o3_iq_entries=${1#*=}; shift ;;
        --o3-lq-entries) need_value "$@"; cli_o3_lq_entries=$2; shift 2 ;;
        --o3-lq-entries=*) cli_o3_lq_entries=${1#*=}; shift ;;
        --o3-sq-entries) need_value "$@"; cli_o3_sq_entries=$2; shift 2 ;;
        --o3-sq-entries=*) cli_o3_sq_entries=${1#*=}; shift ;;
        --o3-load-ports) need_value "$@"; cli_o3_load_ports=$2; shift 2 ;;
        --o3-load-ports=*) cli_o3_load_ports=${1#*=}; shift ;;
        --o3-store-ports) need_value "$@"; cli_o3_store_ports=$2; shift 2 ;;
        --o3-store-ports=*) cli_o3_store_ports=${1#*=}; shift ;;
        --o3-fetch-buffer-bytes) need_value "$@"; cli_o3_fetch_buffer_bytes=$2; shift 2 ;;
        --o3-fetch-buffer-bytes=*) cli_o3_fetch_buffer_bytes=${1#*=}; shift ;;
        --o3-fetch-queue-entries) need_value "$@"; cli_o3_fetch_queue_entries=$2; shift 2 ;;
        --o3-fetch-queue-entries=*) cli_o3_fetch_queue_entries=${1#*=}; shift ;;
        --o3-phys-int-regs) need_value "$@"; cli_o3_phys_int_regs=$2; shift 2 ;;
        --o3-phys-int-regs=*) cli_o3_phys_int_regs=${1#*=}; shift ;;
        --o3-phys-float-regs) need_value "$@"; cli_o3_phys_float_regs=$2; shift 2 ;;
        --o3-phys-float-regs=*) cli_o3_phys_float_regs=${1#*=}; shift ;;
        --o3-phys-vec-regs) need_value "$@"; cli_o3_phys_vec_regs=$2; shift 2 ;;
        --o3-phys-vec-regs=*) cli_o3_phys_vec_regs=${1#*=}; shift ;;
        --o3-phys-vec-pred-regs) need_value "$@"; cli_o3_phys_vec_pred_regs=$2; shift 2 ;;
        --o3-phys-vec-pred-regs=*) cli_o3_phys_vec_pred_regs=${1#*=}; shift ;;
        --o3-phys-mat-regs) need_value "$@"; cli_o3_phys_mat_regs=$2; shift 2 ;;
        --o3-phys-mat-regs=*) cli_o3_phys_mat_regs=${1#*=}; shift ;;
        --cache-line-size) need_value "$@"; cli_cache_line_size=$2; shift 2 ;;
        --cache-line-size=*) cli_cache_line_size=${1#*=}; shift ;;
        --last-cache-level) need_value "$@"; cli_last_cache_level=$2; shift 2 ;;
        --last-cache-level=*) cli_last_cache_level=${1#*=}; shift ;;
        --l1i-size) need_value "$@"; cli_l1i_size=$2; shift 2 ;;
        --l1i-size=*) cli_l1i_size=${1#*=}; shift ;;
        --l1i-assoc) need_value "$@"; cli_l1i_assoc=$2; shift 2 ;;
        --l1i-assoc=*) cli_l1i_assoc=${1#*=}; shift ;;
        --l1i-latency) need_value "$@"; cli_l1i_latency=$2; shift 2 ;;
        --l1i-latency=*) cli_l1i_latency=${1#*=}; shift ;;
        --l1i-mshrs) need_value "$@"; cli_l1i_mshrs=$2; shift 2 ;;
        --l1i-mshrs=*) cli_l1i_mshrs=${1#*=}; shift ;;
        --l1i-targets) need_value "$@"; cli_l1i_targets=$2; shift 2 ;;
        --l1i-targets=*) cli_l1i_targets=${1#*=}; shift ;;
        --l1i-write-buffers) need_value "$@"; cli_l1i_write_buffers=$2; shift 2 ;;
        --l1i-write-buffers=*) cli_l1i_write_buffers=${1#*=}; shift ;;
        --l1d-size) need_value "$@"; cli_l1d_size=$2; shift 2 ;;
        --l1d-size=*) cli_l1d_size=${1#*=}; shift ;;
        --l1d-assoc) need_value "$@"; cli_l1d_assoc=$2; shift 2 ;;
        --l1d-assoc=*) cli_l1d_assoc=${1#*=}; shift ;;
        --l1d-latency) need_value "$@"; cli_l1d_latency=$2; shift 2 ;;
        --l1d-latency=*) cli_l1d_latency=${1#*=}; shift ;;
        --l1d-mshrs) need_value "$@"; cli_l1d_mshrs=$2; shift 2 ;;
        --l1d-mshrs=*) cli_l1d_mshrs=${1#*=}; shift ;;
        --l1d-targets) need_value "$@"; cli_l1d_targets=$2; shift 2 ;;
        --l1d-targets=*) cli_l1d_targets=${1#*=}; shift ;;
        --l1d-write-buffers) need_value "$@"; cli_l1d_write_buffers=$2; shift 2 ;;
        --l1d-write-buffers=*) cli_l1d_write_buffers=${1#*=}; shift ;;
        --l2-size) need_value "$@"; cli_l2_size=$2; shift 2 ;;
        --l2-size=*) cli_l2_size=${1#*=}; shift ;;
        --l2-assoc) need_value "$@"; cli_l2_assoc=$2; shift 2 ;;
        --l2-assoc=*) cli_l2_assoc=${1#*=}; shift ;;
        --l2-latency) need_value "$@"; cli_l2_latency=$2; shift 2 ;;
        --l2-latency=*) cli_l2_latency=${1#*=}; shift ;;
        --l2-mshrs) need_value "$@"; cli_l2_mshrs=$2; shift 2 ;;
        --l2-mshrs=*) cli_l2_mshrs=${1#*=}; shift ;;
        --l2-targets) need_value "$@"; cli_l2_targets=$2; shift 2 ;;
        --l2-targets=*) cli_l2_targets=${1#*=}; shift ;;
        --l2-write-buffers) need_value "$@"; cli_l2_write_buffers=$2; shift 2 ;;
        --l2-write-buffers=*) cli_l2_write_buffers=${1#*=}; shift ;;
        --l3-size) need_value "$@"; cli_l3_size=$2; shift 2 ;;
        --l3-size=*) cli_l3_size=${1#*=}; shift ;;
        --l3-assoc) need_value "$@"; cli_l3_assoc=$2; shift 2 ;;
        --l3-assoc=*) cli_l3_assoc=${1#*=}; shift ;;
        --l3-latency) need_value "$@"; cli_l3_latency=$2; shift 2 ;;
        --l3-latency=*) cli_l3_latency=${1#*=}; shift ;;
        --l3-mshrs) need_value "$@"; cli_l3_mshrs=$2; shift 2 ;;
        --l3-mshrs=*) cli_l3_mshrs=${1#*=}; shift ;;
        --l3-targets) need_value "$@"; cli_l3_targets=$2; shift 2 ;;
        --l3-targets=*) cli_l3_targets=${1#*=}; shift ;;
        --l3-write-buffers) need_value "$@"; cli_l3_write_buffers=$2; shift 2 ;;
        --l3-write-buffers=*) cli_l3_write_buffers=${1#*=}; shift ;;
        --fabric-freq) need_value "$@"; cli_fabric_freq=$2; shift 2 ;;
        --fabric-freq=*) cli_fabric_freq=${1#*=}; shift ;;
        --fabric-width-bytes) need_value "$@"; cli_fabric_width_bytes=$2; shift 2 ;;
        --fabric-width-bytes=*) cli_fabric_width_bytes=${1#*=}; shift ;;
        --coherent-bus-frontend-latency) need_value "$@"; cli_coherent_bus_frontend_latency=$2; shift 2 ;;
        --coherent-bus-frontend-latency=*) cli_coherent_bus_frontend_latency=${1#*=}; shift ;;
        --coherent-bus-forward-latency) need_value "$@"; cli_coherent_bus_forward_latency=$2; shift 2 ;;
        --coherent-bus-forward-latency=*) cli_coherent_bus_forward_latency=${1#*=}; shift ;;
        --coherent-bus-response-latency) need_value "$@"; cli_coherent_bus_response_latency=$2; shift 2 ;;
        --coherent-bus-response-latency=*) cli_coherent_bus_response_latency=${1#*=}; shift ;;
        --coherent-bus-snoop-response-latency) need_value "$@"; cli_coherent_bus_snoop_response_latency=$2; shift 2 ;;
        --coherent-bus-snoop-response-latency=*) cli_coherent_bus_snoop_response_latency=${1#*=}; shift ;;
        --coherent-bus-header-latency) need_value "$@"; cli_coherent_bus_header_latency=$2; shift 2 ;;
        --coherent-bus-header-latency=*) cli_coherent_bus_header_latency=${1#*=}; shift ;;
        --memory-bus-frontend-latency) need_value "$@"; cli_memory_bus_frontend_latency=$2; shift 2 ;;
        --memory-bus-frontend-latency=*) cli_memory_bus_frontend_latency=${1#*=}; shift ;;
        --memory-bus-forward-latency) need_value "$@"; cli_memory_bus_forward_latency=$2; shift 2 ;;
        --memory-bus-forward-latency=*) cli_memory_bus_forward_latency=${1#*=}; shift ;;
        --memory-bus-response-latency) need_value "$@"; cli_memory_bus_response_latency=$2; shift 2 ;;
        --memory-bus-response-latency=*) cli_memory_bus_response_latency=${1#*=}; shift ;;
        --memory-bus-snoop-response-latency) need_value "$@"; cli_memory_bus_snoop_response_latency=$2; shift 2 ;;
        --memory-bus-snoop-response-latency=*) cli_memory_bus_snoop_response_latency=${1#*=}; shift ;;
        --memory-bus-header-latency) need_value "$@"; cli_memory_bus_header_latency=$2; shift 2 ;;
        --memory-bus-header-latency=*) cli_memory_bus_header_latency=${1#*=}; shift ;;
        --core-bus-snoop-filter-capacity) need_value "$@"; cli_core_bus_snoop_filter_capacity=$2; shift 2 ;;
        --core-bus-snoop-filter-capacity=*) cli_core_bus_snoop_filter_capacity=${1#*=}; shift ;;
        --l3-bus-snoop-filter-capacity) need_value "$@"; cli_l3_bus_snoop_filter_capacity=$2; shift 2 ;;
        --l3-bus-snoop-filter-capacity=*) cli_l3_bus_snoop_filter_capacity=${1#*=}; shift ;;
        --membus-snoop-filter-capacity) need_value "$@"; cli_membus_snoop_filter_capacity=$2; shift 2 ;;
        --membus-snoop-filter-capacity=*) cli_membus_snoop_filter_capacity=${1#*=}; shift ;;
        --io-bus-frontend-latency) need_value "$@"; cli_io_bus_frontend_latency=$2; shift 2 ;;
        --io-bus-frontend-latency=*) cli_io_bus_frontend_latency=${1#*=}; shift ;;
        --io-bus-forward-latency) need_value "$@"; cli_io_bus_forward_latency=$2; shift 2 ;;
        --io-bus-forward-latency=*) cli_io_bus_forward_latency=${1#*=}; shift ;;
        --io-bus-response-latency) need_value "$@"; cli_io_bus_response_latency=$2; shift 2 ;;
        --io-bus-response-latency=*) cli_io_bus_response_latency=${1#*=}; shift ;;
        --io-bus-header-latency) need_value "$@"; cli_io_bus_header_latency=$2; shift 2 ;;
        --io-bus-header-latency=*) cli_io_bus_header_latency=${1#*=}; shift ;;
        --io-cache-size) need_value "$@"; cli_io_cache_size=$2; shift 2 ;;
        --io-cache-size=*) cli_io_cache_size=${1#*=}; shift ;;
        --io-cache-assoc) need_value "$@"; cli_io_cache_assoc=$2; shift 2 ;;
        --io-cache-assoc=*) cli_io_cache_assoc=${1#*=}; shift ;;
        --io-cache-latency) need_value "$@"; cli_io_cache_latency=$2; shift 2 ;;
        --io-cache-latency=*) cli_io_cache_latency=${1#*=}; shift ;;
        --io-cache-mshrs) need_value "$@"; cli_io_cache_mshrs=$2; shift 2 ;;
        --io-cache-mshrs=*) cli_io_cache_mshrs=${1#*=}; shift ;;
        --io-cache-targets) need_value "$@"; cli_io_cache_targets=$2; shift 2 ;;
        --io-cache-targets=*) cli_io_cache_targets=${1#*=}; shift ;;
        --io-cache-write-buffers) need_value "$@"; cli_io_cache_write_buffers=$2; shift 2 ;;
        --io-cache-write-buffers=*) cli_io_cache_write_buffers=${1#*=}; shift ;;
        --mem-size) need_value "$@"; cli_mem_size=$2; shift 2 ;;
        --mem-size=*) cli_mem_size=${1#*=}; shift ;;
        --guest-mem-limit) need_value "$@"; cli_guest_mem_limit=$2; shift 2 ;;
        --guest-mem-limit=*) cli_guest_mem_limit=${1#*=}; shift ;;
        --mem-type) need_value "$@"; cli_mem_type=$2; shift 2 ;;
        --mem-type=*) cli_mem_type=${1#*=}; shift ;;
        --mem-channels) need_value "$@"; cli_mem_channels=$2; shift 2 ;;
        --mem-channels=*) cli_mem_channels=${1#*=}; shift ;;
        --mem-channels-intlv) need_value "$@"; cli_mem_channels_intlv=$2; shift 2 ;;
        --mem-channels-intlv=*) cli_mem_channels_intlv=${1#*=}; shift ;;
        --mem-addr-mapping) need_value "$@"; cli_mem_addr_mapping=$2; shift 2 ;;
        --mem-addr-mapping=*) cli_mem_addr_mapping=${1#*=}; shift ;;
        --mem-channel-xor-low-bit) need_value "$@"; cli_mem_channel_xor_low_bit=$2; shift 2 ;;
        --mem-channel-xor-low-bit=*) cli_mem_channel_xor_low_bit=${1#*=}; shift ;;
        --mem-ranks) need_value "$@"; cli_mem_ranks=$2; shift 2 ;;
        --mem-ranks=*) cli_mem_ranks=${1#*=}; shift ;;
        --mem-read-buffer-size) need_value "$@"; cli_mem_read_buffer_size=$2; shift 2 ;;
        --mem-read-buffer-size=*) cli_mem_read_buffer_size=${1#*=}; shift ;;
        --mem-write-buffer-size) need_value "$@"; cli_mem_write_buffer_size=$2; shift 2 ;;
        --mem-write-buffer-size=*) cli_mem_write_buffer_size=${1#*=}; shift ;;
        --mem-page-policy) need_value "$@"; cli_mem_page_policy=$2; shift 2 ;;
        --mem-page-policy=*) cli_mem_page_policy=${1#*=}; shift ;;
        --mem-max-accesses-per-row) need_value "$@"; cli_mem_max_accesses_per_row=$2; shift 2 ;;
        --mem-max-accesses-per-row=*) cli_mem_max_accesses_per_row=${1#*=}; shift ;;
        --mem-sched-policy) need_value "$@"; cli_mem_sched_policy=$2; shift 2 ;;
        --mem-sched-policy=*) cli_mem_sched_policy=${1#*=}; shift ;;
        --mem-write-high-thresh) need_value "$@"; cli_mem_write_high_thresh=$2; shift 2 ;;
        --mem-write-high-thresh=*) cli_mem_write_high_thresh=${1#*=}; shift ;;
        --mem-write-low-thresh) need_value "$@"; cli_mem_write_low_thresh=$2; shift 2 ;;
        --mem-write-low-thresh=*) cli_mem_write_low_thresh=${1#*=}; shift ;;
        --mem-min-writes-per-switch) need_value "$@"; cli_mem_min_writes_per_switch=$2; shift 2 ;;
        --mem-min-writes-per-switch=*) cli_mem_min_writes_per_switch=${1#*=}; shift ;;
        --mem-min-reads-per-switch) need_value "$@"; cli_mem_min_reads_per_switch=$2; shift 2 ;;
        --mem-min-reads-per-switch=*) cli_mem_min_reads_per_switch=${1#*=}; shift ;;
        --mem-ctrl-frontend-latency) need_value "$@"; cli_mem_ctrl_frontend_latency=$2; shift 2 ;;
        --mem-ctrl-frontend-latency=*) cli_mem_ctrl_frontend_latency=${1#*=}; shift ;;
        --mem-ctrl-backend-latency) need_value "$@"; cli_mem_ctrl_backend_latency=$2; shift 2 ;;
        --mem-ctrl-backend-latency=*) cli_mem_ctrl_backend_latency=${1#*=}; shift ;;
        --mem-ctrl-command-window) need_value "$@"; cli_mem_ctrl_command_window=$2; shift 2 ;;
        --mem-ctrl-command-window=*) cli_mem_ctrl_command_window=${1#*=}; shift ;;
        --ub-port-count) need_value "$@"; cli_ub_port_count=$2; shift 2 ;;
        --ub-port-count=*) cli_ub_port_count=${1#*=}; shift ;;
        --peer-latency-ns) need_value "$@"; cli_peer_latency_ns=$2; shift 2 ;;
        --peer-latency-ns=*) cli_peer_latency_ns=${1#*=}; shift ;;
        --sync-quantum-ns) need_value "$@"; cli_sync_quantum_ns=$2; shift 2 ;;
        --sync-quantum-ns=*) cli_sync_quantum_ns=${1#*=}; shift ;;
        --peer-link-rate-gbps) need_value "$@"; cli_peer_link_rate_gbps=$2; shift 2 ;;
        --peer-link-rate-gbps=*) cli_peer_link_rate_gbps=${1#*=}; shift ;;
        --peer-serialization-stages) need_value "$@"; cli_peer_serialization_stages=$2; shift 2 ;;
        --peer-serialization-stages=*) cli_peer_serialization_stages=${1#*=}; shift ;;
        --peer-switch-delay) need_value "$@"; cli_peer_switch_delay=$2; shift 2 ;;
        --peer-switch-delay=*) cli_peer_switch_delay=${1#*=}; shift ;;
        --peer-link-overhead-bytes) need_value "$@"; cli_peer_link_overhead_bytes=$2; shift 2 ;;
        --peer-link-overhead-bytes=*) cli_peer_link_overhead_bytes=${1#*=}; shift ;;
        --sq-control-bytes) need_value "$@"; cli_sq_control_bytes=$2; shift 2 ;;
        --sq-control-bytes=*) cli_sq_control_bytes=${1#*=}; shift ;;
        --wqebb-bytes) need_value "$@"; cli_wqebb_bytes=$2; shift 2 ;;
        --wqebb-bytes=*) cli_wqebb_bytes=${1#*=}; shift ;;
        --sq-sge-bytes) need_value "$@"; cli_sq_sge_bytes=$2; shift 2 ;;
        --sq-sge-bytes=*) cli_sq_sge_bytes=${1#*=}; shift ;;
        --direct-wqe-max-blocks) need_value "$@"; cli_direct_wqe_max_blocks=$2; shift 2 ;;
        --direct-wqe-max-blocks=*) cli_direct_wqe_max_blocks=${1#*=}; shift ;;
        --direct-wqe-latency) need_value "$@"; cli_direct_wqe_latency=$2; shift 2 ;;
        --direct-wqe-latency=*) cli_direct_wqe_latency=${1#*=}; shift ;;
        --sq-fetch-latency) need_value "$@"; cli_sq_fetch_latency=$2; shift 2 ;;
        --sq-fetch-latency=*) cli_sq_fetch_latency=${1#*=}; shift ;;
        --sq-wqebb-latency) need_value "$@"; cli_sq_wqebb_latency=$2; shift 2 ;;
        --sq-wqebb-latency=*) cli_sq_wqebb_latency=${1#*=}; shift ;;
        --payload-dma-latency) need_value "$@"; cli_payload_dma_latency=$2; shift 2 ;;
        --payload-dma-latency=*) cli_payload_dma_latency=${1#*=}; shift ;;
        --payload-dma-rate-gbps) need_value "$@"; cli_payload_dma_rate_gbps=$2; shift 2 ;;
        --payload-dma-rate-gbps=*) cli_payload_dma_rate_gbps=${1#*=}; shift ;;
        --udma-poll-interval) need_value "$@"; cli_udma_poll_interval=$2; shift 2 ;;
        --udma-poll-interval=*) cli_udma_poll_interval=${1#*=}; shift ;;
        --udma-iotlb-entries) need_value "$@"; cli_udma_iotlb_entries=$2; shift 2 ;;
        --udma-iotlb-entries=*) cli_udma_iotlb_entries=${1#*=}; shift ;;
        --dma-max-outstanding) need_value "$@"; cli_dma_max_outstanding=$2; shift 2 ;;
        --dma-max-outstanding=*) cli_dma_max_outstanding=${1#*=}; shift ;;
        --provider) need_value "$@"; cli_provider=$2; shift 2 ;;
        --provider=*) cli_provider=${1#*=}; shift ;;
        --print-config) print_config=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; (( $# == 0 )) || die "unexpected positional argument: $1" ;;
        -*) die "unknown option: $1 (try --help)" ;;
        *) die "unexpected positional argument: $1 (try --help)" ;;
    esac
done

# These are architectural/physical profile values, not constants fitted to a
# measured latency curve. Unknown device-specific service times stay at zero
# until the experiment supplies them explicitly.  The fast profiles retain
# the historical one-core/cache defaults; the server profile overrides every
# CPU/cache/memory field below and records the resolved contract in its
# manifest.
profile_provider=legacy
profile_revision=legacy-v1
profile_num_cpus=1
profile_benchmark_cpu=-1
profile_o3_width=8
profile_o3_rob_entries=192
profile_o3_iq_entries=64
profile_o3_lq_entries=32
profile_o3_sq_entries=32
profile_o3_load_ports=4
profile_o3_store_ports=2
profile_o3_fetch_buffer_bytes=64
profile_o3_fetch_queue_entries=32
profile_o3_phys_int_regs=256
profile_o3_phys_float_regs=256
profile_o3_phys_vec_regs=256
profile_o3_phys_vec_pred_regs=32
profile_o3_phys_mat_regs=2
profile_cache_line_size=64
profile_last_cache_level=2
profile_l1i_size=48kB
profile_l1i_assoc=3
profile_l1i_latency=1,1,1
profile_l1i_mshrs=4
profile_l1i_targets=8
profile_l1i_write_buffers=8
profile_l1d_size=32kB
profile_l1d_assoc=2
profile_l1d_latency=2,2,1
profile_l1d_mshrs=16
profile_l1d_targets=16
profile_l1d_write_buffers=16
profile_l2_size=1MB
profile_l2_assoc=16
profile_l2_latency=12,12,5
profile_l2_mshrs=32
profile_l2_targets=8
profile_l2_write_buffers=8
profile_l3_size=16MB
profile_l3_assoc=16
profile_l3_latency=20,20,20
profile_l3_mshrs=20
profile_l3_targets=12
profile_l3_write_buffers=8
profile_fabric_freq=1GHz
profile_fabric_width_bytes=16
profile_coherent_bus_frontend_latency=1
profile_coherent_bus_forward_latency=0
profile_coherent_bus_response_latency=1
profile_coherent_bus_snoop_response_latency=1
profile_coherent_bus_header_latency=1
profile_memory_bus_frontend_latency=3
profile_memory_bus_forward_latency=4
profile_memory_bus_response_latency=2
profile_memory_bus_snoop_response_latency=4
profile_memory_bus_header_latency=1
profile_core_bus_snoop_filter_capacity=2MiB
profile_l3_bus_snoop_filter_capacity=8MiB
profile_membus_snoop_filter_capacity=64MiB
profile_io_bus_frontend_latency=2
profile_io_bus_forward_latency=1
profile_io_bus_response_latency=2
profile_io_bus_header_latency=1
profile_io_cache_size=1kB
profile_io_cache_assoc=8
profile_io_cache_latency=1,1,1
profile_io_cache_mshrs=32
profile_io_cache_targets=32
profile_io_cache_write_buffers=32
profile_mem_size=1GB
profile_guest_mem_limit=1GB
profile_mem_type=DDR3_1600_8x8
profile_mem_channels=1
profile_mem_channels_intlv=128
profile_mem_addr_mapping=RoRaBaCoCh
profile_mem_channel_xor_low_bit=0
profile_mem_ranks=1
profile_mem_read_buffer_size=32
profile_mem_write_buffer_size=64
profile_mem_page_policy=open_adaptive
profile_mem_max_accesses_per_row=16
profile_mem_sched_policy=frfcfs
profile_mem_write_high_thresh=85
profile_mem_write_low_thresh=50
profile_mem_min_writes_per_switch=16
profile_mem_min_reads_per_switch=16
profile_mem_ctrl_frontend_latency=10ns
profile_mem_ctrl_backend_latency=10ns
profile_mem_ctrl_command_window=10ns
profile_ub_port_count=1
profile_udma_poll_interval=10ns
profile_udma_iotlb_entries=64
profile_dma_max_outstanding=16
case "$profile" in
    fast)
        # Fast functional path: keep the official UMDK/provider/driver stack
        # and the 400-Gbit/s direct UB link, but execute the full guest with
        # AtomicSimpleCPU and no timing caches.  Use this for interactive
        # bring-up and correctness checks, not for CPU/cache latency studies.
        profile_provider=udma
        profile_revision=udma400-atomic-fast-functional-v2
        profile_cpu_mode=atomic_fast
        profile_cpu_freq=3GHz
        profile_num_cpus=1
        profile_benchmark_cpu=0
        profile_peer_link_rate_gbps=400
        profile_peer_serialization_stages=1
        profile_peer_switch_delay=0ns
        profile_peer_link_overhead_bytes=0
        profile_sq_control_bytes=48
        profile_wqebb_bytes=64
        profile_sq_sge_bytes=16
        profile_direct_wqe_max_blocks=0
        profile_direct_wqe_latency=0ns
        profile_sq_fetch_latency=0ns
        profile_sq_wqebb_latency=0ns
        profile_payload_dma_latency=0ns
        profile_payload_dma_rate_gbps=0
        ;;
    server)
        # Generic reduced-core Arm server slice.  It deliberately does not
        # claim to reproduce a named CPU: stock gem5 ArmO3 supplies the core,
        # and the cache/memory capacities are explicit, overrideable inputs.
        profile_provider=udma
        profile_revision=server-o3-ddr4-coherent-tdma-v5
        profile_cpu_mode=server_o3
        profile_cpu_freq=3GHz
        profile_num_cpus=4
        profile_benchmark_cpu=2
        profile_o3_load_ports=4
        profile_o3_store_ports=2
        profile_last_cache_level=3
        profile_l1i_size=64kB
        profile_l1i_assoc=4
        profile_l1i_mshrs=8
        profile_l1i_targets=8
        profile_l1i_write_buffers=8
        profile_l1d_size=64kB
        profile_l1d_assoc=4
        profile_l1d_mshrs=16
        profile_l1d_targets=16
        profile_l1d_write_buffers=16
        profile_l2_size=1MB
        profile_l2_assoc=8
        profile_l2_mshrs=32
        profile_l2_targets=16
        profile_l2_write_buffers=16
        profile_l3_size=32MB
        profile_l3_assoc=16
        profile_l3_mshrs=64
        profile_l3_targets=16
        profile_l3_write_buffers=32
        profile_fabric_freq=2GHz
        profile_fabric_width_bytes=64
        # The controllers model a capacity-consistent 64 GiB socket
        # (8 GiB/rank x one rank x eight channels).  Linux is capped at 8 GiB
        # so page-allocator initialization does not dominate fast-forward.
        profile_mem_size=64GB
        profile_guest_mem_limit=8GB
        profile_mem_type=DDR4_2400_8x8
        profile_mem_channels=8
        profile_mem_ranks=1
        profile_mem_read_buffer_size=64
        profile_mem_write_buffer_size=128
        profile_peer_link_rate_gbps=400
        profile_peer_serialization_stages=1
        profile_peer_switch_delay=0ns
        profile_peer_link_overhead_bytes=0
        profile_sq_control_bytes=48
        profile_wqebb_bytes=64
        profile_sq_sge_bytes=16
        profile_direct_wqe_max_blocks=0
        profile_direct_wqe_latency=0ns
        profile_sq_fetch_latency=0ns
        profile_sq_wqebb_latency=0ns
        profile_payload_dma_latency=0ns
        profile_payload_dma_rate_gbps=0
        ;;
    udma400)
        profile_provider=udma
        profile_revision=udma400-atomic-cache-v1
        profile_cpu_mode=atomic_cache
        profile_cpu_freq=3GHz
        profile_peer_link_rate_gbps=400
        # The lab topology is two hosts directly connected.  A switched path
        # is an explicit experiment: pass --peer-serialization-stages 2 and a
        # measured/architectural --peer-switch-delay when appropriate.
        profile_peer_serialization_stages=1
        profile_peer_switch_delay=0ns
        profile_peer_link_overhead_bytes=0
        profile_sq_control_bytes=48
        profile_wqebb_bytes=64
        profile_sq_sge_bytes=16
        # Phase 1 deliberately advertises dwqe_enable=0 and exercises the
        # provider's ordinary memory-backed SQ path.
        profile_direct_wqe_max_blocks=0
        profile_direct_wqe_latency=0ns
        profile_sq_fetch_latency=0ns
        profile_sq_wqebb_latency=0ns
        profile_payload_dma_latency=0ns
        # Payload/SQ/CQ DMA is timed by gem5's memory hierarchy.  Do not add a
        # second, synthetic bandwidth term in the default profile.
        profile_payload_dma_rate_gbps=0
        ;;
    legacy)
        profile_cpu_mode=atomic
        profile_cpu_freq=3GHz
        profile_peer_link_rate_gbps=100
        profile_peer_serialization_stages=1
        profile_peer_switch_delay=0ns
        profile_peer_link_overhead_bytes=0
        profile_sq_control_bytes=0
        profile_wqebb_bytes=64
        profile_sq_sge_bytes=16
        profile_direct_wqe_max_blocks=0
        profile_direct_wqe_latency=0ns
        profile_sq_fetch_latency=0ns
        profile_sq_wqebb_latency=0ns
        profile_payload_dma_latency=0ns
        profile_payload_dma_rate_gbps=0
        ;;
    *) die "unknown profile '$profile'; expected fast, server, udma400, or legacy" ;;
esac

cpu_mode="${OPENURMA_CPU_MODE:-${OPENURMA_DUAL_CPU:-$profile_cpu_mode}}"
m5ops_base="${OPENURMA_M5OPS_BASE:-0x10010000}"
cpu_freq="${OPENURMA_CPU_FREQ:-${OPENURMA_DUAL_CPU_FREQ:-$profile_cpu_freq}}"
num_cpus="${OPENURMA_NUM_CPUS:-${OPENURMA_DUAL_NUM_CPUS:-$profile_num_cpus}}"
benchmark_cpu="${OPENURMA_BENCHMARK_CPU:-$profile_benchmark_cpu}"
o3_width="${OPENURMA_O3_WIDTH:-$profile_o3_width}"
o3_rob_entries="${OPENURMA_O3_ROB_ENTRIES:-$profile_o3_rob_entries}"
o3_iq_entries="${OPENURMA_O3_IQ_ENTRIES:-$profile_o3_iq_entries}"
o3_lq_entries="${OPENURMA_O3_LQ_ENTRIES:-$profile_o3_lq_entries}"
o3_sq_entries="${OPENURMA_O3_SQ_ENTRIES:-$profile_o3_sq_entries}"
o3_load_ports="${OPENURMA_O3_LOAD_PORTS:-$profile_o3_load_ports}"
o3_store_ports="${OPENURMA_O3_STORE_PORTS:-$profile_o3_store_ports}"
o3_fetch_buffer_bytes="${OPENURMA_O3_FETCH_BUFFER_BYTES:-$profile_o3_fetch_buffer_bytes}"
o3_fetch_queue_entries="${OPENURMA_O3_FETCH_QUEUE_ENTRIES:-$profile_o3_fetch_queue_entries}"
o3_phys_int_regs="${OPENURMA_O3_PHYS_INT_REGS:-$profile_o3_phys_int_regs}"
o3_phys_float_regs="${OPENURMA_O3_PHYS_FLOAT_REGS:-$profile_o3_phys_float_regs}"
o3_phys_vec_regs="${OPENURMA_O3_PHYS_VEC_REGS:-$profile_o3_phys_vec_regs}"
o3_phys_vec_pred_regs="${OPENURMA_O3_PHYS_VEC_PRED_REGS:-$profile_o3_phys_vec_pred_regs}"
o3_phys_mat_regs="${OPENURMA_O3_PHYS_MAT_REGS:-$profile_o3_phys_mat_regs}"
cache_line_size="${OPENURMA_CACHE_LINE_SIZE:-$profile_cache_line_size}"
last_cache_level="${OPENURMA_LAST_CACHE_LEVEL:-$profile_last_cache_level}"
l1i_size="${OPENURMA_L1I_SIZE:-$profile_l1i_size}"
l1i_assoc="${OPENURMA_L1I_ASSOC:-$profile_l1i_assoc}"
l1i_latency="${OPENURMA_L1I_LATENCY:-$profile_l1i_latency}"
l1i_mshrs="${OPENURMA_L1I_MSHRS:-$profile_l1i_mshrs}"
l1i_targets="${OPENURMA_L1I_TARGETS:-$profile_l1i_targets}"
l1i_write_buffers="${OPENURMA_L1I_WRITE_BUFFERS:-$profile_l1i_write_buffers}"
l1d_size="${OPENURMA_L1D_SIZE:-$profile_l1d_size}"
l1d_assoc="${OPENURMA_L1D_ASSOC:-$profile_l1d_assoc}"
l1d_latency="${OPENURMA_L1D_LATENCY:-$profile_l1d_latency}"
l1d_mshrs="${OPENURMA_L1D_MSHRS:-$profile_l1d_mshrs}"
l1d_targets="${OPENURMA_L1D_TARGETS:-$profile_l1d_targets}"
l1d_write_buffers="${OPENURMA_L1D_WRITE_BUFFERS:-$profile_l1d_write_buffers}"
l2_size="${OPENURMA_L2_SIZE:-$profile_l2_size}"
l2_assoc="${OPENURMA_L2_ASSOC:-$profile_l2_assoc}"
l2_latency="${OPENURMA_L2_LATENCY:-$profile_l2_latency}"
l2_mshrs="${OPENURMA_L2_MSHRS:-$profile_l2_mshrs}"
l2_targets="${OPENURMA_L2_TARGETS:-$profile_l2_targets}"
l2_write_buffers="${OPENURMA_L2_WRITE_BUFFERS:-$profile_l2_write_buffers}"
l3_size="${OPENURMA_L3_SIZE:-$profile_l3_size}"
l3_assoc="${OPENURMA_L3_ASSOC:-$profile_l3_assoc}"
l3_latency="${OPENURMA_L3_LATENCY:-$profile_l3_latency}"
l3_mshrs="${OPENURMA_L3_MSHRS:-$profile_l3_mshrs}"
l3_targets="${OPENURMA_L3_TARGETS:-$profile_l3_targets}"
l3_write_buffers="${OPENURMA_L3_WRITE_BUFFERS:-$profile_l3_write_buffers}"
fabric_freq="${OPENURMA_FABRIC_FREQ:-$profile_fabric_freq}"
fabric_width_bytes="${OPENURMA_FABRIC_WIDTH_BYTES:-$profile_fabric_width_bytes}"
coherent_bus_frontend_latency="${OPENURMA_COHERENT_BUS_FRONTEND_LATENCY:-$profile_coherent_bus_frontend_latency}"
coherent_bus_forward_latency="${OPENURMA_COHERENT_BUS_FORWARD_LATENCY:-$profile_coherent_bus_forward_latency}"
coherent_bus_response_latency="${OPENURMA_COHERENT_BUS_RESPONSE_LATENCY:-$profile_coherent_bus_response_latency}"
coherent_bus_snoop_response_latency="${OPENURMA_COHERENT_BUS_SNOOP_RESPONSE_LATENCY:-$profile_coherent_bus_snoop_response_latency}"
coherent_bus_header_latency="${OPENURMA_COHERENT_BUS_HEADER_LATENCY:-$profile_coherent_bus_header_latency}"
memory_bus_frontend_latency="${OPENURMA_MEMORY_BUS_FRONTEND_LATENCY:-$profile_memory_bus_frontend_latency}"
memory_bus_forward_latency="${OPENURMA_MEMORY_BUS_FORWARD_LATENCY:-$profile_memory_bus_forward_latency}"
memory_bus_response_latency="${OPENURMA_MEMORY_BUS_RESPONSE_LATENCY:-$profile_memory_bus_response_latency}"
memory_bus_snoop_response_latency="${OPENURMA_MEMORY_BUS_SNOOP_RESPONSE_LATENCY:-$profile_memory_bus_snoop_response_latency}"
memory_bus_header_latency="${OPENURMA_MEMORY_BUS_HEADER_LATENCY:-$profile_memory_bus_header_latency}"
core_bus_snoop_filter_capacity="${OPENURMA_CORE_BUS_SNOOP_FILTER_CAPACITY:-$profile_core_bus_snoop_filter_capacity}"
l3_bus_snoop_filter_capacity="${OPENURMA_L3_BUS_SNOOP_FILTER_CAPACITY:-$profile_l3_bus_snoop_filter_capacity}"
membus_snoop_filter_capacity="${OPENURMA_MEMBUS_SNOOP_FILTER_CAPACITY:-$profile_membus_snoop_filter_capacity}"
io_bus_frontend_latency="${OPENURMA_IO_BUS_FRONTEND_LATENCY:-$profile_io_bus_frontend_latency}"
io_bus_forward_latency="${OPENURMA_IO_BUS_FORWARD_LATENCY:-$profile_io_bus_forward_latency}"
io_bus_response_latency="${OPENURMA_IO_BUS_RESPONSE_LATENCY:-$profile_io_bus_response_latency}"
io_bus_header_latency="${OPENURMA_IO_BUS_HEADER_LATENCY:-$profile_io_bus_header_latency}"
io_cache_size="${OPENURMA_IO_CACHE_SIZE:-$profile_io_cache_size}"
io_cache_assoc="${OPENURMA_IO_CACHE_ASSOC:-$profile_io_cache_assoc}"
io_cache_latency="${OPENURMA_IO_CACHE_LATENCY:-$profile_io_cache_latency}"
io_cache_mshrs="${OPENURMA_IO_CACHE_MSHRS:-$profile_io_cache_mshrs}"
io_cache_targets="${OPENURMA_IO_CACHE_TARGETS:-$profile_io_cache_targets}"
io_cache_write_buffers="${OPENURMA_IO_CACHE_WRITE_BUFFERS:-$profile_io_cache_write_buffers}"
mem_size="${OPENURMA_DUAL_MEM_SIZE:-$profile_mem_size}"
guest_mem_limit="${OPENURMA_GUEST_MEM_LIMIT:-$profile_guest_mem_limit}"
mem_type="${OPENURMA_MEM_TYPE:-$profile_mem_type}"
mem_channels="${OPENURMA_MEM_CHANNELS:-$profile_mem_channels}"
mem_channels_intlv="${OPENURMA_MEM_CHANNELS_INTLV:-$profile_mem_channels_intlv}"
mem_addr_mapping="${OPENURMA_MEM_ADDR_MAPPING:-$profile_mem_addr_mapping}"
mem_channel_xor_low_bit="${OPENURMA_MEM_CHANNEL_XOR_LOW_BIT:-$profile_mem_channel_xor_low_bit}"
mem_ranks="${OPENURMA_MEM_RANKS:-$profile_mem_ranks}"
mem_read_buffer_size="${OPENURMA_MEM_READ_BUFFER_SIZE:-$profile_mem_read_buffer_size}"
mem_write_buffer_size="${OPENURMA_MEM_WRITE_BUFFER_SIZE:-$profile_mem_write_buffer_size}"
mem_page_policy="${OPENURMA_MEM_PAGE_POLICY:-$profile_mem_page_policy}"
mem_max_accesses_per_row="${OPENURMA_MEM_MAX_ACCESSES_PER_ROW:-$profile_mem_max_accesses_per_row}"
mem_sched_policy="${OPENURMA_MEM_SCHED_POLICY:-$profile_mem_sched_policy}"
mem_write_high_thresh="${OPENURMA_MEM_WRITE_HIGH_THRESH:-$profile_mem_write_high_thresh}"
mem_write_low_thresh="${OPENURMA_MEM_WRITE_LOW_THRESH:-$profile_mem_write_low_thresh}"
mem_min_writes_per_switch="${OPENURMA_MEM_MIN_WRITES_PER_SWITCH:-$profile_mem_min_writes_per_switch}"
mem_min_reads_per_switch="${OPENURMA_MEM_MIN_READS_PER_SWITCH:-$profile_mem_min_reads_per_switch}"
mem_ctrl_frontend_latency="${OPENURMA_MEM_CTRL_FRONTEND_LATENCY:-$profile_mem_ctrl_frontend_latency}"
mem_ctrl_backend_latency="${OPENURMA_MEM_CTRL_BACKEND_LATENCY:-$profile_mem_ctrl_backend_latency}"
mem_ctrl_command_window="${OPENURMA_MEM_CTRL_COMMAND_WINDOW:-$profile_mem_ctrl_command_window}"
ub_port_count="${OPENURMA_UB_PORT_COUNT:-$profile_ub_port_count}"
peer_latency_ns="${OPENURMA_PEER_LATENCY_NS:-100}"
sync_quantum_ns="${OPENURMA_SYNC_QUANTUM_NS:-$peer_latency_ns}"
peer_link_rate_gbps="${OPENURMA_PEER_LINK_RATE_GBPS:-$profile_peer_link_rate_gbps}"
peer_serialization_stages="${OPENURMA_PEER_SERIALIZATION_STAGES:-$profile_peer_serialization_stages}"
peer_switch_delay="${OPENURMA_PEER_SWITCH_DELAY:-$profile_peer_switch_delay}"
peer_link_overhead_bytes="${OPENURMA_PEER_LINK_OVERHEAD_BYTES:-$profile_peer_link_overhead_bytes}"
sq_control_bytes="${OPENURMA_SQ_CONTROL_BYTES:-$profile_sq_control_bytes}"
wqebb_bytes="${OPENURMA_WQEBB_BYTES:-$profile_wqebb_bytes}"
sq_sge_bytes="${OPENURMA_SQ_SGE_BYTES:-$profile_sq_sge_bytes}"
direct_wqe_max_blocks="${OPENURMA_DIRECT_WQE_MAX_BLOCKS:-$profile_direct_wqe_max_blocks}"
direct_wqe_latency="${OPENURMA_DIRECT_WQE_LATENCY:-$profile_direct_wqe_latency}"
sq_fetch_latency="${OPENURMA_SQ_FETCH_LATENCY:-$profile_sq_fetch_latency}"
sq_wqebb_latency="${OPENURMA_SQ_WQEBB_LATENCY:-$profile_sq_wqebb_latency}"
payload_dma_latency="${OPENURMA_PAYLOAD_DMA_LATENCY:-$profile_payload_dma_latency}"
payload_dma_rate_gbps="${OPENURMA_PAYLOAD_DMA_RATE_GBPS:-$profile_payload_dma_rate_gbps}"
udma_poll_interval="${OPENURMA_UDMA_POLL_INTERVAL:-$profile_udma_poll_interval}"
udma_iotlb_entries="${OPENURMA_UDMA_IOTLB_ENTRIES:-$profile_udma_iotlb_entries}"
dma_max_outstanding="${OPENURMA_DMA_MAX_OUTSTANDING:-$profile_dma_max_outstanding}"
provider="${OPENURMA_PROVIDER:-$profile_provider}"

[[ -n "$cli_cpu_mode" ]] && cpu_mode=$cli_cpu_mode
[[ -n "$cli_m5ops_base" ]] && m5ops_base=$cli_m5ops_base
[[ -n "$cli_cpu_freq" ]] && cpu_freq=$cli_cpu_freq
[[ -n "$cli_num_cpus" ]] && num_cpus=$cli_num_cpus
[[ -n "$cli_benchmark_cpu" ]] && benchmark_cpu=$cli_benchmark_cpu
[[ -n "$cli_o3_width" ]] && o3_width=$cli_o3_width
[[ -n "$cli_o3_rob_entries" ]] && o3_rob_entries=$cli_o3_rob_entries
[[ -n "$cli_o3_iq_entries" ]] && o3_iq_entries=$cli_o3_iq_entries
[[ -n "$cli_o3_lq_entries" ]] && o3_lq_entries=$cli_o3_lq_entries
[[ -n "$cli_o3_sq_entries" ]] && o3_sq_entries=$cli_o3_sq_entries
[[ -n "$cli_o3_load_ports" ]] && o3_load_ports=$cli_o3_load_ports
[[ -n "$cli_o3_store_ports" ]] && o3_store_ports=$cli_o3_store_ports
[[ -n "$cli_o3_fetch_buffer_bytes" ]] && o3_fetch_buffer_bytes=$cli_o3_fetch_buffer_bytes
[[ -n "$cli_o3_fetch_queue_entries" ]] && o3_fetch_queue_entries=$cli_o3_fetch_queue_entries
[[ -n "$cli_o3_phys_int_regs" ]] && o3_phys_int_regs=$cli_o3_phys_int_regs
[[ -n "$cli_o3_phys_float_regs" ]] && o3_phys_float_regs=$cli_o3_phys_float_regs
[[ -n "$cli_o3_phys_vec_regs" ]] && o3_phys_vec_regs=$cli_o3_phys_vec_regs
[[ -n "$cli_o3_phys_vec_pred_regs" ]] && o3_phys_vec_pred_regs=$cli_o3_phys_vec_pred_regs
[[ -n "$cli_o3_phys_mat_regs" ]] && o3_phys_mat_regs=$cli_o3_phys_mat_regs
[[ -n "$cli_cache_line_size" ]] && cache_line_size=$cli_cache_line_size
[[ -n "$cli_last_cache_level" ]] && last_cache_level=$cli_last_cache_level
[[ -n "$cli_l1i_size" ]] && l1i_size=$cli_l1i_size
[[ -n "$cli_l1i_assoc" ]] && l1i_assoc=$cli_l1i_assoc
[[ -n "$cli_l1i_latency" ]] && l1i_latency=$cli_l1i_latency
[[ -n "$cli_l1i_mshrs" ]] && l1i_mshrs=$cli_l1i_mshrs
[[ -n "$cli_l1i_targets" ]] && l1i_targets=$cli_l1i_targets
[[ -n "$cli_l1i_write_buffers" ]] && l1i_write_buffers=$cli_l1i_write_buffers
[[ -n "$cli_l1d_size" ]] && l1d_size=$cli_l1d_size
[[ -n "$cli_l1d_assoc" ]] && l1d_assoc=$cli_l1d_assoc
[[ -n "$cli_l1d_latency" ]] && l1d_latency=$cli_l1d_latency
[[ -n "$cli_l1d_mshrs" ]] && l1d_mshrs=$cli_l1d_mshrs
[[ -n "$cli_l1d_targets" ]] && l1d_targets=$cli_l1d_targets
[[ -n "$cli_l1d_write_buffers" ]] && l1d_write_buffers=$cli_l1d_write_buffers
[[ -n "$cli_l2_size" ]] && l2_size=$cli_l2_size
[[ -n "$cli_l2_assoc" ]] && l2_assoc=$cli_l2_assoc
[[ -n "$cli_l2_latency" ]] && l2_latency=$cli_l2_latency
[[ -n "$cli_l2_mshrs" ]] && l2_mshrs=$cli_l2_mshrs
[[ -n "$cli_l2_targets" ]] && l2_targets=$cli_l2_targets
[[ -n "$cli_l2_write_buffers" ]] && l2_write_buffers=$cli_l2_write_buffers
[[ -n "$cli_l3_size" ]] && l3_size=$cli_l3_size
[[ -n "$cli_l3_assoc" ]] && l3_assoc=$cli_l3_assoc
[[ -n "$cli_l3_latency" ]] && l3_latency=$cli_l3_latency
[[ -n "$cli_l3_mshrs" ]] && l3_mshrs=$cli_l3_mshrs
[[ -n "$cli_l3_targets" ]] && l3_targets=$cli_l3_targets
[[ -n "$cli_l3_write_buffers" ]] && l3_write_buffers=$cli_l3_write_buffers
[[ -n "$cli_fabric_freq" ]] && fabric_freq=$cli_fabric_freq
[[ -n "$cli_fabric_width_bytes" ]] && fabric_width_bytes=$cli_fabric_width_bytes
[[ -n "$cli_coherent_bus_frontend_latency" ]] && coherent_bus_frontend_latency=$cli_coherent_bus_frontend_latency
[[ -n "$cli_coherent_bus_forward_latency" ]] && coherent_bus_forward_latency=$cli_coherent_bus_forward_latency
[[ -n "$cli_coherent_bus_response_latency" ]] && coherent_bus_response_latency=$cli_coherent_bus_response_latency
[[ -n "$cli_coherent_bus_snoop_response_latency" ]] && coherent_bus_snoop_response_latency=$cli_coherent_bus_snoop_response_latency
[[ -n "$cli_coherent_bus_header_latency" ]] && coherent_bus_header_latency=$cli_coherent_bus_header_latency
[[ -n "$cli_memory_bus_frontend_latency" ]] && memory_bus_frontend_latency=$cli_memory_bus_frontend_latency
[[ -n "$cli_memory_bus_forward_latency" ]] && memory_bus_forward_latency=$cli_memory_bus_forward_latency
[[ -n "$cli_memory_bus_response_latency" ]] && memory_bus_response_latency=$cli_memory_bus_response_latency
[[ -n "$cli_memory_bus_snoop_response_latency" ]] && memory_bus_snoop_response_latency=$cli_memory_bus_snoop_response_latency
[[ -n "$cli_memory_bus_header_latency" ]] && memory_bus_header_latency=$cli_memory_bus_header_latency
[[ -n "$cli_core_bus_snoop_filter_capacity" ]] && core_bus_snoop_filter_capacity=$cli_core_bus_snoop_filter_capacity
[[ -n "$cli_l3_bus_snoop_filter_capacity" ]] && l3_bus_snoop_filter_capacity=$cli_l3_bus_snoop_filter_capacity
[[ -n "$cli_membus_snoop_filter_capacity" ]] && membus_snoop_filter_capacity=$cli_membus_snoop_filter_capacity
[[ -n "$cli_io_bus_frontend_latency" ]] && io_bus_frontend_latency=$cli_io_bus_frontend_latency
[[ -n "$cli_io_bus_forward_latency" ]] && io_bus_forward_latency=$cli_io_bus_forward_latency
[[ -n "$cli_io_bus_response_latency" ]] && io_bus_response_latency=$cli_io_bus_response_latency
[[ -n "$cli_io_bus_header_latency" ]] && io_bus_header_latency=$cli_io_bus_header_latency
[[ -n "$cli_io_cache_size" ]] && io_cache_size=$cli_io_cache_size
[[ -n "$cli_io_cache_assoc" ]] && io_cache_assoc=$cli_io_cache_assoc
[[ -n "$cli_io_cache_latency" ]] && io_cache_latency=$cli_io_cache_latency
[[ -n "$cli_io_cache_mshrs" ]] && io_cache_mshrs=$cli_io_cache_mshrs
[[ -n "$cli_io_cache_targets" ]] && io_cache_targets=$cli_io_cache_targets
[[ -n "$cli_io_cache_write_buffers" ]] && io_cache_write_buffers=$cli_io_cache_write_buffers
[[ -n "$cli_mem_size" ]] && mem_size=$cli_mem_size
[[ -n "$cli_guest_mem_limit" ]] && guest_mem_limit=$cli_guest_mem_limit
[[ -n "$cli_mem_type" ]] && mem_type=$cli_mem_type
[[ -n "$cli_mem_channels" ]] && mem_channels=$cli_mem_channels
[[ -n "$cli_mem_channels_intlv" ]] && mem_channels_intlv=$cli_mem_channels_intlv
[[ -n "$cli_mem_addr_mapping" ]] && mem_addr_mapping=$cli_mem_addr_mapping
[[ -n "$cli_mem_channel_xor_low_bit" ]] && mem_channel_xor_low_bit=$cli_mem_channel_xor_low_bit
[[ -n "$cli_mem_ranks" ]] && mem_ranks=$cli_mem_ranks
[[ -n "$cli_mem_read_buffer_size" ]] && mem_read_buffer_size=$cli_mem_read_buffer_size
[[ -n "$cli_mem_write_buffer_size" ]] && mem_write_buffer_size=$cli_mem_write_buffer_size
[[ -n "$cli_mem_page_policy" ]] && mem_page_policy=$cli_mem_page_policy
[[ -n "$cli_mem_max_accesses_per_row" ]] && mem_max_accesses_per_row=$cli_mem_max_accesses_per_row
[[ -n "$cli_mem_sched_policy" ]] && mem_sched_policy=$cli_mem_sched_policy
[[ -n "$cli_mem_write_high_thresh" ]] && mem_write_high_thresh=$cli_mem_write_high_thresh
[[ -n "$cli_mem_write_low_thresh" ]] && mem_write_low_thresh=$cli_mem_write_low_thresh
[[ -n "$cli_mem_min_writes_per_switch" ]] && mem_min_writes_per_switch=$cli_mem_min_writes_per_switch
[[ -n "$cli_mem_min_reads_per_switch" ]] && mem_min_reads_per_switch=$cli_mem_min_reads_per_switch
[[ -n "$cli_mem_ctrl_frontend_latency" ]] && mem_ctrl_frontend_latency=$cli_mem_ctrl_frontend_latency
[[ -n "$cli_mem_ctrl_backend_latency" ]] && mem_ctrl_backend_latency=$cli_mem_ctrl_backend_latency
[[ -n "$cli_mem_ctrl_command_window" ]] && mem_ctrl_command_window=$cli_mem_ctrl_command_window
[[ -n "$cli_ub_port_count" ]] && ub_port_count=$cli_ub_port_count
[[ -n "$cli_peer_latency_ns" ]] && peer_latency_ns=$cli_peer_latency_ns
[[ -n "$cli_sync_quantum_ns" ]] && sync_quantum_ns=$cli_sync_quantum_ns
[[ -n "$cli_peer_link_rate_gbps" ]] && peer_link_rate_gbps=$cli_peer_link_rate_gbps
[[ -n "$cli_peer_serialization_stages" ]] && peer_serialization_stages=$cli_peer_serialization_stages
[[ -n "$cli_peer_switch_delay" ]] && peer_switch_delay=$cli_peer_switch_delay
[[ -n "$cli_peer_link_overhead_bytes" ]] && peer_link_overhead_bytes=$cli_peer_link_overhead_bytes
[[ -n "$cli_sq_control_bytes" ]] && sq_control_bytes=$cli_sq_control_bytes
[[ -n "$cli_wqebb_bytes" ]] && wqebb_bytes=$cli_wqebb_bytes
[[ -n "$cli_sq_sge_bytes" ]] && sq_sge_bytes=$cli_sq_sge_bytes
[[ -n "$cli_direct_wqe_max_blocks" ]] && direct_wqe_max_blocks=$cli_direct_wqe_max_blocks
[[ -n "$cli_direct_wqe_latency" ]] && direct_wqe_latency=$cli_direct_wqe_latency
[[ -n "$cli_sq_fetch_latency" ]] && sq_fetch_latency=$cli_sq_fetch_latency
[[ -n "$cli_sq_wqebb_latency" ]] && sq_wqebb_latency=$cli_sq_wqebb_latency
[[ -n "$cli_payload_dma_latency" ]] && payload_dma_latency=$cli_payload_dma_latency
[[ -n "$cli_payload_dma_rate_gbps" ]] && payload_dma_rate_gbps=$cli_payload_dma_rate_gbps
[[ -n "$cli_udma_poll_interval" ]] && udma_poll_interval=$cli_udma_poll_interval
[[ -n "$cli_udma_iotlb_entries" ]] && udma_iotlb_entries=$cli_udma_iotlb_entries
[[ -n "$cli_dma_max_outstanding" ]] && dma_max_outstanding=$cli_dma_max_outstanding
[[ -n "$cli_provider" ]] && provider=$cli_provider

if [[ "$provider" == official ]]; then
    default_dma_backend=udma
else
    default_dma_backend=$provider
fi
dma_backend="${OPENURMA_DMA_BACKEND:-$default_dma_backend}"

# These labels describe executable simulator mechanisms, not latency-fit
# inputs.  Every non-legacy DMA operation enters the native gem5 RequestPort;
# the port chooses atomic or timing requests from the System's current memory
# mode.  UDMA virtual addresses are rooted in the creating process's page
# table, carried by the versioned simulation control ABI.
if [ "$dma_backend" = legacy ]; then
    dma_transport=legacy_functional
else
    dma_transport=native_gem5_request_port_dynamic
fi
udma_address_translation=context_pgd_control_abi_v2
dma_request_segmentation=cacheline_and_4KiB_boundaries
udma_iotlb_policy=fully_associative_lru_4KiB_context_tagged
if [[ "$cpu_mode" == server_o3 ]]; then
    cpu_switch_policy=send_lat_roi_enter_o3_exit_atomic
    cpu_boot_model=AtomicSimpleCPU
    cpu_roi_model=ArmO3CPU
    initial_memory_mode=atomic
    online_cpu_count=$num_cpus
    cpu_event_queue_policy=single_event_queue
    m5ops_mode=inst
else
    cpu_switch_policy=none
    case "$cpu_mode" in
        atomic|atomic_hot|atomic_fast|atomic_cache) cpu_model=AtomicSimpleCPU ;;
        timing|timing_nocache|timing_full) cpu_model=TimingSimpleCPU ;;
        o3) cpu_model=ArmO3CPU ;;
        *) die "invalid CPU mode '$cpu_mode'" ;;
    esac
    cpu_boot_model=$cpu_model
    cpu_roi_model=$cpu_model
    case "$cpu_mode" in
        timing*|o3) initial_memory_mode=timing ;;
        *) initial_memory_mode=atomic ;;
    esac
    online_cpu_count=$num_cpus
    cpu_event_queue_policy=single_event_queue
    m5ops_mode=inst
fi

lab_host="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
container="${OPENURMA_CONTAINER:-openurma-repro-20260909}"
lab="${OPENURMA_LAB_ROOT:-/workspace/openurma-gem5-lab}"
gem5="${OPENURMA_GEM5:-$lab/gem5/build/ARM/gem5.opt}"
m5_path="${OPENURMA_M5_PATH:-$lab/system}"
kernel="${OPENURMA_KERNEL:-$lab/artifacts/kernel/vmlinux}"
if [[ "$provider" == official ]]; then
    default_initrd="$lab/out/official-udma.cpio.gz"
else
    default_initrd="$lab/out/openurma-interactive.cpio.gz"
fi
initrd="${OPENURMA_INITRD:-$default_initrd}"
config="${OPENURMA_CONFIG:-$lab/configs/single_node_fs_openurma.py}"
switch_config="${OPENURMA_SWITCH_CONFIG:-$lab/gem5/configs/dist/sw.py}"
run_root="${OPENURMA_DUAL_OUT:-$lab/run-dual}"
ring="${OPENURMA_DUAL_RING:-/tmp/openurma-dual.peer-ring}"
tap0="${OPENURMA_DUAL_TAP0:-/tmp/openurma-dual.eth0.sock}"
tap1="${OPENURMA_DUAL_TAP1:-/tmp/openurma-dual.eth1.sock}"
uart0="${OPENURMA_DUAL_UART0:-3460}"
uart1="${OPENURMA_DUAL_UART1:-3470}"
pipe_data="${OPENURMA_PIPE_DATA:-0}"
packet_trace="${OPENURMA_TRACE_PACKETS:-0}"
dist_port="${OPENURMA_DIST_PORT:-2200}"
dist_link_speed="${OPENURMA_DIST_LINK_SPEED:-${peer_link_rate_gbps}Gbps}"
oob_link_speed="${OPENURMA_OOB_LINK_SPEED:-100Gbps}"

case "$uart0:$uart1:$dist_port:$ub_port_count:$peer_latency_ns:$sync_quantum_ns:$peer_link_rate_gbps:$peer_serialization_stages:$peer_link_overhead_bytes:$sq_control_bytes:$wqebb_bytes:$sq_sge_bytes:$direct_wqe_max_blocks:$payload_dma_rate_gbps:$dma_max_outstanding:$udma_iotlb_entries:$num_cpus:$o3_width:$o3_rob_entries:$o3_iq_entries:$o3_lq_entries:$o3_sq_entries:$o3_load_ports:$o3_store_ports:$o3_fetch_buffer_bytes:$o3_fetch_queue_entries:$o3_phys_int_regs:$o3_phys_float_regs:$o3_phys_vec_regs:$o3_phys_vec_pred_regs:$o3_phys_mat_regs:$cache_line_size:$last_cache_level:$l1i_assoc:$l1i_mshrs:$l1i_targets:$l1i_write_buffers:$l1d_assoc:$l1d_mshrs:$l1d_targets:$l1d_write_buffers:$l2_assoc:$l2_mshrs:$l2_targets:$l2_write_buffers:$l3_assoc:$l3_mshrs:$l3_targets:$l3_write_buffers:$fabric_width_bytes:$coherent_bus_frontend_latency:$coherent_bus_forward_latency:$coherent_bus_response_latency:$coherent_bus_header_latency:$io_bus_frontend_latency:$io_bus_forward_latency:$io_bus_response_latency:$io_bus_header_latency:$io_cache_assoc:$io_cache_mshrs:$io_cache_targets:$io_cache_write_buffers:$mem_channels:$mem_channels_intlv:$mem_channel_xor_low_bit:$mem_ranks:$mem_read_buffer_size:$mem_write_buffer_size:$mem_max_accesses_per_row:$mem_write_high_thresh:$mem_write_low_thresh:$mem_min_writes_per_switch:$mem_min_reads_per_switch" in
    *[!0-9:]*) die "ports, nanosecond values, rates, stages, and byte counts must be decimal integers" ;;
esac
case "$coherent_bus_snoop_response_latency:$memory_bus_frontend_latency:$memory_bus_forward_latency:$memory_bus_response_latency:$memory_bus_snoop_response_latency:$memory_bus_header_latency" in
    *[!0-9:]*) die "coherent and memory bus latencies must be decimal integers" ;;
esac
case "$benchmark_cpu" in
    -1|[0-9]|[0-9][0-9]*) ;;
    *) die "benchmark CPU must be -1 or a non-negative decimal integer" ;;
esac
case "$cpu_mode" in
    atomic|atomic_hot|atomic_fast|atomic_cache|timing|timing_nocache|timing_full|o3|server_o3) ;;
    *) die "invalid CPU mode '$cpu_mode'" ;;
esac
[[ "$m5ops_base" =~ ^0[xX][0-9a-fA-F]+$ ]] ||
    die "m5ops base must be a hexadecimal physical address (for example 0x10010000)"
(( m5ops_base > 0 && (m5ops_base & 65535) == 0 )) ||
    die "m5ops base must be non-zero and 64-KiB aligned"
(( m5ops_base == 0x10010000 )) ||
    die "VExpress_GEM5_V1 reserves m5ops at 0x10010000; another address needs another platform memory map"
case "$provider" in
    legacy|udma|official) ;;
    *) die "invalid provider '$provider'; expected legacy, udma, or official" ;;
esac
case "$dma_backend" in
    legacy|timing|udma) ;;
    *) die "invalid DMA backend '$dma_backend'; expected legacy, timing, or udma" ;;
esac
case "$cpu_freq" in
    ""|*[!0-9A-Za-z.+_-]*) die "invalid CPU frequency '$cpu_freq'" ;;
esac
case "$fabric_freq" in
    ""|*[!0-9A-Za-z.+_-]*) die "invalid fabric frequency '$fabric_freq'" ;;
esac
case "$mem_type" in
    ""|*[!0-9A-Za-z_+-]*) die "invalid memory type '$mem_type'" ;;
esac
case "$mem_page_policy" in
    open|close|open_adaptive|close_adaptive) ;;
    *) die "invalid memory page policy '$mem_page_policy'" ;;
esac
case "$mem_sched_policy" in
    fcfs|frfcfs) ;;
    *) die "invalid memory scheduling policy '$mem_sched_policy'" ;;
esac
case "$mem_addr_mapping" in
    RoRaBaChCo|RoRaBaCoCh|RoCoRaBaCh) ;;
    *) die "invalid DRAM address mapping '$mem_addr_mapping'" ;;
esac
valid_cache_size() {
    [[ "$2" =~ ^[0-9]+(B|kB|KiB|MB|MiB|GB|GiB)$ ]] ||
        die "$1 must be a positive gem5 capacity (for example 64kB or 1MB): $2"
}
valid_cache_latency() {
    [[ "$2" =~ ^[1-9][0-9]*,[1-9][0-9]*,[1-9][0-9]*$ ]] ||
        die "$1 must be TAG,DATA,RESPONSE positive cycle counts: $2"
}
valid_cache_size l1i-size "$l1i_size"
valid_cache_size l1d-size "$l1d_size"
valid_cache_size l2-size "$l2_size"
valid_cache_size l3-size "$l3_size"
valid_cache_size core-bus-snoop-filter-capacity "$core_bus_snoop_filter_capacity"
valid_cache_size l3-bus-snoop-filter-capacity "$l3_bus_snoop_filter_capacity"
valid_cache_size io-cache-size "$io_cache_size"
valid_cache_size membus-snoop-filter-capacity "$membus_snoop_filter_capacity"
valid_cache_size memory-size "$mem_size"
valid_cache_size guest-memory-limit "$guest_mem_limit"
valid_cache_latency l1i-latency "$l1i_latency"
valid_cache_latency l1d-latency "$l1d_latency"
valid_cache_latency l2-latency "$l2_latency"
valid_cache_latency l3-latency "$l3_latency"
valid_cache_latency io-cache-latency "$io_cache_latency"
valid_time() {
    [[ "$2" =~ ^[0-9]+([.][0-9]+)?(ps|ns|us|ms|s)$ ]] ||
        die "$1 must be a non-negative gem5 time (for example 0ns or 1us): $2"
}
valid_time peer-switch-delay "$peer_switch_delay"
valid_time direct-wqe-latency "$direct_wqe_latency"
valid_time sq-fetch-latency "$sq_fetch_latency"
valid_time sq-wqebb-latency "$sq_wqebb_latency"
valid_time payload-dma-latency "$payload_dma_latency"
valid_time udma-poll-interval "$udma_poll_interval"
valid_time mem-ctrl-frontend-latency "$mem_ctrl_frontend_latency"
valid_time mem-ctrl-backend-latency "$mem_ctrl_backend_latency"
valid_time mem-ctrl-command-window "$mem_ctrl_command_window"
case "$mem_ctrl_command_window" in
    0ps|0ns|0us|0ms|0s|0.0ps|0.0ns|0.0us|0.0ms|0.0s)
        die "mem-ctrl-command-window must be positive"
        ;;
esac
case "$udma_poll_interval" in
    0ps|0ns|0us|0ms|0s|0.0ps|0.0ns|0.0us|0.0ms|0.0s)
        die "udma-poll-interval must be positive"
        ;;
esac
(( uart0 >= 1024 && uart0 <= 65531 )) || die "invalid node0 UART base: $uart0"
(( uart1 >= 1024 && uart1 <= 65531 )) || die "invalid node1 UART base: $uart1"
(( uart0 + 3 < uart1 || uart1 + 3 < uart0 )) ||
    die "UART ranges overlap: $uart0-$((uart0 + 3)) and $uart1-$((uart1 + 3))"
(( dist_port >= 1024 && dist_port <= 65535 )) || die "invalid dist switch port: $dist_port"
(( peer_latency_ns > 0 )) || die "peer lookahead must be positive"
(( sync_quantum_ns > 0 && sync_quantum_ns <= peer_latency_ns )) ||
    die "sync quantum must satisfy 0 < quantum <= peer latency"
(( peer_link_rate_gbps > 0 )) || die "peer link rate must be positive"
(( num_cpus > 0 )) || die "number of CPUs must be positive"
(( benchmark_cpu < num_cpus )) || die "benchmark CPU must be smaller than the CPU count"
(( o3_width > 0 && o3_width <= 12 && o3_rob_entries > 0 && o3_iq_entries > 0 )) ||
    die "O3 width and queue sizes must be positive"
(( o3_lq_entries > 0 && o3_sq_entries > 0 )) || die "O3 LSQ sizes must be positive"
(( o3_load_ports > 0 && o3_store_ports > 0 )) ||
    die "O3 cache port counts must be positive"
(( o3_fetch_buffer_bytes > 0 && o3_fetch_buffer_bytes <= cache_line_size &&
   cache_line_size % o3_fetch_buffer_bytes == 0 )) ||
    die "O3 fetch buffer must divide and not exceed the cache line"
(( o3_fetch_queue_entries > 0 )) || die "O3 fetch queue must be positive"
(( o3_phys_int_regs > 42 && o3_phys_float_regs > 0 &&
   o3_phys_vec_regs > 44 && o3_phys_vec_pred_regs > 18 &&
   o3_phys_mat_regs > 1 )) ||
    die "O3 physical register files are smaller than ARM architectural minima"
(( last_cache_level >= 1 && last_cache_level <= 3 )) ||
    die "last cache level must be 1, 2, or 3"
(( cache_line_size > 0 && (cache_line_size & (cache_line_size - 1)) == 0 )) ||
    die "cache line size must be a power of two"
(( l1i_assoc > 0 && l1d_assoc > 0 && l2_assoc > 0 && l3_assoc > 0 )) ||
    die "cache associativities must be positive"
(( l1i_mshrs > 0 && l1i_targets > 0 && l1i_write_buffers > 0 &&
   l1d_mshrs > 0 && l1d_targets > 0 && l1d_write_buffers > 0 &&
   l2_mshrs > 0 && l2_targets > 0 && l2_write_buffers > 0 &&
   l3_mshrs > 0 && l3_targets > 0 && l3_write_buffers > 0 )) ||
    die "all CPU-cache MSHR, target and write-buffer counts must be positive"
(( fabric_width_bytes > 0 )) || die "fabric width must be positive"
(( coherent_bus_frontend_latency >= 0 && coherent_bus_forward_latency >= 0 &&
   coherent_bus_response_latency >= 0 &&
   coherent_bus_snoop_response_latency >= 0 &&
   coherent_bus_header_latency >= 0 )) ||
    die "coherent bus latencies must be non-negative"
(( memory_bus_frontend_latency >= 0 && memory_bus_forward_latency >= 0 &&
   memory_bus_response_latency >= 0 &&
   memory_bus_snoop_response_latency >= 0 &&
   memory_bus_header_latency >= 0 )) ||
    die "memory bus latencies must be non-negative"
(( io_cache_assoc > 0 )) || die "I/O cache associativity must be positive"
(( io_cache_mshrs >= dma_max_outstanding )) ||
    die "I/O cache MSHRs must be at least DMA max outstanding"
(( io_cache_targets >= dma_max_outstanding )) ||
    die "I/O cache targets per MSHR must be at least DMA max outstanding"
(( io_cache_write_buffers > 0 )) || die "I/O cache write buffers must be positive"
(( mem_channels > 0 && (mem_channels & (mem_channels - 1)) == 0 )) ||
    die "memory channels must be a positive power of two"
(( mem_channels_intlv > 0 && (mem_channels_intlv & (mem_channels_intlv - 1)) == 0 )) ||
    die "memory channel interleave must be a positive power of two"
(( mem_channels_intlv >= cache_line_size )) ||
    die "memory channel interleave must be at least one cache line"
(( mem_ranks > 0 && (mem_ranks & (mem_ranks - 1)) == 0 )) ||
    die "memory ranks must be a positive power of two"
(( mem_read_buffer_size > 0 && mem_write_buffer_size > 0 )) ||
    die "memory burst queues must be positive"
(( mem_max_accesses_per_row > 0 )) ||
    die "memory max accesses per row must be positive"
(( mem_write_low_thresh >= 0 && mem_write_high_thresh <= 100 &&
   mem_write_low_thresh < mem_write_high_thresh )) ||
    die "memory write thresholds must satisfy 0 <= low < high <= 100"
(( mem_min_writes_per_switch > 0 && mem_min_reads_per_switch > 0 )) ||
    die "memory read/write switch burst counts must be positive"
(( ub_port_count > 0 && ub_port_count <= 16 )) ||
    die "UB port count must be between 1 and 16"
(( wqebb_bytes > 0 )) || die "WQEBB bytes must be positive"
(( sq_sge_bytes > 0 )) || die "SQ SGE bytes must be positive"
(( udma_iotlb_entries >= 0 )) || die "UDMA IOTLB entries must be non-negative"
(( dma_max_outstanding > 0 )) || die "DMA max outstanding must be positive"

# PeerRing has a fixed 4-KiB index header plus one 64 x 8-KiB queue for
# every direction and physical port. Keep this formula synchronized with
# NICTopologySC::PeerRing and its slot-offset calculation.
peer_ring_bytes=$((4096 + 2 * ub_port_count * 64 * 8192))

print_resolved_config() {
    cat <<EOF
manifest_version=1
profile=$profile
profile_revision=$profile_revision
provider=$provider
dma_backend=$dma_backend
dma_transport=$dma_transport
udma_address_translation=$udma_address_translation
dma_request_segmentation=$dma_request_segmentation
udma_iotlb_policy=$udma_iotlb_policy
cpu_mode=$cpu_mode
m5ops_mode=$m5ops_mode
m5ops_base=$m5ops_base
initial_memory_mode=$initial_memory_mode
cpu_freq=$cpu_freq
cpu_boot_model=$cpu_boot_model
cpu_roi_model=$cpu_roi_model
cpu_switch_policy=$cpu_switch_policy
cpu_count=$num_cpus
online_cpu_count=$online_cpu_count
cpu_event_queue_policy=$cpu_event_queue_policy
benchmark_cpu=$benchmark_cpu
o3_width=$o3_width
o3_rob_entries=$o3_rob_entries
o3_iq_entries=$o3_iq_entries
o3_lq_entries=$o3_lq_entries
o3_sq_entries=$o3_sq_entries
o3_load_ports=$o3_load_ports
o3_store_ports=$o3_store_ports
o3_fetch_buffer_bytes=$o3_fetch_buffer_bytes
o3_fetch_queue_entries=$o3_fetch_queue_entries
o3_phys_int_regs=$o3_phys_int_regs
o3_phys_float_regs=$o3_phys_float_regs
o3_phys_vec_regs=$o3_phys_vec_regs
o3_phys_vec_pred_regs=$o3_phys_vec_pred_regs
o3_phys_mat_regs=$o3_phys_mat_regs
o3_phys_cc_regs=$((o3_phys_int_regs * 5))
o3_branch_predictor=TournamentBP
o3_fu_pool=DefaultFUPool
o3_fu_pool_signature=int_alu6,int_muldiv2,fp_alu4,fp_muldiv2,simd4,pred1,rdwr4,ipr1
cache_line_size=$cache_line_size
last_cache_level=$last_cache_level
cache_prefetcher=none
cache_replacement_policy=lru
l1i_size=$l1i_size
l1i_assoc=$l1i_assoc
l1i_latency_tag_data_response_cycles=$l1i_latency
l1i_mshrs=$l1i_mshrs
l1i_targets_per_mshr=$l1i_targets
l1i_write_buffers=$l1i_write_buffers
l1i_clusivity=mostly_incl
l1d_size=$l1d_size
l1d_assoc=$l1d_assoc
l1d_latency_tag_data_response_cycles=$l1d_latency
l1d_mshrs=$l1d_mshrs
l1d_targets_per_mshr=$l1d_targets
l1d_write_buffers=$l1d_write_buffers
l1d_clusivity=mostly_incl
l2_size=$l2_size
l2_assoc=$l2_assoc
l2_latency_tag_data_response_cycles=$l2_latency
l2_mshrs=$l2_mshrs
l2_targets_per_mshr=$l2_targets
l2_write_buffers=$l2_write_buffers
l2_clusivity=mostly_excl
l3_size=$l3_size
l3_assoc=$l3_assoc
l3_latency_tag_data_response_cycles=$l3_latency
l3_mshrs=$l3_mshrs
l3_targets_per_mshr=$l3_targets
l3_write_buffers=$l3_write_buffers
l3_clusivity=mostly_excl
l3_freq=$fabric_freq
fabric_freq=$fabric_freq
fabric_width_bytes=$fabric_width_bytes
coherent_bus_frontend_latency_cycles=$coherent_bus_frontend_latency
coherent_bus_forward_latency_cycles=$coherent_bus_forward_latency
coherent_bus_response_latency_cycles=$coherent_bus_response_latency
coherent_bus_snoop_response_latency_cycles=$coherent_bus_snoop_response_latency
coherent_bus_header_latency_cycles=$coherent_bus_header_latency
memory_bus_frontend_latency_cycles=$memory_bus_frontend_latency
memory_bus_forward_latency_cycles=$memory_bus_forward_latency
memory_bus_response_latency_cycles=$memory_bus_response_latency
memory_bus_snoop_response_latency_cycles=$memory_bus_snoop_response_latency
memory_bus_header_latency_cycles=$memory_bus_header_latency
core_bus_snoop_filter_capacity=$core_bus_snoop_filter_capacity
l3_bus_snoop_filter_capacity=$l3_bus_snoop_filter_capacity
membus_snoop_filter_capacity=$membus_snoop_filter_capacity
io_bus_frontend_latency_cycles=$io_bus_frontend_latency
io_bus_forward_latency_cycles=$io_bus_forward_latency
io_bus_response_latency_cycles=$io_bus_response_latency
io_bus_header_latency_cycles=$io_bus_header_latency
io_cache_size=$io_cache_size
io_cache_assoc=$io_cache_assoc
io_cache_latency_tag_data_response_cycles=$io_cache_latency
io_cache_mshrs=$io_cache_mshrs
io_cache_targets_per_mshr=$io_cache_targets
io_cache_write_buffers=$io_cache_write_buffers
memory_size=$mem_size
guest_memory_limit=$guest_mem_limit
memory_type=$mem_type
memory_channels=$mem_channels
memory_channel_interleave_bytes=$mem_channels_intlv
memory_address_mapping=$mem_addr_mapping
memory_channel_xor_low_bit=$mem_channel_xor_low_bit
memory_ranks_per_channel=$mem_ranks
memory_read_buffer_bursts_per_channel=$mem_read_buffer_size
memory_write_buffer_bursts_per_channel=$mem_write_buffer_size
memory_page_policy=$mem_page_policy
memory_max_accesses_per_row=$mem_max_accesses_per_row
memory_scheduler=$mem_sched_policy
memory_write_high_threshold_percent=$mem_write_high_thresh
memory_write_low_threshold_percent=$mem_write_low_thresh
memory_min_writes_per_switch=$mem_min_writes_per_switch
memory_min_reads_per_switch=$mem_min_reads_per_switch
memory_controller_frontend_latency=$mem_ctrl_frontend_latency
memory_controller_backend_latency=$mem_ctrl_backend_latency
memory_controller_command_window=$mem_ctrl_command_window
peer_latency_ns=$peer_latency_ns
sync_quantum_ns=$sync_quantum_ns
ub_port_count=$ub_port_count
peer_ring_bytes=$peer_ring_bytes
peer_link_rate_gbps=$peer_link_rate_gbps
peer_serialization_stages=$peer_serialization_stages
peer_switch_delay=$peer_switch_delay
peer_link_overhead_bytes=$peer_link_overhead_bytes
sq_control_bytes=$sq_control_bytes
wqebb_bytes=$wqebb_bytes
sq_sge_bytes=$sq_sge_bytes
direct_wqe_max_blocks=$direct_wqe_max_blocks
direct_wqe_latency=$direct_wqe_latency
sq_fetch_latency=$sq_fetch_latency
sq_wqebb_latency=$sq_wqebb_latency
payload_dma_latency=$payload_dma_latency
payload_dma_rate_gbps=$payload_dma_rate_gbps
udma_poll_interval=$udma_poll_interval
udma_iotlb_entries=$udma_iotlb_entries
dma_max_outstanding=$dma_max_outstanding
dist_link_speed=$dist_link_speed
oob_link_speed=$oob_link_speed
pipe_data=$pipe_data
packet_trace=$packet_trace
comparison_benchmark_profile=ctp-rm-send-imm-i128
comparison_transport_mode=RM
comparison_operation=SEND_IMM
comparison_ctp=1
comparison_inline_bytes=128
comparison_jettys=1
EOF
}

if (( print_config )); then
    print_resolved_config
    exit 0
fi

docker start "$container" >/dev/null

for path in "$gem5" "$kernel" "$initrd" "$config" "$switch_config" \
            "$lab/tools/run-background.sh"; do
    docker exec "$container" test -f "$path" || die "missing in container: $path"
done

# The default image records both its own digest and the exact paths/digests of
# mutable build inputs. Refuse an overwritten archive or a source/image skew.
# Custom initramfs paths remain the caller's own contract.
if [[ "$initrd" == "$lab/out/openurma-interactive.cpio.gz" ]]; then
    image_manifest="${initrd%.cpio.gz}.manifest.txt"
    docker exec "$container" test -r "$image_manifest" ||
        die "missing default initramfs manifest: $image_manifest"
    image_manifest_value() {
        docker exec "$container" awk -F= -v key="$1" \
            '$1 == key { sub(/^[^=]*=/, ""); print; exit }' "$image_manifest"
    }
    verify_image_hash() {
        hash_key=$1
        image_input=$2
        expected_hash=$(image_manifest_value "$hash_key")
        [[ -n "$expected_hash" ]] ||
            die "initramfs manifest lacks $hash_key; rebuild it first"
        actual_hash=$(docker exec "$container" sha256sum "$image_input" | awk '{print $1}')
        [[ "$actual_hash" == "$expected_hash" ]] ||
            die "hash mismatch for $image_input; rebuild the initramfs first"
    }
    verify_image_hash initramfs_sha256 "$initrd"
    verify_image_hash kernel_sha256 "$kernel"
    image_components=(
        overlay_init ou_cpu_switch ou_lat_server ou_lat_client
        urma_perftest provider_source openurma_kmod
        ipv6_module ubcore_module uburma_module
        dist_sync_source cpu_switch_source m5ops_dispatch_source
    )
    if [[ "$provider" == udma ]]; then
        image_components+=(stock_udma_provider ummu_shim)
        image_providers=$(image_manifest_value providers)
        [[ " $image_providers " == *" udma "* ]] ||
            die "default initramfs does not contain the official UDMA provider"
    fi
    for image_component in "${image_components[@]}"; do
        image_input=$(image_manifest_value "${image_component}_path")
        [[ -n "$image_input" ]] ||
            die "initramfs manifest lacks ${image_component}_path; rebuild it first"
        docker exec "$container" test -e "$image_input" ||
            die "recorded initramfs input is missing: $image_input"
        verify_image_hash "${image_component}_sha256" "$image_input"
    done
fi
for resource in boot.arm64 boot.arm; do
    docker exec "$container" test -f "$m5_path/binaries/$resource" ||
        die "missing gem5 ARM resource: $m5_path/binaries/$resource"
done

pid_is_live() {
    docker exec "$container" bash -c '
        pidfile=$1
        expected=$2
        test -r "$pidfile" || exit 1
        pid=$(sed -n "1p" "$pidfile")
        case "$pid" in ""|*[!0-9]*) exit 1;; esac
        kill -0 "$pid" 2>/dev/null || exit 1
        tr "\000" " " < "/proc/$pid/cmdline" | grep -Fq -- "$expected"
    ' _ "$1" "$2"
}

for node in node0 node1 switch; do
    if pid_is_live "$run_root/$node/gem5.pid" "$run_root/$node"; then
        die "$node is already running; use status-dual.sh or stop-dual.sh"
    fi
done
if pid_is_live "$run_root/relay/relay.pid" "$lab/tools/ethernet_relay.py"; then
    die "relay is already running; use status-dual.sh or stop-dual.sh"
fi

if docker exec "$container" test -d "$run_root"; then
    stamp=$(date +%Y%m%d-%H%M%S)
    docker exec "$container" mv "$run_root" "$run_root.previous-$stamp"
    echo "Archived the previous run as $run_root.previous-$stamp"
fi
docker exec "$container" mkdir -p "$run_root/node0" "$run_root/node1" "$run_root/switch" "$run_root/relay"
print_resolved_config | docker exec -i "$container" sh -c \
    'umask 022; tee "$1" >/dev/null' _ "$run_root/run-manifest.txt"
docker exec "$container" rm -f "$ring" "$tap0" "$tap1"
# Must match the per-port trailing-slot ABI in NICTopologySC.cc.
docker exec "$container" truncate -s "$peer_ring_bytes" "$ring"

# The stock dist-gem5 switch owns the synchronization protocol.  It starts
# first and waits for both node connections, just like util/dist/gem5-dist.sh.
docker exec -d "$container" \
    bash "$lab/tools/run-background.sh" \
    "$run_root/switch/gem5.pid" "$run_root/switch/gem5.log" \
    "$gem5" --listener-mode=on --outdir="$run_root/switch" "$switch_config" \
    --is-switch --dist-size=2 --dist-rank=0 \
    --dist-server-port="$dist_port" --dist-sync-start=0t \
    --dist-sync-repeat="${sync_quantum_ns}ns" \
    --ethernet-linkdelay="${peer_latency_ns}ns" \
    --ethernet-linkspeed="$dist_link_speed"

actual_dist_port=""
for _ in $(seq 1 100); do
    actual_dist_port="$(docker exec "$container" sed -n \
        's/.*tcp_iface listening on port \([0-9][0-9]*\).*/\1/p' \
        "$run_root/switch/gem5.log" 2>/dev/null | tail -n 1)"
    [[ -n "$actual_dist_port" ]] && break
    sleep 0.1
done
[[ -n "$actual_dist_port" ]] || die "dist switch did not begin listening; see $run_root/switch/gem5.log"

launch_node() {
    node=$1
    uart=$2
    mac=$3
    tap=$4
    out="$run_root/node$node"
    official_args=()
    if [[ "$provider" == official ]]; then
        official_args+=(--official-udma-discovery)
        official_args+=(--udma-endpoint-eid="$((0x100 + node))")
    fi
    docker exec -d \
        -e "M5_PATH=$m5_path" \
        -e "OPENURMA_PIPE_DATA=$pipe_data" \
        -e "OPENURMA_TRACE_PACKETS=$packet_trace" \
        "$container" \
        bash "$lab/tools/run-background.sh" "$out/gem5.pid" "$out/gem5.log" \
        "$gem5" --listener-mode=on --outdir="$out" "$config" \
        "${official_args[@]}" \
        --kernel="$kernel" --initrd="$initrd" --root-device=/dev/ram \
        --cpu="$cpu_mode" --m5ops-base="$m5ops_base" \
        --cpu-freq="$cpu_freq" \
        --num-cpus="$num_cpus" --benchmark-cpu="$benchmark_cpu" \
        --o3-width="$o3_width" --o3-rob-entries="$o3_rob_entries" \
        --o3-iq-entries="$o3_iq_entries" \
        --o3-lq-entries="$o3_lq_entries" \
        --o3-sq-entries="$o3_sq_entries" \
        --o3-load-ports="$o3_load_ports" \
        --o3-store-ports="$o3_store_ports" \
        --o3-fetch-buffer-bytes="$o3_fetch_buffer_bytes" \
        --o3-fetch-queue-entries="$o3_fetch_queue_entries" \
        --o3-phys-int-regs="$o3_phys_int_regs" \
        --o3-phys-float-regs="$o3_phys_float_regs" \
        --o3-phys-vec-regs="$o3_phys_vec_regs" \
        --o3-phys-vec-pred-regs="$o3_phys_vec_pred_regs" \
        --o3-phys-mat-regs="$o3_phys_mat_regs" \
        --last-cache-level="$last_cache_level" \
        --cache-line-size="$cache_line_size" \
        --l1i-size="$l1i_size" --l1i-assoc="$l1i_assoc" \
        --l1i-latency="$l1i_latency" \
        --l1i-mshrs="$l1i_mshrs" \
        --l1i-targets-per-mshr="$l1i_targets" \
        --l1i-write-buffers="$l1i_write_buffers" \
        --l1d-size="$l1d_size" --l1d-assoc="$l1d_assoc" \
        --l1d-latency="$l1d_latency" \
        --l1d-mshrs="$l1d_mshrs" \
        --l1d-targets-per-mshr="$l1d_targets" \
        --l1d-write-buffers="$l1d_write_buffers" \
        --l2-size="$l2_size" --l2-assoc="$l2_assoc" \
        --l2-latency="$l2_latency" \
        --l2-mshrs="$l2_mshrs" \
        --l2-targets-per-mshr="$l2_targets" \
        --l2-write-buffers="$l2_write_buffers" \
        --l3-size="$l3_size" --l3-assoc="$l3_assoc" \
        --l3-latency="$l3_latency" \
        --l3-mshrs="$l3_mshrs" \
        --l3-targets-per-mshr="$l3_targets" \
        --l3-write-buffers="$l3_write_buffers" \
        --fabric-freq="$fabric_freq" \
        --fabric-width-bytes="$fabric_width_bytes" \
        --coherent-bus-frontend-latency="$coherent_bus_frontend_latency" \
        --coherent-bus-forward-latency="$coherent_bus_forward_latency" \
        --coherent-bus-response-latency="$coherent_bus_response_latency" \
        --coherent-bus-snoop-response-latency="$coherent_bus_snoop_response_latency" \
        --coherent-bus-header-latency="$coherent_bus_header_latency" \
        --memory-bus-frontend-latency="$memory_bus_frontend_latency" \
        --memory-bus-forward-latency="$memory_bus_forward_latency" \
        --memory-bus-response-latency="$memory_bus_response_latency" \
        --memory-bus-snoop-response-latency="$memory_bus_snoop_response_latency" \
        --memory-bus-header-latency="$memory_bus_header_latency" \
        --core-bus-snoop-filter-capacity="$core_bus_snoop_filter_capacity" \
        --l3-bus-snoop-filter-capacity="$l3_bus_snoop_filter_capacity" \
        --membus-snoop-filter-capacity="$membus_snoop_filter_capacity" \
        --io-bus-frontend-latency="$io_bus_frontend_latency" \
        --io-bus-forward-latency="$io_bus_forward_latency" \
        --io-bus-response-latency="$io_bus_response_latency" \
        --io-bus-header-latency="$io_bus_header_latency" \
        --io-cache-size="$io_cache_size" \
        --io-cache-assoc="$io_cache_assoc" \
        --io-cache-latency="$io_cache_latency" \
        --io-cache-mshrs="$io_cache_mshrs" \
        --io-cache-targets-per-mshr="$io_cache_targets" \
        --io-cache-write-buffers="$io_cache_write_buffers" \
        --mem-size="$mem_size" --guest-mem-limit="$guest_mem_limit" \
        --mem-type="$mem_type" \
        --mem-channels="$mem_channels" \
        --mem-channels-intlv="$mem_channels_intlv" \
        --mem-addr-mapping="$mem_addr_mapping" \
        --xor-low-bit="$mem_channel_xor_low_bit" \
        --mem-ranks="$mem_ranks" \
        --mem-read-buffer-size="$mem_read_buffer_size" \
        --mem-write-buffer-size="$mem_write_buffer_size" \
        --mem-page-policy="$mem_page_policy" \
        --mem-max-accesses-per-row="$mem_max_accesses_per_row" \
        --mem-sched-policy="$mem_sched_policy" \
        --mem-write-high-thresh="$mem_write_high_thresh" \
        --mem-write-low-thresh="$mem_write_low_thresh" \
        --mem-min-writes-per-switch="$mem_min_writes_per_switch" \
        --mem-min-reads-per-switch="$mem_min_reads_per_switch" \
        --mem-ctrl-frontend-latency="$mem_ctrl_frontend_latency" \
        --mem-ctrl-backend-latency="$mem_ctrl_backend_latency" \
        --mem-ctrl-command-window="$mem_ctrl_command_window" \
        --link-delay-ns=0 \
        --peer-ring="$ring" --peer-node="$node" \
        --ub-port-count="$ub_port_count" \
        --peer-link-latency="${peer_latency_ns}ns" \
        --peer-link-rate-gbps="$peer_link_rate_gbps" \
        --peer-serialization-stages="$peer_serialization_stages" \
        --peer-switch-delay="$peer_switch_delay" \
        --peer-link-overhead-bytes="$peer_link_overhead_bytes" \
        --sq-control-bytes="$sq_control_bytes" \
        --wqebb-bytes="$wqebb_bytes" \
        --sq-sge-bytes="$sq_sge_bytes" \
        --direct-wqe-max-blocks="$direct_wqe_max_blocks" \
        --direct-wqe-latency="$direct_wqe_latency" \
        --sq-fetch-latency="$sq_fetch_latency" \
        --sq-wqebb-latency="$sq_wqebb_latency" \
        --payload-dma-latency="$payload_dma_latency" \
        --payload-dma-rate-gbps="$payload_dma_rate_gbps" \
        --udma-poll-interval="$udma_poll_interval" \
        --udma-iotlb-entries="$udma_iotlb_entries" \
        --dma-max-outstanding="$dma_max_outstanding" \
        --dma-backend="$dma_backend" \
        --dist-rank="$node" --dist-size=2 --dist-server-name=127.0.0.1 \
        --dist-server-port="$actual_dist_port" --dist-sync-start=0t \
        --dist-sync-repeat="${sync_quantum_ns}ns" --dist-sync-on-pseudo-op \
        --eth-tap-socket="$tap" \
        --eth-link-speed="$oob_link_speed" --eth-link-delay="${peer_latency_ns}ns" \
        --terminal-port="$uart" --eth-mac="$mac" \
        --extra-cmdline="openurma_node=$node openurma_provider=$provider"
}

launch_node 0 "$uart0" 02:00:00:00:00:01 "$tap0"
launch_node 1 "$uart1" 02:00:00:00:00:02 "$tap1"

# The TCP control plane is intentionally outside the fine-grained virtual-time
# barrier. Perftest enters the conservative barrier collectively only after
# this channel has completed its final pre-measurement handshake.
docker exec -d "$container" \
    bash "$lab/tools/run-background.sh" \
    "$run_root/relay/relay.pid" "$run_root/relay/relay.log" \
    python3 "$lab/tools/ethernet_relay.py" "unix:$tap0" "unix:$tap1"

echo "Started two independent gem5 full-system guests:"
if [[ "$provider" == official ]]; then
    echo "  node0 UART: localhost:$uart0, EID ...:0100, OOB 10.0.0.1"
    echo "  node1 UART: localhost:$uart1, EID ...:0101, OOB 10.0.0.2"
else
    echo "  node0 UART: localhost:$uart0, EID fe80::1, OOB 10.0.0.1"
    echo "  node1 UART: localhost:$uart1, EID fe80::2, OOB 10.0.0.2"
fi
echo "  model profile: $profile ($num_cpus x $cpu_mode at $cpu_freq)"
echo "  cache: private $l1i_size I + $l1d_size D + $l2_size L2; shared $l3_size L3"
echo "  memory: $mem_size modeled, Linux limited to $guest_mem_limit; $mem_channels x $mem_type, $mem_ranks rank/channel"
echo "  resolved parameters: $run_root/run-manifest.txt"
echo "  UB peer ring: $ring (${ub_port_count} physical port(s), ${peer_link_rate_gbps} Gbit/s per port, ${peer_latency_ns} ns propagation, ${peer_serialization_stages} serialization stage(s) per port)"
echo "  UB switch service delay: $peer_switch_delay"
echo "  dist-gem5 switch: localhost:$actual_dist_port (${sync_quantum_ns} ns quantum)"
echo "  OOB relay: $tap0 <-> $tap1 (setup only)"
echo
echo "After both shells are ready, detach any existing UART clients and run:"
echo "  bash $lab_host/sync-dual.sh"
echo
echo "Attach from two host terminals:"
echo "  bash $lab_host/attach-node0.sh"
echo "  bash $lab_host/attach-node1.sh"
