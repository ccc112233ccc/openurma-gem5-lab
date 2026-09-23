#!/usr/bin/env bash
# Shared command backend for native Linux and Docker-hosted execution.

OPENURMA_EXECUTION_MODE="${OPENURMA_EXECUTION_MODE:-docker}"
OPENURMA_CONTAINER="${OPENURMA_CONTAINER:-openurma-gem5-lab}"

ou_runtime_validate() {
    case "$OPENURMA_EXECUTION_MODE" in
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
            echo "OPENURMA_EXECUTION_MODE must be docker or native" >&2
            return 2
            ;;
    esac
}

ou_runtime_default_lab() {
    local host_lab=$1
    if [[ "$OPENURMA_EXECUTION_MODE" == docker ]]; then
        printf '%s\n' /workspace/openurma-gem5-lab
    else
        printf '%s\n' "$host_lab"
    fi
}

ou_runtime_start() {
    ou_runtime_validate
    if [[ "$OPENURMA_EXECUTION_MODE" == docker ]]; then
        docker start "$OPENURMA_CONTAINER" >/dev/null
    fi
}

ou_exec() {
    if [[ "$OPENURMA_EXECUTION_MODE" == docker ]]; then
        docker exec "$OPENURMA_CONTAINER" "$@"
    else
        "$@"
    fi
}

ou_exec_i() {
    if [[ "$OPENURMA_EXECUTION_MODE" == docker ]]; then
        docker exec -i "$OPENURMA_CONTAINER" "$@"
    else
        "$@"
    fi
}

ou_exec_detached() {
    if [[ "$OPENURMA_EXECUTION_MODE" == docker ]]; then
        docker exec -d "$OPENURMA_CONTAINER" "$@"
    else
        nohup "$@" </dev/null >/dev/null 2>&1 &
    fi
}

ou_exec_detached_env() {
    local assignments=()
    while (( $# )) && [[ "$1" != -- ]]; do
        assignments+=("$1")
        shift
    done
    [[ "${1:-}" == -- ]] || {
        echo "ou_exec_detached_env: missing -- separator" >&2
        return 2
    }
    shift
    if [[ "$OPENURMA_EXECUTION_MODE" == docker ]]; then
        local docker_args=(-d)
        local assignment
        for assignment in "${assignments[@]}"; do
            docker_args+=(-e "$assignment")
        done
        docker exec "${docker_args[@]}" "$OPENURMA_CONTAINER" "$@"
    else
        nohup env "${assignments[@]}" "$@" </dev/null >/dev/null 2>&1 &
    fi
}
