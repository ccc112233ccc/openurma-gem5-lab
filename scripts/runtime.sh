#!/usr/bin/env bash
# Shared command backend for native Linux and Apple Silicon execution.

UBSIM_EXECUTION_MODE="${UBSIM_EXECUTION_MODE:-native}"

ubsim_runtime_validate() {
    case "$UBSIM_EXECUTION_MODE" in
        native)
            case "$(uname -s):$(uname -m)" in
                Linux:*|Darwin:arm64) ;;
                *)
                    echo "Native execution requires Linux or Apple Silicon macOS" >&2
                    return 2
                    ;;
            esac
            ;;
        *)
            echo "UBSIM_EXECUTION_MODE must be native" >&2
            return 2
            ;;
    esac
}

ubsim_runtime_default_lab() {
    printf '%s\n' "$1"
}

ubsim_runtime_start() {
    ubsim_runtime_validate
}

ubsim_exec() {
    "$@"
}

ubsim_exec_i() {
    "$@"
}

ubsim_exec_detached() {
    nohup "$@" </dev/null >/dev/null 2>&1 &
}

ubsim_exec_detached_env() {
    local assignments=()
    while (( $# )) && [[ "$1" != -- ]]; do
        assignments+=("$1")
        shift
    done
    [[ "${1:-}" == -- ]] || {
        echo "ubsim_exec_detached_env: missing -- separator" >&2
        return 2
    }
    shift
    nohup env "${assignments[@]}" "$@" </dev/null >/dev/null 2>&1 &
}
