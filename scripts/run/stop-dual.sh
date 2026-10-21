#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/runtime.sh
source "$script_dir/../runtime.sh"
repo_root="$(cd "$script_dir/../.." && pwd)"
lab="${UBSIM_LAB_ROOT:-$(ubsim_runtime_default_lab "$repo_root")}"
run_root="${UBSIM_DUAL_OUT:-$lab/run-dual}"

ubsim_runtime_validate

manifest="$run_root/run-manifest.txt"
network_backend="$(ubsim_exec awk -F= \
    '$1 == "network_backend" { print $2; exit }' "$manifest" 2>/dev/null || true)"
if [[ "$(uname -s)" == Darwin ]]; then
    switch_default="$lab/sources/ns-3-ub/build-macos/scratch/ns3.44-ub-net-adapter"
    udma_default="$lab/artifacts/udma-device-sim-build-darwin-arm64/udma-device-sim"
else
    switch_default="$lab/sources/ns-3-ub/build-linux/scratch/ns3.44-ub-net-adapter"
    runtime_arch="$(ubsim_exec uname -m)"
    [[ "$runtime_arch" != arm64 ]] || runtime_arch=aarch64
    udma_default="$lab/artifacts/udma-device-sim-build-linux-$runtime_arch/udma-device-sim"
fi
ub_switch_binary="${UBSIM_UB_SWITCH_BINARY:-$switch_default}"
udma_device_binary="${UBSIM_UDMA_DEVICE_BINARY:-$udma_default}"

stop_one() {
    label=$1
    pidfile=$2
    expected=$3
    ubsim_exec bash -c '
        label=$1
        pidfile=$2
        expected=$3
        test -r "$pidfile" || { echo "$label: no pid file"; exit 0; }
        pid=$(sed -n "1p" "$pidfile")
        case "$pid" in ""|*[!0-9]*) echo "$label: invalid pid file"; exit 0;; esac
        kill -0 "$pid" 2>/dev/null || { echo "$label: already stopped"; exit 0; }
        if test -r "/proc/$pid/cmdline"; then
            cmd=$(tr "\000" " " < "/proc/$pid/cmdline")
        else
            cmd=$(ps -ww -p "$pid" -o command=)
        fi
        printf "%s" "$cmd" | grep -Fq -- "$expected" || {
            echo "$label: refusing to signal reused/unexpected pid $pid" >&2
            exit 3
        }
        kill -TERM "$pid"
        echo "$label: sent TERM to pid $pid"
    ' _ "$label" "$pidfile" "$expected"
}

node_count="$(ubsim_exec awk -F= \
    '$1 == "node_count" { print $2; exit }' \
    "$run_root/run-manifest.txt" 2>/dev/null || true)"
node_count=${node_count:-2}
for ((node = 0; node < node_count; ++node)); do
    stop_one "node$node" "$run_root/node$node/gem5.pid" "$run_root/node$node"
done
for ((node = 0; node < node_count; ++node)); do
    stop_one "udma$node" "$run_root/udma-node$node/udma.pid" \
        "$udma_device_binary"
done
stop_one switch "$run_root/switch/gem5.pid" "$run_root/switch"
stop_one ub-switch "$run_root/ub-switch/gem5.pid" "$ub_switch_binary"
if ubsim_exec test -r "$run_root/oob-switch/relay.pid"; then
    stop_one oob-switch "$run_root/oob-switch/relay.pid" \
        "$lab/tools/ethernet_relay.py"
elif ubsim_exec test -r "$run_root/relay/relay.pid"; then
    stop_one relay "$run_root/relay/relay.pid" "$lab/tools/ethernet_relay.py"
else
    for ((pair = 0; pair < node_count / 2; ++pair)); do
        stop_one "relay$pair" "$run_root/relay$pair/relay.pid" \
            "$lab/tools/ethernet_relay.py"
    done
fi
echo "Any separate single-node gem5 session was left untouched."
