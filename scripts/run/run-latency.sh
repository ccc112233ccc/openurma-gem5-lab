#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/runtime.sh
source "$script_dir/../runtime.sh"

die() { echo "run-latency.sh: $*" >&2; exit 2; }

usage() {
    cat <<'EOF'
usage: ./lab latency [OPTIONS] [SAMPLES [MESSAGE_BYTES [PORT]]]

Run synchronized send_lat on the two guests. The default benchmark profile is
the real-machine comparison shape: CTP, RM, SEND_IMM, inline threshold 128 B,
and one Jetty.

  --profile ctp-rm-send-imm-i128
  --samples N                 measured samples (default: 100)
  --size BYTES                message bytes (default: 128)
  --port PORT                 TCP setup port (default: 21115)
  --roi-stats                 set UBSIM_ROI_STATS=1 in both guests
  --format human|tsv          transcript plus TSV summary, or TSV only
  --raw-output FILE           retain the complete dual-UART transcript
  --timeout SECONDS           host command timeout (default: 300)
  --stagger SECONDS           rendezvous regression delay (default: 0; fastest)
  -h, --help

Environment equivalents: UBSIM_LAT_PROFILE, UBSIM_LAT_SAMPLES,
UBSIM_LAT_SIZE, UBSIM_LAT_PORT, UBSIM_ROI_STATS,
UBSIM_LAT_FORMAT, UBSIM_LAT_RAW_OUTPUT, UBSIM_LAT_TIMEOUT, and
UBSIM_LAT_STAGGER.
EOF
}

container="$UBSIM_CONTAINER"
repo_root="$(cd "$script_dir/../.." && pwd)"
lab="${UBSIM_LAB_ROOT:-$(ubsim_runtime_default_lab "$repo_root")}"
run_root="${UBSIM_DUAL_OUT:-$lab/run-dual}"
uart0="${UBSIM_DUAL_UART0:-3460}"
uart1="${UBSIM_DUAL_UART1:-3470}"
profile="${UBSIM_LAT_PROFILE:-ctp-rm-send-imm-i128}"
samples="${UBSIM_LAT_SAMPLES:-100}"
size="${UBSIM_LAT_SIZE:-128}"
port="${UBSIM_LAT_PORT:-21115}"
roi_stats="${UBSIM_ROI_STATS:-0}"
output_format="${UBSIM_LAT_FORMAT:-human}"
raw_output="${UBSIM_LAT_RAW_OUTPUT:-}"
timeout="${UBSIM_LAT_TIMEOUT:-300}"
stagger="${UBSIM_LAT_STAGGER:-0}"
positional=()

need_value() {
    (( $# >= 2 )) && [[ -n "$2" ]] || die "$1 requires a value"
}

while (( $# > 0 )); do
    case "$1" in
        --profile) need_value "$@"; profile=$2; shift 2 ;;
        --profile=*) profile=${1#*=}; shift ;;
        --samples) need_value "$@"; samples=$2; shift 2 ;;
        --samples=*) samples=${1#*=}; shift ;;
        --size) need_value "$@"; size=$2; shift 2 ;;
        --size=*) size=${1#*=}; shift ;;
        --port) need_value "$@"; port=$2; shift 2 ;;
        --port=*) port=${1#*=}; shift ;;
        --roi-stats) roi_stats=1; shift ;;
        --no-roi-stats) roi_stats=0; shift ;;
        --format) need_value "$@"; output_format=$2; shift 2 ;;
        --format=*) output_format=${1#*=}; shift ;;
        --raw-output) need_value "$@"; raw_output=$2; shift 2 ;;
        --raw-output=*) raw_output=${1#*=}; shift ;;
        --timeout) need_value "$@"; timeout=$2; shift 2 ;;
        --timeout=*) timeout=${1#*=}; shift ;;
        --stagger) need_value "$@"; stagger=$2; shift 2 ;;
        --stagger=*) stagger=${1#*=}; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; while (( $# > 0 )); do positional+=("$1"); shift; done ;;
        -*) die "unknown option: $1 (try --help)" ;;
        *) positional+=("$1"); shift ;;
    esac
done

(( ${#positional[@]} <= 3 )) || die "too many positional arguments (try --help)"
(( ${#positional[@]} < 1 )) || samples=${positional[0]}
(( ${#positional[@]} < 2 )) || size=${positional[1]}
(( ${#positional[@]} < 3 )) || port=${positional[2]}

case "$profile" in
    ctp-rm-send-imm-i128|ctp-rm-send-imm-inline128) profile=ctp-rm-send-imm-i128 ;;
    *) die "unknown profile '$profile'" ;;
esac
case "$samples:$size:$port:$uart0:$uart1" in
    *[!0-9:]*|0:*|*:0:*|*:*:0:*|*:*:*:0:*|*:*:*:*:0)
        die "samples, size, port, and UART ports must be positive decimal integers"
        ;;
esac
(( size <= 8088 )) || die "message size must be <= 8088 bytes for the current peer-ring slot"
(( port <= 65535 )) || die "port must be <= 65535"
case "$roi_stats" in 0|1) ;; *) die "UBSIM_ROI_STATS must be 0 or 1" ;; esac
case "$output_format" in human|tsv) ;; *) die "format must be human or tsv" ;; esac
[[ "$timeout" =~ ^[0-9]+([.][0-9]+)?$ ]] || die "timeout must be a positive number"
[[ "$stagger" =~ ^[0-9]+([.][0-9]+)?$ ]] || die "stagger must be a non-negative number"

ubsim_runtime_start
ubsim_exec test -e "$run_root/sync.ready" || {
    echo "dual-node measurement setup is not ready; run './lab sync' first" >&2
    exit 1
}

guest_roi_arg=""
(( roi_stats == 0 )) || guest_roi_arg="--roi-stats"
server_command="ubsim-lat-server --profile $profile $guest_roi_arg $samples $size $port"
client_command="ubsim-lat-client --profile $profile $guest_roi_arg $samples $size $port"
transcript=$(mktemp "${TMPDIR:-/tmp}/ubsim-latency.XXXXXX")
trap 'rm -f "$transcript"' EXIT

set +e
wall_started_ns=$(python3 -c 'import time; print(time.monotonic_ns())')
ubsim_exec python3 "$lab/tools/dual_serial_command.py" \
    --ports "$uart0" "$uart1" \
    --commands "$server_command" "$client_command" \
    --stagger "$stagger" --timeout "$timeout" --prompt-kick-after 1 --full-output \
    >"$transcript" 2>&1
command_rc=$?
wall_finished_ns=$(python3 -c 'import time; print(time.monotonic_ns())')
set -e
wall_seconds=$(awk -v start="$wall_started_ns" -v finish="$wall_finished_ns" \
    'BEGIN { printf "%.6f", (finish - start) / 1000000000 }')

if [[ -n "$raw_output" ]]; then
    mkdir -p "$(dirname "$raw_output")"
    cp "$transcript" "$raw_output"
fi
if (( command_rc != 0 )); then
    cat "$transcript" >&2
    exit "$command_rc"
fi

rows=$(awk -v profile="$profile" -v uart0="$uart0" -v uart1="$uart1" \
    -v expected="$size" -v wall="$wall_seconds" '
    /^--- UART [0-9]+ ---$/ { uart=$3; next }
    $1 == expected && $1 ~ /^[0-9]+$/ && NF >= 11 {
        node=(uart == uart0 ? "node0" : (uart == uart1 ? "node1" : "unknown"))
        row[node]=sprintf("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s", \
            profile, node, $1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, wall)
    }
    END {
        if ("node0" in row) print row["node0"]
        if ("node1" in row) print row["node1"]
    }
' "$transcript")
row_count=$(printf '%s\n' "$rows" | awk 'NF { count++ } END { print count + 0 }')
if (( row_count != 2 )); then
    cat "$transcript" >&2
    die "expected one result from each node, parsed $row_count"
fi

if [[ "$output_format" == human ]]; then
    echo "Running synchronized send_lat: profile=$profile measured_samples=$samples size=$size bytes port=$port roi_stats=$roi_stats"
    echo "Host wall time: ${wall_seconds} s"
    echo "node0 is the server; node1 is the client"
    cat "$transcript"
    echo
    echo "Parsed result (tab-separated):"
fi
printf '%s\n' $'profile\tnode\tbytes\titerations\tt_min_us\tt_max_us\tt_median_us\tt_avg_us\tt_stdev_us\tp99_us\tp99_9_us\tp99_99_us\tp99_999_us\thost_wall_seconds'
printf '%s\n' "$rows"
