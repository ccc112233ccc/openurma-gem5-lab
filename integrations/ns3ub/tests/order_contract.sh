#!/usr/bin/env bash
set -euo pipefail

adapter=$1
peer=$2
work=$3
mkdir -p "$work"
rm -f "$work"/*.sock "$work"/*.shm "$work"/*.log

cleanup() {
    jobs -pr | xargs kill 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

"$peer" sender "$work/a.sock" "$work/a.shm" off ctp-order \
    >"$work/a.log" 2>&1 &
sender_pid=$!
"$peer" receiver "$work/b.sock" "$work/b.shm" off ctp-order \
    >"$work/b.log" 2>&1 &
receiver_pid=$!

for _ in $(seq 1 100); do
    [[ -S "$work/a.sock" && -S "$work/b.sock" ]] && break
    sleep 0.01
done
[[ -S "$work/a.sock" && -S "$work/b.sock" ]]

"$adapter" --sync off \
    --endpoint "$work/a.sock,0x101" \
    --endpoint "$work/b.sock,0x202" >"$work/adapter.log" 2>&1 &
adapter_pid=$!

wait "$receiver_pid"
kill "$sender_pid" 2>/dev/null || true
wait "$sender_pid" 2>/dev/null || true
kill "$adapter_pid" 2>/dev/null || true
wait "$adapter_pid" 2>/dev/null || true

grep -q 'native CTP preserved NO/RO/SO ordering PASS' "$work/b.log"
grep -q 'native_wqes_submitted=3' "$work/adapter.log"
grep -q 'native_wqes_completed=3' "$work/adapter.log"
grep -q 'native_order_no=1' "$work/adapter.log"
grep -q 'native_order_relax=1' "$work/adapter.log"
grep -q 'native_order_strong=1' "$work/adapter.log"
grep -q 'ns3_runtime_drops=0' "$work/adapter.log"
echo 'ns-3-UB native CTP NO/RO/SO contract: PASS'
