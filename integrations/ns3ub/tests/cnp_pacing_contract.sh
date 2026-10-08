#!/usr/bin/env bash
set -euo pipefail

adapter=$1
peer=$2
root=$3
for mode in baseline cnp; do
work="$root/$mode"
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

args=(--sync off --rate-gbps 2)
[[ "$mode" != cnp ]] || args+=(--inject-ctp-cnp-after-first-segment)
"$adapter" "${args[@]}" \
    --endpoint "$work/a.sock,0x101" --endpoint "$work/b.sock,0x202" \
    >"$work/adapter.log" 2>&1 &
adapter_pid=$!
wait "$receiver_pid"
kill "$sender_pid" "$adapter_pid" 2>/dev/null || true
wait "$sender_pid" "$adapter_pid" 2>/dev/null || true

grep -q 'native CTP preserved NO/RO/SO ordering PASS' "$work/b.log"
cuts=0
[[ "$mode" != cnp ]] || cuts=1
grep -q "ctp_congestion_rate_cuts=$cuts" "$work/adapter.log"
gap=$(sed -n 's/.*ctp_second_segment_gap_ps=\([0-9][0-9]*\).*/\1/p' \
    "$work/adapter.log")
[[ -n "$gap" ]]
if [[ "$mode" == baseline ]]; then baseline_gap=$gap; else cnp_gap=$gap; fi
grep -q 'ns3_runtime_drops=0' "$work/adapter.log"
done
[[ "$baseline_gap" -eq 0 && "$cnp_gap" -eq 256000 ]]
echo "ns-3-UB native CTP CNP pacing contract: PASS (baseline_gap_ps=$baseline_gap cnp_gap_ps=$cnp_gap)"
