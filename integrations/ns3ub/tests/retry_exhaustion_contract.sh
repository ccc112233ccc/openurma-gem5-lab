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

"$peer" sender "$work/a.sock" "$work/a.shm" off ctp-exhaust >"$work/a.log" 2>&1 &
sender_pid=$!
"$peer" receiver "$work/b.sock" "$work/b.shm" off ctp-exhaust >"$work/b.log" 2>&1 &
receiver_pid=$!
for _ in $(seq 1 100); do
    [[ -S "$work/a.sock" && -S "$work/b.sock" ]] && break
    sleep 0.01
done
[[ -S "$work/a.sock" && -S "$work/b.sock" ]]

"$adapter" --sync off --ctp-retransmission on --ctp-rto-ps 1000000 \
    --ctp-max-retransmissions 2 --drop-all-ctp-requests \
    --endpoint "$work/a.sock,0x101" --endpoint "$work/b.sock,0x202" \
    >"$work/adapter.log" 2>&1 &
adapter_pid=$!

wait "$sender_pid"
kill "$receiver_pid" "$adapter_pid" 2>/dev/null || true
wait "$receiver_pid" 2>/dev/null || true
wait "$adapter_pid" 2>/dev/null || true

grep -q 'retry exhaustion returned RMA error PASS' "$work/a.log"
grep -q 'native_wqes_submitted=1' "$work/adapter.log"
grep -q 'native_wqes_completed=0' "$work/adapter.log"
grep -q 'native_wqes_failed=1' "$work/adapter.log"
grep -q 'ctp_retransmissions=2' "$work/adapter.log"
grep -q 'ctp_retransmission_exhausted=1' "$work/adapter.log"
grep -q 'injected_ctp_request_drops=3' "$work/adapter.log"
echo 'ns-3-UB CTP retry-exhaustion failure contract: PASS'
