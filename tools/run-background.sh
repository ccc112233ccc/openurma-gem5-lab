#!/usr/bin/env bash
set -euo pipefail

if (( $# < 3 )); then
    echo "usage: run-background.sh PIDFILE LOGFILE COMMAND [ARG ...]" >&2
    exit 2
fi

pidfile=$1
logfile=$2
shift 2

mkdir -p "$(dirname "$pidfile")" "$(dirname "$logfile")"
printf '%s\n' "$$" > "$pidfile"
exec "$@" > "$logfile" 2>&1
