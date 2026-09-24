#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/runtime.sh
source "$script_dir/../runtime.sh"

die() { echo "run-node-pair-latency.sh: $*" >&2; exit 2; }

usage() {
    cat <<'EOF'
usage: ./lab latency-pair SERVER_NODE CLIENT_NODE [SAMPLES [SIZE [PORT]]]

Run official synchronized send_lat between any two nodes on the shared OOB and
UB switches. The client uses the server's 10.0.0.(node+1) control address;
the official resource exchange and TP setup carry the destination EID.
EOF
}

(( $# > 0 )) || { usage; exit 2; }
case "${1:-}" in -h|--help) usage; exit 0;; esac
(( $# >= 2 && $# <= 5 )) || { usage >&2; exit 2; }

server=$1
client=$2
samples=${3:-100}
size=${4:-128}
port=${5:-21115}
container="$OPENURMA_CONTAINER"
repo_root="$(cd "$script_dir/../.." && pwd)"
lab="${OPENURMA_LAB_ROOT:-$(ou_runtime_default_lab "$repo_root")}"
run_root="${OPENURMA_DUAL_OUT:-$lab/run-dual}"
uart0="${OPENURMA_DUAL_UART0:-3460}"
uart1="${OPENURMA_DUAL_UART1:-3470}"
profile="${OPENURMA_LAT_PROFILE:-ctp-rm-send-imm-i128}"
timeout="${OPENURMA_LAT_TIMEOUT:-600}"

case "$server:$client:$samples:$size:$port" in
    *[!0-9:]*) die "nodes, samples, size, and port must be decimal integers" ;;
esac
(( server != client )) || die "server and client must be different nodes"
(( samples > 0 && size > 0 && size <= 8088 && port > 0 && port <= 65535 )) ||
    die "require samples>0, size=1..8088, and port=1..65535"

ou_runtime_start
ou_exec test -e "$run_root/sync.ready" ||
    die "measurement setup is not ready; run './lab sync' first"
node_count=$(ou_exec awk -F= '$1 == "node_count" {print $2; exit}' \
    "$run_root/run-manifest.txt")
[[ "$node_count" =~ ^[0-9]+$ ]] || die "invalid node count in run manifest"
(( server < node_count && client < node_count )) ||
    die "node must be in range 0..$((node_count - 1))"

uart_stride=$((uart1 - uart0))
server_uart=$((uart0 + server * uart_stride))
client_uart=$((uart0 + client * uart_stride))
server_ip="10.0.0.$((server + 1))"
roi_arg=""
[[ "${OPENURMA_ROI_STATS:-0}" == 0 ]] || roi_arg=--roi-stats
server_command="ou-lat-server --profile $profile $roi_arg $samples $size $port"
client_command="ou-lat-client --profile $profile $roi_arg $samples $size $port $server_ip"

echo "Running node$client -> node$server: OOB=$server_ip, EID selected by official TP setup"
ou_exec python3 "$lab/tools/dual_serial_command.py" \
    --ports "$server_uart" "$client_uart" \
    --commands "$server_command" "$client_command" \
    --timeout "$timeout" --prompt-kick-after 1 --full-output
