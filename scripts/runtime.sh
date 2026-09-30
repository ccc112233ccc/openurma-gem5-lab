#!/usr/bin/env bash
# Shared command backend for native Linux and Docker-hosted execution.

UBSIM_EXECUTION_MODE="${UBSIM_EXECUTION_MODE:-docker}"
UBSIM_CONTAINER="${UBSIM_CONTAINER:-ubsim-gem5-lab}"

ubsim_runtime_validate() {
    case "$UBSIM_EXECUTION_MODE" in
        docker)
            command -v docker >/dev/null || {
                echo "Docker execution requested but docker is unavailable" >&2
                return 2
            }
            ;;
        native)
            [[ "$(uname -s)" == Linux ]] || {
                echo "Native execution requires Linux" >&2
                return 2
            }
            ;;
        *)
            echo "UBSIM_EXECUTION_MODE must be docker or native" >&2
            return 2
            ;;
    esac
}

ubsim_runtime_default_lab() {
    local host_lab=$1
    if [[ "$UBSIM_EXECUTION_MODE" == docker ]]; then
        printf '%s\n' /workspace/ubsim-gem5-lab
    else
        printf '%s\n' "$host_lab"
    fi
}

ubsim_runtime_start() {
    ubsim_runtime_validate
    if [[ "$UBSIM_EXECUTION_MODE" == docker ]]; then
        docker start "$UBSIM_CONTAINER" >/dev/null
    fi
}

ubsim_exec() {
    if [[ "$UBSIM_EXECUTION_MODE" == docker ]]; then
        docker exec "$UBSIM_CONTAINER" "$@"
    else
        "$@"
    fi
}

ubsim_exec_i() {
    if [[ "$UBSIM_EXECUTION_MODE" == docker ]]; then
        docker exec -i "$UBSIM_CONTAINER" "$@"
    else
        "$@"
    fi
}

ubsim_exec_detached() {
    if [[ "$UBSIM_EXECUTION_MODE" == docker ]]; then
        docker exec -d "$UBSIM_CONTAINER" "$@"
    else
        nohup "$@" </dev/null >/dev/null 2>&1 &
    fi
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
    if [[ "$UBSIM_EXECUTION_MODE" == docker ]]; then
        local docker_args=(-d)
        local assignment
        for assignment in "${assignments[@]}"; do
            docker_args+=(-e "$assignment")
        done
        docker exec "${docker_args[@]}" "$UBSIM_CONTAINER" "$@"
    else
        nohup env "${assignments[@]}" "$@" </dev/null >/dev/null 2>&1 &
    fi
}
