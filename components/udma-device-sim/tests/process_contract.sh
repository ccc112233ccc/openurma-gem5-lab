#!/usr/bin/env bash
set -euo pipefail

device=$1
peer=$2
root=$3
host_socket="$root-host.sock"
net_socket="$root-net.sock"
shm="$root-shm"
device_log="$root-device.log"

rm -f "$host_socket" "$net_socket" "$shm" "$device_log"
cleanup() {
    [[ -z "${device_pid:-}" ]] || kill "$device_pid" 2>/dev/null || true
    wait "${device_pid:-0}" 2>/dev/null || true
    rm -f "$host_socket" "$net_socket" "$shm"
}
trap cleanup EXIT

"$device" --host-socket "$host_socket" --net-socket "$net_socket" \
    --shm "$shm" --sync off >"$device_log" 2>&1 &
device_pid=$!

for _ in $(seq 1 100); do
    [[ -S "$host_socket" && -S "$net_socket" ]] && break
    sleep 0.01
done
[[ -S "$host_socket" && -S "$net_socket" ]]

"$peer" net "$net_socket" &
net_pid=$!
"$peer" host "$host_socket"
wait "$net_pid"

kill "$device_pid" 2>/dev/null || true
wait "$device_pid" 2>/dev/null || true
device_pid=
grep -q "udma-device-sim: connected" "$device_log"
echo "three-process UDMA contract: PASS"
