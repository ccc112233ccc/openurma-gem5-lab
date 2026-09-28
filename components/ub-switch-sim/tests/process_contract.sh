#!/usr/bin/env bash
set -euo pipefail

switch_bin="$1"
peer_bin="$2"
work="$3"
sync_mode="${4:-off}"
mkdir -p "$work"
rm -f "$work"/*.sock "$work"/*.shm "$work"/*.log

cleanup() {
    jobs -pr | xargs kill 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

"$peer_bin" sender "$work/a.sock" "$work/a.shm" "$sync_mode" >"$work/a.log" 2>&1 &
sender_pid=$!
"$peer_bin" receiver "$work/b.sock" "$work/b.shm" "$sync_mode" >"$work/b.log" 2>&1 &
receiver_pid=$!

for _ in $(seq 1 100); do
    [[ -S "$work/a.sock" && -S "$work/b.sock" ]] && break
    sleep 0.01
done

"$switch_bin" --sync "$sync_mode" \
    --endpoint "$work/a.sock,0x101" \
    --endpoint "$work/b.sock,0x202" >"$work/switch.log" 2>&1 &
switch_pid=$!

wait "$sender_pid"
wait "$receiver_pid"
kill "$switch_pid" 2>/dev/null || true
wait "$switch_pid" 2>/dev/null || true

grep -q 'routed reply and link-state PASS' "$work/a.log"
grep -q 'routed request and reply PASS' "$work/b.log"
grep -q 'connected 2 UB-NET endpoints' "$work/switch.log"
echo 'ub-switch process contract: PASS'
