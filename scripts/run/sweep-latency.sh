#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/runtime.sh
source "$script_dir/../runtime.sh"

die() { echo "sweep-latency.sh: $*" >&2; exit 2; }

usage() {
    cat <<'EOF'
usage: ./lab sweep-latency [OPTIONS] [SIZE ...]

Run a reproducible series of two-node send_lat measurements. With no SIZE
arguments the sweep includes powers of two plus the first receive-DMA,
inline-WQEBB, inline/non-inline, and non-inline DMA boundary points.

  --profile ctp-rm-send-imm-i128|legacy
  --samples N                 measured samples per size (default: 100)
  --sizes "2 16 17 32 ..."    alternate way to supply the size list
  --port PORT                 TCP setup port (default: 21115)
  --output-dir DIR            new result directory
  --roi-stats                 collect optional ROI stats in both guests
  --timeout SECONDS           timeout for each size (default: 300)
  --stagger SECONDS           rendezvous regression delay (default: 0; fastest)
  -h, --help

Environment equivalents: OPENURMA_LAT_PROFILE, OPENURMA_SWEEP_SAMPLES,
OPENURMA_SWEEP_SIZES, OPENURMA_LAT_PORT, OPENURMA_SWEEP_OUT,
OPENURMA_ROI_STATS, OPENURMA_LAT_TIMEOUT, and OPENURMA_LAT_STAGGER.
EOF
}

container="$OPENURMA_CONTAINER"
repo_root="$(cd "$script_dir/../.." && pwd)"
lab="${OPENURMA_LAB_ROOT:-$(ou_runtime_default_lab "$repo_root")}"
run_root="${OPENURMA_DUAL_OUT:-$lab/run-dual}"
profile="${OPENURMA_LAT_PROFILE:-ctp-rm-send-imm-i128}"
samples="${OPENURMA_SWEEP_SAMPLES:-100}"
sizes_text="${OPENURMA_SWEEP_SIZES:-2 4 8 16 17 32 64 65 80 81 128 129 192 193 256 512 1024 1025 2048 4096}"
port="${OPENURMA_LAT_PORT:-21115}"
roi_stats="${OPENURMA_ROI_STATS:-0}"
timeout="${OPENURMA_LAT_TIMEOUT:-300}"
stagger="${OPENURMA_LAT_STAGGER:-0}"
output_dir="${OPENURMA_SWEEP_OUT:-$repo_root/sweeps/$(date -u +%Y%m%dT%H%M%SZ)}"
positional_sizes=()

need_value() {
    (( $# >= 2 )) && [[ -n "$2" ]] || die "$1 requires a value"
}

while (( $# > 0 )); do
    case "$1" in
        --profile) need_value "$@"; profile=$2; shift 2 ;;
        --profile=*) profile=${1#*=}; shift ;;
        --samples) need_value "$@"; samples=$2; shift 2 ;;
        --samples=*) samples=${1#*=}; shift ;;
        --sizes) need_value "$@"; sizes_text=$2; shift 2 ;;
        --sizes=*) sizes_text=${1#*=}; shift ;;
        --port) need_value "$@"; port=$2; shift 2 ;;
        --port=*) port=${1#*=}; shift ;;
        --output-dir) need_value "$@"; output_dir=$2; shift 2 ;;
        --output-dir=*) output_dir=${1#*=}; shift ;;
        --roi-stats) roi_stats=1; shift ;;
        --no-roi-stats) roi_stats=0; shift ;;
        --timeout) need_value "$@"; timeout=$2; shift 2 ;;
        --timeout=*) timeout=${1#*=}; shift ;;
        --stagger) need_value "$@"; stagger=$2; shift 2 ;;
        --stagger=*) stagger=${1#*=}; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; while (( $# > 0 )); do positional_sizes+=("$1"); shift; done ;;
        -*) die "unknown option: $1 (try --help)" ;;
        *) positional_sizes+=("$1"); shift ;;
    esac
done

if (( ${#positional_sizes[@]} > 0 )); then
    sizes=("${positional_sizes[@]}")
else
    read -r -a sizes <<< "$sizes_text"
fi
(( ${#sizes[@]} > 0 )) || die "the size list is empty"
case "$profile" in
    ctp-rm-send-imm-i128|ctp-rm-send-imm-inline128) profile=ctp-rm-send-imm-i128 ;;
    legacy) ;;
    *) die "unknown profile '$profile'" ;;
esac
case "$samples:$port" in
    *[!0-9:]*|0:*|*:0) die "samples and port must be positive decimal integers" ;;
esac
(( port <= 65535 )) || die "port must be <= 65535"
case "$roi_stats" in 0|1) ;; *) die "OPENURMA_ROI_STATS must be 0 or 1" ;; esac
[[ "$timeout" =~ ^[0-9]+([.][0-9]+)?$ ]] || die "timeout must be a positive number"
[[ "$stagger" =~ ^[0-9]+([.][0-9]+)?$ ]] || die "stagger must be a non-negative number"
for size in "${sizes[@]}"; do
    case "$size" in ""|*[!0-9]*|0) die "invalid message size '$size'" ;; esac
    (( size <= 8088 )) || die "message size $size exceeds the 8088-byte peer-ring limit"
done

if [[ -e "$output_dir" ]]; then
    [[ -d "$output_dir" && -z "$(ls -A "$output_dir")" ]] ||
        die "output path already exists and is not an empty directory: $output_dir"
else
    mkdir -p "$output_dir"
fi

results="$output_dir/results.tsv"
printf '%s\n' $'profile\tnode\tbytes\titerations\tt_min_us\tt_max_us\tt_median_us\tt_avg_us\tt_stdev_us\tp99_us\tp99_9_us\tp99_99_us\tp99_999_us' >"$results"
{
    printf 'created_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'benchmark_profile=%s\n' "$profile"
    printf 'measured_samples_per_size=%s\n' "$samples"
    printf 'sizes_bytes=%s\n' "${sizes[*]}"
    printf 'port=%s\n' "$port"
    printf 'roi_stats=%s\n' "$roi_stats"
    printf 'timeout_seconds=%s\n' "$timeout"
    printf 'client_stagger_seconds=%s\n' "$stagger"
} >"$output_dir/sweep-manifest.txt"

ou_runtime_start
if ou_exec test -r "$run_root/run-manifest.txt"; then
    ou_exec cat "$run_root/run-manifest.txt" >"$output_dir/model-manifest.txt"
else
    printf '%s\n' "model manifest unavailable: $run_root/run-manifest.txt" >"$output_dir/model-manifest.txt"
fi

# Keep the array non-empty for macOS Bash 3.2: expanding an empty array under
# `set -u` is treated as an unbound variable by that shell.
roi_arg=(--no-roi-stats)
(( roi_stats == 0 )) || roi_arg=(--roi-stats)

stats_block_count() {
    ou_exec awk "/Begin Simulation Statistics/ { count++ } END { print count + 0 }" "$1"
}

extract_stats_block() {
    local stats_path=$1
    local wanted=$2
    ou_exec awk -v wanted="$wanted" '
        /Begin Simulation Statistics/ {
            block++
            capture = (block == wanted)
        }
        capture { print }
        /End Simulation Statistics/ && capture { exit }
    ' "$stats_path"
}

for size in "${sizes[@]}"; do
    label=$(printf '%04d' "$size")
    raw="$output_dir/size-${label}.raw.txt"
    parsed="$output_dir/size-${label}.tsv"
    if (( roi_stats )); then
        node0_stats="$run_root/node0/stats.txt"
        node1_stats="$run_root/node1/stats.txt"
        before_node0=$(stats_block_count "$node0_stats")
        before_node1=$(stats_block_count "$node1_stats")
    fi
    echo "[$size B] running $samples measured samples" >&2
    if "$script_dir/run-latency.sh" \
        --profile "$profile" --samples "$samples" --size "$size" --port "$port" \
        --format tsv --raw-output "$raw" --timeout "$timeout" --stagger "$stagger" \
        "${roi_arg[@]}" >"$parsed"; then
        sed -n '2,$p' "$parsed" >>"$results"
    else
        die "measurement failed at $size B; raw transcript retained in $raw"
    fi
    if (( roi_stats )); then
        for node in 0 1; do
            before_var="before_node$node"
            before=${!before_var}
            wanted=$((before + 1))
            stats_var="node${node}_stats"
            stats_path=${!stats_var}
            after=$(stats_block_count "$stats_path")
            (( after == wanted )) ||
                die "expected one new ROI stats block for node$node at $size B; found $((after - before))"
            extract_stats_block "$stats_path" "$wanted" \
                >"$output_dir/size-${label}-node${node}-roi-stats.txt"
        done
    fi
done

echo "Sweep complete: $results"
echo "Model parameters: $output_dir/model-manifest.txt"
echo "Raw UART transcripts: $output_dir/size-*.raw.txt"
(( roi_stats == 0 )) || echo "Per-size ROI stats: $output_dir/size-*-node*-roi-stats.txt"
