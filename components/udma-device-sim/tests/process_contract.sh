#!/usr/bin/env bash
set -euo pipefail

device=$1
peer=$2
root=$3
temporary_root=
# Darwin limits AF_UNIX paths to roughly 104 bytes.  CMake build directories
# can easily exceed that, so relocate only the ephemeral contract endpoints.
if (( ${#root} > 60 )); then
    temporary_root=$(mktemp -d "${TMPDIR:-/tmp}/ubsim-udma.XXXXXX")
    root="$temporary_root/c"
fi
host_socket="$root-host.sock"
net_socket="$root-net.sock"
shm="$root-shm"
device_log="$root-device.log"

rm -f "$host_socket" "$net_socket" "$shm" "$device_log"
cleanup() {
    [[ -z "${device_pid:-}" ]] || kill "$device_pid" 2>/dev/null || true
    wait "${device_pid:-0}" 2>/dev/null || true
    rm -f "$host_socket" "$net_socket" "$shm"
    [[ -z "$temporary_root" ]] || rmdir "$temporary_root" 2>/dev/null || true
}
trap cleanup EXIT

"$device" --host-socket "$host_socket" --net-socket "$net_socket" \
    --shm "$shm" --sync off --test-abi >"$device_log" 2>&1 &
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
virtual_ps=$(sed -n 's/.*\[UDMA_PROFILE\].* virtual_ps=\([0-9][0-9]*\).*/\1/p' "$device_log")
[[ -n "$virtual_ps" && "$virtual_ps" -ge 5000000000 ]]
echo "three-process UDMA contract: PASS"
