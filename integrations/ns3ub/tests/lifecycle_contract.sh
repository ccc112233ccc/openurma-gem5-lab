#!/usr/bin/env bash
set -euo pipefail
adapter=$1
peer=$2
work=$3
# Each invocation uses a new directory; never delete another experiment's data.
mkdir "$work"
pids=()
cleanup() {
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
}
trap cleanup EXIT
"$peer" sender "$work/a.sock" "$work/a.shm" off lifecycle-idle >"$work/a.log" 2>&1 &
pids+=("$!"); sender_pid=$!
"$peer" receiver "$work/b.sock" "$work/b.shm" off lifecycle-idle >"$work/b.log" 2>&1 &
pids+=("$!"); receiver_pid=$!
for _ in $(seq 1 100); do
    [[ -S "$work/a.sock" && -S "$work/b.sock" ]] && break
    sleep 0.01
done
[[ -S "$work/a.sock" && -S "$work/b.sock" ]]
"$adapter" --sync off --lifecycle-sync \
    --endpoint "$work/a.sock,0x101" --endpoint "$work/b.sock,0x202" \
    >"$work/adapter.log" 2>&1 &
pids+=("$!"); adapter_pid=$!
wait "$sender_pid"
wait "$receiver_pid"
kill "$adapter_pid"
wait "$adapter_pid"
python3 - "$work" <<'PY'
from pathlib import Path
import re, sys
root = Path(sys.argv[1])
expected = [('1', '1'), ('2', '0'), ('3', '1'), ('4', '0')]
for name in ('a', 'b'):
    text = (root / f'{name}.log').read_text()
    assert 'LIFECYCLE_IDLE PASS' in text, text
    assert re.findall(r'LIFECYCLE_COMMIT generation=(\d+) active=(\d+)', text) == expected, text
text = (root / 'adapter.log').read_text()
fences = [line for line in text.splitlines() if '[NS3_UB_NET_FENCE]' in line]
assert len(fences) == 4, text
assert [tuple(re.search(r'generation=(\d+) active=(\d+)', line).groups()) for line in fences] == expected
assert all('native_event_queue_empty=0' in line and 'endpoints=2' in line for line in fences), fences
stats = [line for line in text.splitlines() if '[NS3_UB_NET_STATS]' in line]
assert len(stats) == 1, text
fields = dict(re.findall(r'(\w+)=(\d+)', stats[0]))
assert fields['native_wqes_submitted'] == '0', fields
assert fields['native_wqes_failed'] == '0', fields
assert fields['native_event_queue_empty'] == '0', fields
assert int(fields['native_orphan_events_processed']) >= 2, fields
print('ns-3-UB idle lifecycle with preserved native watchdogs: PASS')
PY
