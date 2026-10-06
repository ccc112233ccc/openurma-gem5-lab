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

"$peer" sender "$work/a.sock" "$work/a.shm" off ctp-retrans \
    >"$work/a.log" 2>&1 &
sender_pid=$!
"$peer" receiver "$work/b.sock" "$work/b.shm" off ctp-retrans \
    >"$work/b.log" 2>&1 &
receiver_pid=$!

for _ in $(seq 1 100); do
    [[ -S "$work/a.sock" && -S "$work/b.sock" ]] && break
    sleep 0.01
done
[[ -S "$work/a.sock" && -S "$work/b.sock" ]]

"$adapter" --sync off --ctp-retransmission on --ctp-rto-ps 1000000 \
    --drop-first-ctp-request \
    --endpoint "$work/a.sock,0x101" \
    --endpoint "$work/b.sock,0x202" >"$work/adapter.log" 2>&1 &
adapter_pid=$!

wait "$sender_pid"
wait "$receiver_pid"
kill "$adapter_pid" 2>/dev/null || true
wait "$adapter_pid" 2>/dev/null || true

grep -q 'CTP WRITE completed after injected loss PASS' "$work/a.log"
grep -q 'executed one retransmitted CTP WRITE PASS' "$work/b.log"
grep -q 'native_wqes_submitted=1' "$work/adapter.log"
grep -q 'native_wqes_completed=1' "$work/adapter.log"
grep -q 'ctp_retransmissions=1' "$work/adapter.log"
grep -q 'ctp_retransmission_exhausted=0' "$work/adapter.log"
grep -q 'injected_ctp_request_drops=1' "$work/adapter.log"
grep -q 'ns3_runtime_drops=0' "$work/adapter.log"
echo 'ns-3-UB native CTP timeout retransmission contract: PASS'
