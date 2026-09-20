#!/usr/bin/env bash
# Build a self-contained ARM64 build/runtime container and reproduce the lab.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
image="${OPENURMA_IMAGE:-openurma-gem5-lab:ubuntu22.04-arm64}"
container="${OPENURMA_CONTAINER:-openurma-gem5-lab}"
kernel_volume="${OPENURMA_KERNEL_VOLUME:-openurma-gem5-lab-kernel}"
jobs="${JOBS:-2}"
sources_only=0

usage() {
    cat <<'EOF'
Usage: ./setup.sh [--sources-only] [--jobs N]

Build the ARM64 Ubuntu image, create the persistent container, fetch every
pinned source revision, and build gem5 + OLK + official UMDK + initramfs.
--sources-only stops after fetching and validating the source trees.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sources-only) sources_only=1; shift ;;
        --jobs) [[ $# -ge 2 ]] || { usage >&2; exit 2; }; jobs=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "setup.sh: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "setup.sh: --jobs must be positive" >&2; exit 2; }
command -v docker >/dev/null || { echo "setup.sh: Docker is required" >&2; exit 1; }
docker info >/dev/null

echo "[setup] building $image"
docker build --provenance=false --platform linux/arm64 -t "$image" \
    -f "$script_dir/docker/Dockerfile" "$script_dir"
docker volume create "$kernel_volume" >/dev/null

if docker container inspect "$container" >/dev/null 2>&1; then
    desired_image=$(docker image inspect -f '{{.Id}}' "$image")
    current_image=$(docker inspect -f '{{.Image}}' "$container")
    current_lab=$(docker inspect -f '{{range .Mounts}}{{if eq .Destination "/workspace/openurma-gem5-lab"}}{{.Source}}{{end}}{{end}}' "$container")
    if [[ "$current_image" != "$desired_image" || "$current_lab" != "$script_dir" ]]; then
        echo "[setup] replacing $container because its image or workspace mount changed"
        docker rm -f "$container" >/dev/null
    fi
fi
if ! docker container inspect "$container" >/dev/null 2>&1; then
    docker run -d --name "$container" --platform linux/arm64 \
        --label openurma.gem5.lab=managed \
        --mount "type=bind,src=$script_dir,dst=/workspace/openurma-gem5-lab" \
        --mount "type=volume,src=$kernel_volume,dst=/opt/openurma-gem5-lab" \
        "$image" >/dev/null
else
    running=$(docker inspect -f '{{.State.Running}}' "$container")
    [[ "$running" == true ]] || docker start "$container" >/dev/null
fi

echo "[setup] fetching pinned source trees"
docker exec "$container" bash /workspace/openurma-gem5-lab/scripts/fetch-sources.sh

if [[ "$sources_only" == 1 ]]; then
    echo "[setup] source preparation passed"
    exit 0
fi

echo "[setup] building the complete stack; the first build can take a long time"
docker exec -e JOBS="$jobs" "$container" \
    bash /workspace/openurma-gem5-lab/scripts/build-all.sh
echo "[setup] PASS"
echo "Start the official two-node stack with: ./run-dual.sh --profile fast --provider official"
