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

"$peer" sender "$work/a.sock" "$work/a.shm" off ctp-order >"$work/a.log" 2>&1 &
sender_pid=$!
"$peer" receiver "$work/b.sock" "$work/b.shm" off ctp-order >"$work/b.log" 2>&1 &
receiver_pid=$!
for _ in $(seq 1 100); do
    [[ -S "$work/a.sock" && -S "$work/b.sock" ]] && break
    sleep 0.01
done
[[ -S "$work/a.sock" && -S "$work/b.sock" ]]

"$adapter" --sync off --rate-gbps 2 --inject-ctp-cnp-after-first-segment \
    --endpoint "$work/a.sock,0x101" --endpoint "$work/b.sock,0x202" \
    >"$work/adapter.log" 2>&1 &
adapter_pid=$!
wait "$receiver_pid"
kill "$sender_pid" "$adapter_pid" 2>/dev/null || true
wait "$sender_pid" "$adapter_pid" 2>/dev/null || true

grep -q 'native CTP preserved NO/RO/SO ordering PASS' "$work/b.log"
grep -q 'ctp_congestion_rate_cuts=1' "$work/adapter.log"
span=$(sed -n 's/.*ctp_native_segment_send_span_ps=\([0-9][0-9]*\).*/\1/p' \
    "$work/adapter.log")
[[ -n "$span" && "$span" -ge 256000 ]]
grep -q 'ns3_runtime_drops=0' "$work/adapter.log"
echo "ns-3-UB native CTP CNP pacing contract: PASS (span_ps=$span)"
