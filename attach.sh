#!/usr/bin/env bash
set -euo pipefail

container="${OPENURMA_CONTAINER:-openurma-repro-20260909}"
m5term="${OPENURMA_M5TERM:-/workspace/openurma-gem5-lab/gem5/util/term/m5term}"
port="${OPENURMA_M5TERM_PORT:-3456}"
takeover="${OPENURMA_ATTACH_TAKEOVER:-1}"

case "$takeover" in
    0|1) ;;
    *)
        echo "OPENURMA_ATTACH_TAKEOVER must be 0 or 1" >&2
        exit 2
        ;;
esac

export DOCKER_CLI_HINTS=false

docker start "$container" >/dev/null
if ! docker exec "$container" test -x "$m5term"; then
    echo "m5term is not built at $m5term" >&2
    echo "Build it with: docker exec $container make -C /workspace/openurma-gem5-lab/gem5/util/term" >&2
    exit 2
fi

# A Docker exec process can survive when its host terminal window is closed,
# leaving gem5's single-client Terminal socket occupied.  Reclaim only an
# exact m5term process for this UART.  This never touches gem5 or another
# node's console.  Set OPENURMA_ATTACH_TAKEOVER=0 to protect an intentional
# attachment in another host terminal.
existing_pids=$(
    docker exec "$container" ps -eo pid=,args= |
        awk -v wanted="$m5term localhost $port" '
            {
                pid = $1
                $1 = ""
                sub(/^[[:space:]]+/, "")
                if ($0 == wanted)
                    print pid
            }
        '
)

if [ -n "$existing_pids" ]; then
    if [ "$takeover" -eq 0 ]; then
        echo "UART $port already has an m5term client (PID(s): $existing_pids)." >&2
        echo "Detach it with ~. or rerun without OPENURMA_ATTACH_TAKEOVER=0." >&2
        exit 1
    fi

    echo "Reclaiming previous m5term session on UART $port (PID(s): $existing_pids)"
    for pid in $existing_pids; do
        case "$pid" in
            ''|*[!0-9]*) continue ;;
        esac
        docker exec "$container" kill -TERM "$pid" 2>/dev/null || true
    done

    for _attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        still_running=0
        for pid in $existing_pids; do
            if docker exec "$container" kill -0 "$pid" 2>/dev/null; then
                still_running=1
            fi
        done
        [ "$still_running" -eq 0 ] && break
        sleep 0.05
    done
fi

echo "Connecting to the gem5 PL011 console on localhost:$port"
echo "Type ~. to detach without stopping the simulated machine."
exec docker exec -it "$container" "$m5term" localhost "$port"
