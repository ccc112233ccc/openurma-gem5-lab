#!/usr/bin/env bash
set -euo pipefail

die() { echo "run-paired-latency.sh: $*" >&2; exit 2; }

usage() {
    cat <<'EOF'
usage: run-paired-latency.sh [OPTIONS] [SAMPLES [MESSAGE_BYTES [BASE_PORT]]]

Run one synchronized send_lat session per adjacent node pair (0<->1, 2<->3,
...) at the same time.  The node count is read from run-manifest.txt.

  --samples N                 measured samples per pair (default: 100)
  --size BYTES                message bytes (default: 128)
  --base-port PORT            pair 0 port; pair N uses PORT+N (default: 21115)
  --profile NAME              benchmark profile (default: ctp-rm-send-imm-i128)
  --roi-stats                 dump gem5 ROI statistics
  --raw-output FILE           retain the complete multi-UART transcript
  --timeout SECONDS           host command timeout (default: 600)
  -h, --help
EOF
}

container="${OPENURMA_CONTAINER:-openurma-gem5-lab}"
lab="${OPENURMA_LAB_ROOT:-/workspace/openurma-gem5-lab}"
run_root="${OPENURMA_DUAL_OUT:-$lab/run-dual}"
uart0="${OPENURMA_DUAL_UART0:-3460}"
uart1="${OPENURMA_DUAL_UART1:-3470}"
profile="${OPENURMA_LAT_PROFILE:-ctp-rm-send-imm-i128}"
samples="${OPENURMA_LAT_SAMPLES:-100}"
size="${OPENURMA_LAT_SIZE:-128}"
base_port="${OPENURMA_LAT_PORT:-21115}"
roi_stats="${OPENURMA_ROI_STATS:-0}"
raw_output="${OPENURMA_LAT_RAW_OUTPUT:-}"
timeout="${OPENURMA_LAT_TIMEOUT:-600}"
positional=()

need_value() { (( $# >= 2 )) && [[ -n "$2" ]] || die "$1 requires a value"; }
while (( $# > 0 )); do
    case "$1" in
        --samples) need_value "$@"; samples=$2; shift 2 ;;
        --samples=*) samples=${1#*=}; shift ;;
        --size) need_value "$@"; size=$2; shift 2 ;;
        --size=*) size=${1#*=}; shift ;;
        --base-port|--port) need_value "$@"; base_port=$2; shift 2 ;;
        --base-port=*|--port=*) base_port=${1#*=}; shift ;;
        --profile) need_value "$@"; profile=$2; shift 2 ;;
        --profile=*) profile=${1#*=}; shift ;;
        --roi-stats) roi_stats=1; shift ;;
        --no-roi-stats) roi_stats=0; shift ;;
        --raw-output) need_value "$@"; raw_output=$2; shift 2 ;;
        --raw-output=*) raw_output=${1#*=}; shift ;;
        --timeout) need_value "$@"; timeout=$2; shift 2 ;;
        --timeout=*) timeout=${1#*=}; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) die "unknown option: $1" ;;
        *) positional+=("$1"); shift ;;
    esac
done

(( ${#positional[@]} <= 3 )) || die "too many positional arguments"
(( ${#positional[@]} < 1 )) || samples=${positional[0]}
(( ${#positional[@]} < 2 )) || size=${positional[1]}
(( ${#positional[@]} < 3 )) || base_port=${positional[2]}

case "$samples:$size:$base_port:$uart0:$uart1" in
    *[!0-9:]*|0:*|*:0:*|*:*:0:*|*:*:*:0:*|*:*:*:*:0)
        die "samples, size, base port, and UART ports must be positive integers" ;;
esac
(( size <= 8088 )) || die "message size must be <= 8088 bytes"
case "$roi_stats" in 0|1) ;; *) die "roi-stats must be 0 or 1" ;; esac

docker exec "$container" test -e "$run_root/sync.ready" ||
    die "measurement setup is not ready; run sync-dual.sh first"
node_count=$(docker exec "$container" awk -F= '$1 == "node_count" {print $2; exit}' \
    "$run_root/run-manifest.txt")
[[ "$node_count" =~ ^[0-9]+$ ]] && (( node_count >= 2 && node_count % 2 == 0 )) ||
    die "manifest node_count must be an even integer >= 2"
pair_count=$((node_count / 2))
(( base_port + pair_count - 1 <= 65535 )) || die "pair ports exceed 65535"

uart_stride=$((uart1 - uart0))
uart_ports=()
commands=()
guest_roi_arg=""
(( roi_stats == 0 )) || guest_roi_arg="--roi-stats"
for ((node = 0; node < node_count; ++node)); do
    pair=$((node / 2))
    port=$((base_port + pair))
    role=client
    (( node % 2 == 0 )) && role=server
    uart_ports+=("$((uart0 + node * uart_stride))")
    if [[ "$role" == server ]]; then
        commands+=("ou-lat-server --profile $profile $guest_roi_arg $samples $size $port")
    else
        server_ip="10.0.0.$node"
        commands+=("ou-lat-client --profile $profile $guest_roi_arg $samples $size $port $server_ip")
    fi
done

transcript=$(mktemp "${TMPDIR:-/tmp}/openurma-paired-latency.XXXXXX")
trap 'rm -f "$transcript"' EXIT
set +e
docker exec "$container" python3 "$lab/tools/dual_serial_command.py" \
    --ports "${uart_ports[@]}" --commands "${commands[@]}" \
    --timeout "$timeout" --prompt-kick-after 1 --full-output >"$transcript" 2>&1
command_rc=$?
set -e
[[ -z "$raw_output" ]] || { mkdir -p "$(dirname "$raw_output")"; cp "$transcript" "$raw_output"; }
(( command_rc == 0 )) || { cat "$transcript" >&2; exit "$command_rc"; }

echo "Concurrent paired send_lat: nodes=$node_count pairs=$pair_count samples=$samples size=$size"
cat "$transcript"
