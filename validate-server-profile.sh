#!/usr/bin/env bash
set -euo pipefail

# Validate the instantiated generic Arm server profile.  This script is
# deliberately read-only: it checks the resolved manifest, gem5's config.ini
# files, and (optionally) evidence left by a completed CPU switch.

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
run_root="${OPENURMA_DUAL_OUT:-$script_dir/run-dual}"
runtime_mode=auto
positional_root_seen=0

usage() {
    cat <<'EOF'
Usage: validate-server-profile.sh [OPTIONS] [RUN_ROOT]

Validate a dual-node server-profile run without changing it.

Options:
  --runtime             Check runtime evidence when available (default).
  --require-runtime     Fail unless both nodes have entered O3 and recorded
                        non-zero O3 cycles.
  --no-runtime          Check only the manifest and config.ini files.
  --run-root DIR        Use DIR instead of ./run-dual.
  -h, --help            Show this help.

Missing runtime evidence is reported as NOTE unless --require-runtime is used.
Missing manifest/config.ini files are always an error because no static profile
can be validated yet.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --runtime)
            runtime_mode=auto
            shift
            ;;
        --require-runtime)
            runtime_mode=require
            shift
            ;;
        --no-runtime)
            runtime_mode=skip
            shift
            ;;
        --run-root)
            [ "$#" -ge 2 ] || {
                echo "validate-server-profile.sh: --run-root needs a value" >&2
                exit 2
            }
            run_root=$2
            positional_root_seen=1
            shift 2
            ;;
        --run-root=*)
            run_root=${1#*=}
            positional_root_seen=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --*)
            echo "validate-server-profile.sh: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            if [ "$positional_root_seen" -ne 0 ]; then
                echo "validate-server-profile.sh: more than one run root" >&2
                exit 2
            fi
            run_root=$1
            positional_root_seen=1
            shift
            ;;
    esac
done

LC_ALL=C
export LC_ALL

checks=0
errors=0
notes=0

fail() {
    errors=$((errors + 1))
    echo "ERROR: $*" >&2
}

note() {
    notes=$((notes + 1))
    echo "NOTE: $*"
}

checked() {
    checks=$((checks + 1))
}

assert_eq() {
    actual=$1
    expected=$2
    label=$3
    checked
    if [ "$actual" != "$expected" ]; then
        if [ -z "$actual" ]; then
            actual='<missing>'
        fi
        fail "$label: expected '$expected', got '$actual'"
    fi
}

assert_match() {
    actual=$1
    pattern=$2
    label=$3
    checked
    if ! printf '%s\n' "$actual" | grep -Eq -- "$pattern"; then
        if [ -z "$actual" ]; then
            actual='<missing>'
        fi
        fail "$label: value '$actual' does not match /$pattern/"
    fi
}

manifest_value() {
    key=$1
    file=$2
    awk -F= -v wanted="$key" '
        $1 == wanted {
            sub(/^[^=]*=/, "")
            sub(/\r$/, "")
            print
            exit
        }
    ' "$file"
}

ini_value() {
    file=$1
    section=$2
    key=$3
    awk -v wanted_section="[$section]" -v wanted_key="$key" '
        /^\[/ {
            active = ($0 == wanted_section)
            next
        }
        active && index($0, wanted_key "=") == 1 {
            print substr($0, length(wanted_key) + 2)
            exit
        }
    ' "$file"
}

ini_section_count() {
    file=$1
    section_pattern=$2
    awk -v wanted="$section_pattern" '
        $0 ~ ("^\\[" wanted "\\]$") { count++ }
        END { print count + 0 }
    ' "$file"
}

assert_manifest() {
    key=$1
    expected=$2
    value=$(manifest_value "$key" "$manifest")
    assert_eq "$value" "$expected" "manifest $key"
}

assert_ini() {
    file=$1
    section=$2
    key=$3
    expected=$4
    value=$(ini_value "$file" "$section" "$key")
    assert_eq "$value" "$expected" "$(basename "$(dirname "$file")") [$section] $key"
}

assert_ini_match() {
    file=$1
    section=$2
    key=$3
    pattern=$4
    value=$(ini_value "$file" "$section" "$key")
    assert_match "$value" "$pattern" "$(basename "$(dirname "$file")") [$section] $key"
}

manifest="$run_root/run-manifest.txt"

echo "Validating server profile under: $run_root"

if [ -r "$manifest" ]; then
    assert_manifest manifest_version 1
    assert_manifest profile server
    assert_manifest profile_revision server-o3-ddr4-coherent-tdma-v5
    assert_manifest provider udma
    assert_manifest dma_backend udma
    assert_manifest dma_transport native_gem5_request_port_dynamic
    assert_manifest udma_address_translation context_pgd_control_abi_v2
    assert_manifest dma_request_segmentation cacheline_and_4KiB_boundaries
    assert_manifest udma_iotlb_policy fully_associative_lru_4KiB_context_tagged
    assert_manifest cpu_mode server_o3
    assert_manifest cpu_freq 3GHz
    assert_manifest cpu_count 4
    assert_manifest benchmark_cpu 2
    assert_manifest o3_width 8
    assert_manifest o3_rob_entries 192
    assert_manifest o3_iq_entries 64
    assert_manifest o3_lq_entries 32
    assert_manifest o3_sq_entries 32
    assert_manifest o3_load_ports 4
    assert_manifest o3_store_ports 2
    assert_manifest o3_fetch_buffer_bytes 64
    assert_manifest o3_fetch_queue_entries 32
    assert_manifest o3_phys_int_regs 256
    assert_manifest o3_phys_float_regs 256
    assert_manifest o3_phys_vec_regs 256
    assert_manifest o3_phys_vec_pred_regs 32
    assert_manifest o3_phys_mat_regs 2
    assert_manifest o3_phys_cc_regs 1280
    assert_manifest o3_branch_predictor TournamentBP
    assert_manifest o3_fu_pool DefaultFUPool
    assert_manifest o3_fu_pool_signature int_alu6,int_muldiv2,fp_alu4,fp_muldiv2,simd4,pred1,rdwr4,ipr1
    assert_manifest cache_line_size 64
    assert_manifest last_cache_level 3
    assert_manifest cache_prefetcher none
    assert_manifest cache_replacement_policy lru
    assert_manifest l1i_size 64kB
    assert_manifest l1i_assoc 4
    assert_manifest l1i_latency_tag_data_response_cycles 1,1,1
    assert_manifest l1i_mshrs 8
    assert_manifest l1i_targets_per_mshr 8
    assert_manifest l1i_write_buffers 8
    assert_manifest l1i_clusivity mostly_incl
    assert_manifest l1d_size 64kB
    assert_manifest l1d_assoc 4
    assert_manifest l1d_latency_tag_data_response_cycles 2,2,1
    assert_manifest l1d_mshrs 16
    assert_manifest l1d_targets_per_mshr 16
    assert_manifest l1d_write_buffers 16
    assert_manifest l1d_clusivity mostly_incl
    assert_manifest l2_size 1MB
    assert_manifest l2_assoc 8
    assert_manifest l2_latency_tag_data_response_cycles 12,12,5
    assert_manifest l2_mshrs 32
    assert_manifest l2_targets_per_mshr 16
    assert_manifest l2_write_buffers 16
    assert_manifest l2_clusivity mostly_excl
    assert_manifest l3_size 32MB
    assert_manifest l3_assoc 16
    assert_manifest l3_latency_tag_data_response_cycles 20,20,20
    assert_manifest l3_mshrs 64
    assert_manifest l3_targets_per_mshr 16
    assert_manifest l3_write_buffers 32
    assert_manifest l3_clusivity mostly_excl
    assert_manifest l3_freq 2GHz
    assert_manifest fabric_freq 2GHz
    assert_manifest fabric_width_bytes 64
    assert_manifest coherent_bus_frontend_latency_cycles 1
    assert_manifest coherent_bus_forward_latency_cycles 0
    assert_manifest coherent_bus_response_latency_cycles 1
    assert_manifest coherent_bus_snoop_response_latency_cycles 1
    assert_manifest coherent_bus_header_latency_cycles 1
    assert_manifest memory_bus_frontend_latency_cycles 3
    assert_manifest memory_bus_forward_latency_cycles 4
    assert_manifest memory_bus_response_latency_cycles 2
    assert_manifest memory_bus_snoop_response_latency_cycles 4
    assert_manifest memory_bus_header_latency_cycles 1
    assert_manifest core_bus_snoop_filter_capacity 2MiB
    assert_manifest l3_bus_snoop_filter_capacity 8MiB
    assert_manifest membus_snoop_filter_capacity 64MiB
    assert_manifest io_bus_frontend_latency_cycles 2
    assert_manifest io_bus_forward_latency_cycles 1
    assert_manifest io_bus_response_latency_cycles 2
    assert_manifest io_bus_header_latency_cycles 1
    assert_manifest io_cache_size 1kB
    assert_manifest io_cache_assoc 8
    assert_manifest io_cache_latency_tag_data_response_cycles 1,1,1
    assert_manifest io_cache_mshrs 32
    assert_manifest io_cache_targets_per_mshr 32
    assert_manifest io_cache_write_buffers 32
    assert_manifest memory_size 64GB
    assert_manifest guest_memory_limit 8GB
    assert_manifest memory_type DDR4_2400_8x8
    assert_manifest memory_channels 8
    assert_manifest memory_channel_interleave_bytes 128
    assert_manifest memory_address_mapping RoRaBaCoCh
    assert_manifest memory_channel_xor_low_bit 0
    assert_manifest memory_ranks_per_channel 1
    assert_manifest memory_read_buffer_bursts_per_channel 64
    assert_manifest memory_write_buffer_bursts_per_channel 128
    assert_manifest memory_page_policy open_adaptive
    assert_manifest memory_max_accesses_per_row 16
    assert_manifest memory_scheduler frfcfs
    assert_manifest memory_write_high_threshold_percent 85
    assert_manifest memory_write_low_threshold_percent 50
    assert_manifest memory_min_writes_per_switch 16
    assert_manifest memory_min_reads_per_switch 16
    assert_manifest memory_controller_frontend_latency 10ns
    assert_manifest memory_controller_backend_latency 10ns
    assert_manifest memory_controller_command_window 10ns
    assert_manifest udma_poll_interval 10ns
    assert_manifest udma_iotlb_entries 64
    assert_manifest dma_max_outstanding 16
else
    fail "missing readable manifest: $manifest"
    note "No resolved server profile exists yet; start run-dual.sh and retry after gem5 writes its output files."
fi

validate_node_config() {
    node=$1
    cfg="$run_root/$node/config.ini"

    if [ ! -r "$cfg" ]; then
        fail "$node config.ini is not readable: $cfg"
        note "$node has not instantiated yet; wait for gem5 configuration, then retry."
        return
    fi

    assert_ini "$cfg" system cache_line_size 64
    # config.ini is an instantiate-time snapshot.  server_o3 intentionally
    # starts in atomic mode; m5.switchCpus changes this to timing at runtime.
    assert_ini "$cfg" system mem_mode atomic
    assert_ini "$cfg" system mem_ranges 2147483648:70866960384

    count=$(ini_section_count "$cfg" 'system\.cpu_cluster[0-9]+\.cpus')
    assert_eq "$count" 4 "$node boot CPU count"
    count=$(ini_section_count "$cfg" 'system\.switch_cpus[0-9]+')
    assert_eq "$count" 4 "$node O3 target CPU count"
    count=$(ini_section_count "$cfg" 'system\.cpu_cluster[0-9]+\.cpus\.icache')
    assert_eq "$count" 4 "$node private L1I count"
    count=$(ini_section_count "$cfg" 'system\.cpu_cluster[0-9]+\.cpus\.dcache')
    assert_eq "$count" 4 "$node private L1D count"
    count=$(ini_section_count "$cfg" 'system\.cpu_cluster[0-9]+\.l2')
    assert_eq "$count" 4 "$node private L2 count"
    count=$(ini_section_count "$cfg" 'system\.l3')
    assert_eq "$count" 1 "$node shared L3 count"

    i=0
    while [ "$i" -lt 4 ]; do
        cluster="system.cpu_cluster$i"
        boot="$cluster.cpus"
        o3="system.switch_cpus$i"

        assert_ini "$cfg" "$cluster.clk_domain" clock 333
        assert_ini "$cfg" "$boot" type BaseAtomicSimpleCPU
        assert_ini "$cfg" "$boot" cpu_id "$i"
        assert_ini "$cfg" "$boot" switched_out false
        assert_ini "$cfg" "$boot" clk_domain "$cluster.clk_domain"
        assert_ini "$cfg" "$boot" icache_port "$boot.icache.cpu_side"
        assert_ini "$cfg" "$boot" dcache_port "$boot.dcache.cpu_side"

        assert_ini "$cfg" "$o3" type BaseO3CPU
        assert_ini "$cfg" "$o3" cpu_id "$i"
        assert_ini "$cfg" "$o3" switched_out true
        assert_ini "$cfg" "$o3" clk_domain "$cluster.clk_domain"
        for width in fetchWidth decodeWidth renameWidth dispatchWidth \
            issueWidth wbWidth commitWidth squashWidth; do
            assert_ini "$cfg" "$o3" "$width" 8
        done
        assert_ini "$cfg" "$o3" numROBEntries 192
        assert_ini "$cfg" "$o3" numIQEntries 64
        assert_ini "$cfg" "$o3" LQEntries 32
        assert_ini "$cfg" "$o3" SQEntries 32
        assert_ini "$cfg" "$o3" cacheLoadPorts 4
        assert_ini "$cfg" "$o3" cacheStorePorts 2
        assert_ini "$cfg" "$o3" fetchBufferSize 64
        assert_ini "$cfg" "$o3" fetchQueueSize 32
        assert_ini "$cfg" "$o3" numPhysIntRegs 256
        assert_ini "$cfg" "$o3" numPhysFloatRegs 256
        assert_ini "$cfg" "$o3" numPhysVecRegs 256
        assert_ini "$cfg" "$o3" numPhysVecPredRegs 32
        assert_ini "$cfg" "$o3" numPhysMatRegs 2
        assert_ini "$cfg" "$o3" numPhysCCRegs 1280
        for delay in decodeToFetchDelay renameToFetchDelay iewToFetchDelay \
            commitToFetchDelay renameToDecodeDelay iewToDecodeDelay \
            commitToDecodeDelay fetchToDecodeDelay iewToRenameDelay \
            commitToRenameDelay decodeToRenameDelay commitToIEWDelay \
            issueToExecuteDelay iewToCommitDelay renameToROBDelay; do
            assert_ini "$cfg" "$o3" "$delay" 1
        done
        assert_ini "$cfg" "$o3" renameToIEWDelay 2
        assert_ini "$cfg" "$o3" trapLatency 13
        assert_ini "$cfg" "$o3" fetchTrapLatency 1
        assert_ini "$cfg" "$o3" backComSize 5
        assert_ini "$cfg" "$o3" forwardComSize 5
        assert_ini "$cfg" "$o3" LSQDepCheckShift 4
        assert_ini "$cfg" "$o3" LSQCheckLoads true
        assert_ini "$cfg" "$o3" store_set_clear_period 250000
        assert_ini "$cfg" "$o3" LFSTSize 1024
        assert_ini "$cfg" "$o3" SSITSize 1024
        assert_ini "$cfg" "$o3.branchPred" type TournamentBP
        assert_ini "$cfg" "$o3.branchPred" localPredictorSize 2048
        assert_ini "$cfg" "$o3.branchPred" localCtrBits 2
        assert_ini "$cfg" "$o3.branchPred" localHistoryTableSize 2048
        assert_ini "$cfg" "$o3.branchPred" globalPredictorSize 8192
        assert_ini "$cfg" "$o3.branchPred" globalCtrBits 2
        assert_ini "$cfg" "$o3.branchPred" choicePredictorSize 8192
        assert_ini "$cfg" "$o3.branchPred" choiceCtrBits 2
        assert_ini "$cfg" "$o3.branchPred" instShiftAmt 2
        assert_ini "$cfg" "$o3.branchPred" requiresBTBHit false
        assert_ini "$cfg" "$o3.branchPred.btb" type SimpleBTB
        assert_ini "$cfg" "$o3.branchPred.btb" numEntries 4096
        assert_ini "$cfg" "$o3.branchPred.btb" tagBits 16
        assert_ini "$cfg" "$o3.branchPred.ras" type ReturnAddrStack
        assert_ini "$cfg" "$o3.branchPred.ras" numEntries 16
        assert_ini "$cfg" "$o3.branchPred.indirectBranchPred" type SimpleIndirectPredictor
        assert_ini "$cfg" "$o3.branchPred.indirectBranchPred" indirectSets 256
        assert_ini "$cfg" "$o3.branchPred.indirectBranchPred" indirectWays 2
        assert_ini "$cfg" "$o3.branchPred.indirectBranchPred" indirectTagSize 16
        assert_ini "$cfg" "$o3.branchPred.indirectBranchPred" indirectPathLength 3
        assert_ini "$cfg" "$o3.branchPred.indirectBranchPred" speculativePathLength 256
        assert_ini "$cfg" "$o3.branchPred.indirectBranchPred" indirectGHRBits 13
        assert_ini "$cfg" "$o3.branchPred.indirectBranchPred" indirectHashGHR true
        assert_ini "$cfg" "$o3.branchPred.indirectBranchPred" indirectHashTargets true
        assert_ini "$cfg" "$o3.fuPool" type FUPool
        fu_counts='6 2 4 2 0 4 1 0 4 1'
        fu_index=0
        for fu_count in $fu_counts; do
            assert_ini "$cfg" "$o3.fuPool.FUList$fu_index" count "$fu_count"
            fu_index=$((fu_index + 1))
        done
        for spec in \
            'FUList0.opList:IntAlu:1:true' \
            'FUList1.opList0:IntMult:3:true' \
            'FUList1.opList1:IntDiv:20:false' \
            'FUList8.opList00:MemRead:1:true' \
            'FUList8.opList01:MemWrite:1:true' \
            'FUList9.opList:IprAccess:3:false'; do
            path=${spec%%:*}
            rest=${spec#*:}
            op_class=${rest%%:*}
            rest=${rest#*:}
            op_latency=${rest%%:*}
            pipelined=${rest##*:}
            section="$o3.fuPool.$path"
            assert_ini "$cfg" "$section" opClass "$op_class"
            assert_ini "$cfg" "$section" opLat "$op_latency"
            assert_ini "$cfg" "$section" pipelined "$pipelined"
        done

        l1i="$boot.icache"
        assert_ini "$cfg" "$l1i" type Cache
        assert_ini "$cfg" "$l1i" size 65536
        assert_ini "$cfg" "$l1i" assoc 4
        assert_ini "$cfg" "$l1i" tag_latency 1
        assert_ini "$cfg" "$l1i" data_latency 1
        assert_ini "$cfg" "$l1i" response_latency 1
        assert_ini "$cfg" "$l1i" mshrs 8
        assert_ini "$cfg" "$l1i" tgts_per_mshr 8
        assert_ini "$cfg" "$l1i" write_buffers 8
        assert_ini "$cfg" "$l1i" clusivity mostly_incl
        assert_ini "$cfg" "$l1i" demand_mshr_reserve 1
        assert_ini "$cfg" "$l1i" sequential_access false
        assert_ini "$cfg" "$l1i" write_allocator Null
        assert_ini "$cfg" "$l1i" is_read_only true
        assert_ini "$cfg" "$l1i" writeback_clean true
        assert_ini "$cfg" "$l1i" prefetcher Null
        assert_ini "$cfg" "$l1i.replacement_policy" type LRURP
        assert_ini "$cfg" "$l1i.tags" type BaseSetAssoc
        assert_ini "$cfg" "$l1i.tags.indexing_policy" type SetAssociative
        assert_ini "$cfg" "$l1i" clk_domain "$cluster.clk_domain"
        assert_ini "$cfg" "$l1i" mem_side "$cluster.toL2Bus.cpu_side_ports[0]"

        l1d="$boot.dcache"
        assert_ini "$cfg" "$l1d" type Cache
        assert_ini "$cfg" "$l1d" size 65536
        assert_ini "$cfg" "$l1d" assoc 4
        assert_ini "$cfg" "$l1d" tag_latency 2
        assert_ini "$cfg" "$l1d" data_latency 2
        assert_ini "$cfg" "$l1d" response_latency 1
        assert_ini "$cfg" "$l1d" mshrs 16
        assert_ini "$cfg" "$l1d" tgts_per_mshr 16
        assert_ini "$cfg" "$l1d" write_buffers 16
        assert_ini "$cfg" "$l1d" clusivity mostly_incl
        assert_ini "$cfg" "$l1d" demand_mshr_reserve 1
        assert_ini "$cfg" "$l1d" sequential_access false
        assert_ini "$cfg" "$l1d" write_allocator Null
        assert_ini "$cfg" "$l1d" is_read_only false
        assert_ini "$cfg" "$l1d" writeback_clean false
        assert_ini "$cfg" "$l1d" prefetcher Null
        assert_ini "$cfg" "$l1d.replacement_policy" type LRURP
        assert_ini "$cfg" "$l1d.tags" type BaseSetAssoc
        assert_ini "$cfg" "$l1d.tags.indexing_policy" type SetAssociative
        assert_ini "$cfg" "$l1d" clk_domain "$cluster.clk_domain"
        assert_ini "$cfg" "$l1d" mem_side "$cluster.toL2Bus.cpu_side_ports[1]"

        l2="$cluster.l2"
        assert_ini "$cfg" "$l2" type Cache
        assert_ini "$cfg" "$l2" size 1048576
        assert_ini "$cfg" "$l2" assoc 8
        assert_ini "$cfg" "$l2" tag_latency 12
        assert_ini "$cfg" "$l2" data_latency 12
        assert_ini "$cfg" "$l2" response_latency 5
        assert_ini "$cfg" "$l2" mshrs 32
        assert_ini "$cfg" "$l2" tgts_per_mshr 16
        assert_ini "$cfg" "$l2" write_buffers 16
        assert_ini "$cfg" "$l2" clusivity mostly_excl
        assert_ini "$cfg" "$l2" demand_mshr_reserve 1
        assert_ini "$cfg" "$l2" sequential_access false
        assert_ini "$cfg" "$l2" write_allocator Null
        assert_ini "$cfg" "$l2" is_read_only false
        assert_ini "$cfg" "$l2" writeback_clean false
        assert_ini "$cfg" "$l2" prefetcher Null
        assert_ini "$cfg" "$l2.replacement_policy" type LRURP
        assert_ini "$cfg" "$l2.tags" type BaseSetAssoc
        assert_ini "$cfg" "$l2.tags.indexing_policy" type SetAssociative
        assert_ini "$cfg" "$l2" clk_domain "$cluster.clk_domain"
        assert_ini "$cfg" "$l2" cpu_side "$cluster.toL2Bus.mem_side_ports[0]"
        assert_ini "$cfg" "$l2" mem_side "system.toL3Bus.cpu_side_ports[$i]"

        core_bus="$cluster.toL2Bus"
        assert_ini "$cfg" "$core_bus" clk_domain "$cluster.clk_domain"
        assert_ini "$cfg" "$core_bus" width 64
        assert_ini "$cfg" "$core_bus" frontend_latency 1
        assert_ini "$cfg" "$core_bus" forward_latency 0
        assert_ini "$cfg" "$core_bus" response_latency 1
        assert_ini "$cfg" "$core_bus" snoop_response_latency 1
        assert_ini "$cfg" "$core_bus" header_latency 1
        assert_ini "$cfg" "$core_bus.snoop_filter" max_capacity 2097152

        i=$((i + 1))
    done

    assert_ini "$cfg" system.l3 type Cache
    assert_ini "$cfg" system.l3 size 33554432
    assert_ini "$cfg" system.l3 assoc 16
    assert_ini "$cfg" system.l3 tag_latency 20
    assert_ini "$cfg" system.l3 data_latency 20
    assert_ini "$cfg" system.l3 response_latency 20
    assert_ini "$cfg" system.l3 mshrs 64
    assert_ini "$cfg" system.l3 tgts_per_mshr 16
    assert_ini "$cfg" system.l3 write_buffers 32
    assert_ini "$cfg" system.l3 clusivity mostly_excl
    assert_ini "$cfg" system.l3 demand_mshr_reserve 1
    assert_ini "$cfg" system.l3 sequential_access false
    assert_ini "$cfg" system.l3 write_allocator Null
    assert_ini "$cfg" system.l3 is_read_only false
    assert_ini "$cfg" system.l3 writeback_clean false
    assert_ini "$cfg" system.l3 prefetcher Null
    assert_ini "$cfg" system.l3.replacement_policy type LRURP
    assert_ini "$cfg" system.l3.tags type BaseSetAssoc
    assert_ini "$cfg" system.l3.tags.indexing_policy type SetAssociative
    assert_ini "$cfg" system.l3 clk_domain system.fabric_clk_domain
    assert_ini "$cfg" system.l3 cpu_side 'system.toL3Bus.mem_side_ports[0]'
    assert_ini_match "$cfg" system.l3 mem_side \
        '^system\.membus\.cpu_side_ports\[[0-9]+\]$'

    assert_ini "$cfg" system.fabric_clk_domain clock 500
    assert_ini "$cfg" system.toL3Bus clk_domain system.fabric_clk_domain
    assert_ini "$cfg" system.toL3Bus width 64
    assert_ini "$cfg" system.toL3Bus frontend_latency 1
    assert_ini "$cfg" system.toL3Bus forward_latency 0
    assert_ini "$cfg" system.toL3Bus response_latency 1
    assert_ini "$cfg" system.toL3Bus snoop_response_latency 1
    assert_ini "$cfg" system.toL3Bus header_latency 1
    assert_ini "$cfg" system.toL3Bus.snoop_filter max_capacity 8388608
    assert_ini "$cfg" system.toL3Bus cpu_side_ports \
        'system.cpu_cluster0.l2.mem_side system.cpu_cluster1.l2.mem_side system.cpu_cluster2.l2.mem_side system.cpu_cluster3.l2.mem_side'
    assert_ini "$cfg" system.toL3Bus mem_side_ports system.l3.cpu_side
    assert_ini "$cfg" system.membus clk_domain system.fabric_clk_domain
    assert_ini "$cfg" system.membus width 64
    assert_ini "$cfg" system.membus frontend_latency 3
    assert_ini "$cfg" system.membus forward_latency 4
    assert_ini "$cfg" system.membus response_latency 2
    assert_ini "$cfg" system.membus snoop_response_latency 4
    assert_ini "$cfg" system.membus header_latency 1
    assert_ini "$cfg" system.membus.snoop_filter max_capacity 67108864

    assert_ini "$cfg" system.iobus type NoncoherentXBar
    assert_ini "$cfg" system.iobus clk_domain system.fabric_clk_domain
    assert_ini "$cfg" system.iobus width 64
    assert_ini "$cfg" system.iobus frontend_latency 2
    assert_ini "$cfg" system.iobus forward_latency 1
    assert_ini "$cfg" system.iobus response_latency 2
    assert_ini "$cfg" system.iobus header_latency 1

    assert_ini "$cfg" system.iocache type Cache
    assert_ini "$cfg" system.iocache size 1024
    assert_ini "$cfg" system.iocache assoc 8
    assert_ini "$cfg" system.iocache tag_latency 1
    assert_ini "$cfg" system.iocache data_latency 1
    assert_ini "$cfg" system.iocache response_latency 1
    assert_ini "$cfg" system.iocache mshrs 32
    assert_ini "$cfg" system.iocache tgts_per_mshr 32
    assert_ini "$cfg" system.iocache write_buffers 32
    assert_ini "$cfg" system.iocache clusivity mostly_incl
    assert_ini "$cfg" system.iocache demand_mshr_reserve 1
    assert_ini "$cfg" system.iocache sequential_access false
    assert_ini "$cfg" system.iocache write_allocator Null
    assert_ini "$cfg" system.iocache is_read_only false
    assert_ini "$cfg" system.iocache writeback_clean false
    assert_ini "$cfg" system.iocache prefetcher Null
    assert_ini "$cfg" system.iocache.replacement_policy type LRURP
    assert_ini "$cfg" system.iocache.tags type BaseSetAssoc
    assert_ini "$cfg" system.iocache.tags.indexing_policy type SetAssociative
    assert_ini "$cfg" system.iocache clk_domain system.fabric_clk_domain
    assert_ini_match "$cfg" system.iocache cpu_side \
        '^system\.iobus\.mem_side_ports\[[0-9]+\]$'
    assert_ini_match "$cfg" system.iocache mem_side \
        '^system\.membus\.cpu_side_ports\[[0-9]+\]$'

    assert_ini "$cfg" system.nic type NICTopologySC
    assert_ini "$cfg" system.nic dma_backend udma
    assert_ini "$cfg" system.nic dma_max_outstanding 16
    assert_ini "$cfg" system.nic udma_iotlb_entries 64
    assert_ini_match "$cfg" system.nic dma \
        '^system\.iobus\.cpu_side_ports\[[0-9]+\]$'
    assert_ini "$cfg" system.nic udma_poll_interval 10000
    assert_ini "$cfg" system.nic direct_wqe_latency 0
    assert_ini "$cfg" system.nic sq_fetch_latency 0
    assert_ini "$cfg" system.nic sq_wqebb_latency 0
    assert_ini "$cfg" system.nic payload_dma_latency 0
    assert_ini "$cfg" system.nic payload_dma_bandwidth 0.000000

    count=$(ini_section_count "$cfg" 'system\.mem_ctrls[0-9]+')
    assert_eq "$count" 8 "$node memory-controller count"
    count=$(ini_section_count "$cfg" 'system\.mem_ctrls[0-9]+\.dram')
    assert_eq "$count" 8 "$node DRAM-interface count"

    i=0
    while [ "$i" -lt 8 ]; do
        ctrl="system.mem_ctrls$i"
        dram="$ctrl.dram"
        assert_ini "$cfg" "$ctrl" type MemCtrl
        assert_ini "$cfg" "$ctrl" clk_domain system.fabric_clk_domain
        assert_ini "$cfg" "$ctrl" mem_sched_policy frfcfs
        assert_ini "$cfg" "$ctrl" write_high_thresh_perc 85
        assert_ini "$cfg" "$ctrl" write_low_thresh_perc 50
        assert_ini "$cfg" "$ctrl" min_writes_per_switch 16
        assert_ini "$cfg" "$ctrl" min_reads_per_switch 16
        assert_ini "$cfg" "$ctrl" static_frontend_latency 10000
        assert_ini "$cfg" "$ctrl" static_backend_latency 10000
        assert_ini "$cfg" "$ctrl" command_window 10000
        assert_ini "$cfg" "$dram" type DRAMInterface
        assert_ini "$cfg" "$dram" clk_domain system.fabric_clk_domain
        assert_ini "$cfg" "$dram" addr_mapping RoRaBaCoCh
        assert_ini "$cfg" "$dram" ranks_per_channel 1
        assert_ini "$cfg" "$dram" device_bus_width 8
        assert_ini "$cfg" "$dram" devices_per_rank 8
        assert_ini "$cfg" "$dram" device_size 1073741824
        assert_ini "$cfg" "$dram" device_rowbuffer_size 1024
        assert_ini "$cfg" "$dram" bank_groups_per_rank 4
        assert_ini "$cfg" "$dram" banks_per_rank 16
        assert_ini "$cfg" "$dram" burst_length 8
        assert_ini "$cfg" "$dram" read_buffer_size 64
        assert_ini "$cfg" "$dram" write_buffer_size 128
        assert_ini "$cfg" "$dram" page_policy open_adaptive
        assert_ini "$cfg" "$dram" max_accesses_per_row 16
        assert_ini "$cfg" "$dram" tCK 833
        assert_ini "$cfg" "$dram" tBURST 3332
        assert_ini "$cfg" "$dram" tRCD 14160
        assert_ini "$cfg" "$dram" tCL 14160
        assert_ini "$cfg" "$dram" tRP 14160
        assert_ini "$cfg" "$dram" tRAS 32000
        assert_ini "$cfg" "$dram" tCCD_L 5000
        assert_ini "$cfg" "$dram" tRRD 3332
        assert_ini "$cfg" "$dram" tRRD_L 4900
        assert_ini "$cfg" "$dram" tXAW 21000
        assert_ini "$cfg" "$dram" activation_limit 4
        assert_ini "$cfg" "$dram" tRFC 350000
        assert_ini "$cfg" "$dram" tREFI 7800000
        assert_ini "$cfg" "$dram" enable_dram_powerdown false
        assert_ini "$cfg" "$dram" two_cycle_activate false
        assert_ini "$cfg" "$dram" range \
            "2147483648:70866960384:$i:128:256:512"
        i=$((i + 1))
    done

    assert_ini_match "$cfg" system.workload command_line \
        '(^| )mem=8GB( |$)'
    assert_ini_match "$cfg" system.workload command_line \
        '(^| )openurma_cpu_switch=server_o3( |$)'
    assert_ini_match "$cfg" system.workload command_line \
        '(^| )openurma_bench_cpu=2( |$)'
}

validate_node_config node0
validate_node_config node1

validate_no_fatal_log() {
    node=$1
    log="$run_root/$node/gem5.log"
    [ -r "$log" ] || return
    checked
    if grep -Eiq 'fatal:|panic:|assertion .* failed|Aborted|peer closed|segmentation fault' "$log"; then
        evidence=$(grep -Ein 'fatal:|panic:|assertion .* failed|Aborted|peer closed|segmentation fault' \
            "$log" | sed -n '1,3p' | tr '\n' '; ')
        fail "$node gem5.log contains a fatal simulator failure: $evidence"
    fi
}

# A stale or crashed runtime must never be reported as a valid server profile,
# even when the caller requests only static checks.
validate_no_fatal_log node0
validate_no_fatal_log node1

runtime_absent() {
    message=$1
    if [ "$runtime_mode" = require ]; then
        fail "$message"
    else
        note "$message"
    fi
}

roi_field() {
    line=$1
    key=$2
    printf '%s\n' "$line" | awk -v wanted="$key" '
        {
            for (i = 1; i <= NF; ++i) {
                split($i, part, "=")
                if (part[1] == wanted) {
                    print part[2]
                    exit
                }
            }
        }
    '
}

validate_node_runtime() {
    node=$1
    log="$run_root/$node/gem5.log"
    stats="$run_root/$node/stats.txt"

    if [ ! -r "$log" ]; then
        runtime_absent "$node has no readable gem5.log yet; runtime O3 transition is not verified."
        return
    fi

    if ! grep -Fq '[single_node_fs_clean] switching all boot CPUs to ArmO3CPU' "$log"; then
        runtime_absent "$node has not logged the Atomic-to-O3 transition yet."
        return
    fi
    checked
    if ! grep -Fq '[single_node_fs_clean] ArmO3CPU server ROI model active' "$log"; then
        runtime_absent "$node began a CPU switch but has not logged O3 activation yet."
        return
    fi
    checked

    if [ ! -r "$stats" ]; then
        runtime_absent "$node entered O3, but stats.txt is not readable yet; O3 cycles are not verified."
        return
    fi

    for stat_name in system.switch_cpus2.numCycles \
        system.switch_cpus2.commitStats0.numInsts; do
        value=$(awk -v wanted="$stat_name" \
            '$1 == wanted { sample=$2 } END { if (sample != "") print sample }' \
            "$stats")
        checked
        case "$value" in
            ''|*[!0-9]*)
                runtime_absent "$node $stat_name has no numeric sample yet."
                ;;
            0)
                runtime_absent "$node $stat_name is still zero; benchmark CPU 2 has not executed the O3 ROI."
                ;;
        esac
    done

    roi_line=$(grep -F '[NIC_DMA_ROI ' "$log" | tail -n 1 || true)
    if [ -z "$roi_line" ]; then
        runtime_absent "$node has no NIC_DMA_ROI snapshot; timing DMA is not verified."
    else
        assert_eq "$(roi_field "$roi_line" scope)" roi \
            "$node final DMA diagnostic scope"
        for pair in idle:1 pending:0 failed:0 pte_failures:0 queue_errors:0 \
            roi_active:1 roi_failed:0 roi_atomic_chunks:0 \
            roi_pte_failures:0 roi_queue_errors:0; do
            key=${pair%%:*}
            expected=${pair#*:}
            value=$(roi_field "$roi_line" "$key")
            assert_eq "$value" "$expected" "$node final DMA ROI $key"
        done

        submitted=$(roi_field "$roi_line" roi_submitted)
        completed=$(roi_field "$roi_line" roi_completed)
        timing_chunks=$(roi_field "$roi_line" roi_timing_chunks)
        peak=$(roi_field "$roi_line" roi_peak_outstanding)
        walks=$(roi_field "$roi_line" roi_page_walks)
        pte_reads=$(roi_field "$roi_line" roi_pte_reads)
        hits=$(roi_field "$roi_line" roi_iotlb_hits)
        misses=$(roi_field "$roi_line" roi_iotlb_misses)

        for item in submitted completed timing_chunks peak walks pte_reads hits misses; do
            eval "value=\${$item}"
            checked
            case "$value" in
                ''|*[!0-9]*) fail "$node final DMA ROI $item is not numeric: ${value:-<missing>}" ;;
            esac
        done
        if case "$submitted:$completed:$timing_chunks:$peak:$walks:$pte_reads:$hits:$misses" in
            *[!0-9:]*) false ;;
            *) true ;;
        esac; then
            checked
            [ "$submitted" -gt 0 ] || fail "$node submitted no DMA requests"
            checked
            [ "$submitted" -eq "$completed" ] ||
                fail "$node DMA submitted/completed mismatch: $submitted/$completed"
            checked
            [ "$timing_chunks" -gt 0 ] || fail "$node issued no timing DMA chunks"
            checked
            [ "$peak" -gt 0 ] && [ "$peak" -le 16 ] ||
                fail "$node DMA peak outstanding is outside 1..16: $peak"
            checked
            [ "$walks" -gt 0 ] || fail "$node performed no timing page walks"
            checked
            [ "$misses" -eq "$walks" ] ||
                fail "$node IOTLB misses/page walks mismatch: $misses/$walks"
            checked
            [ "$pte_reads" -ge "$walks" ] && \
                [ "$pte_reads" -le $((walks * 4)) ] ||
                fail "$node PTE reads are outside 1..4 per walk: $pte_reads/$walks"
            checked
            [ "$hits" -gt 0 ] || fail "$node IOTLB recorded no hits"
        fi
    fi

    # A completed round trip is useful information, but it is not required:
    # during an interactive test the current valid state may still be O3.
    if grep -Fq '[single_node_fs_clean] AtomicSimpleCPU fast mode active' "$log"; then
        note "$node also records a completed O3-to-Atomic return."
    elif [ "$runtime_mode" = require ]; then
        fail "$node has not completed the O3-to-Atomic return."
    else
        note "$node records O3 entry; no return-to-Atomic marker is present (it may still be in O3)."
    fi
}

case "$runtime_mode" in
    skip)
        note "runtime evidence skipped by --no-runtime"
        ;;
    auto|require)
        validate_node_runtime node0
        validate_node_runtime node1
        ;;
esac

if [ "$errors" -ne 0 ]; then
    echo "FAIL: $errors error(s), $checks assertion(s), $notes note(s)." >&2
    exit 1
fi

echo "PASS: $checks assertion(s); server profile is internally consistent ($notes note(s))."
