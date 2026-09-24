#!/usr/bin/env bash
# Docker wrapper around the same native Ubuntu setup used on bare metal.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
lab_root=$(cd "$script_dir/.." && pwd)
image="${OPENURMA_IMAGE:-openurma-gem5-lab:ubuntu22.04-arm64}"
container="${OPENURMA_CONTAINER:-openurma-gem5-lab}"
kernel_volume="${OPENURMA_KERNEL_VOLUME:-openurma-gem5-lab-kernel}"
jobs="${JOBS:-2}"
sources_only=0
enable_kvm=0

usage() {
    cat <<'EOF'
Usage: ./lab --runtime docker setup [--sources-only] [--kvm] [--jobs N]

Build the ARM64 Ubuntu image, create the persistent container, then invoke
setup-native.sh inside it. The native and Docker paths therefore share source,
build, and validation logic.
--sources-only stops after fetching and validating the source trees.
--kvm requires an ARM64 Linux host with /dev/kvm and passes that device into
the persistent container. It is not available through Docker Desktop on macOS.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sources-only) sources_only=1; shift ;;
        --kvm) enable_kvm=1; shift ;;
        --jobs) [[ $# -ge 2 ]] || { usage >&2; exit 2; }; jobs=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "lab setup: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "lab setup: --jobs must be positive" >&2; exit 2; }
command -v docker >/dev/null || { echo "lab setup: Docker is required" >&2; exit 1; }
docker info >/dev/null
if (( enable_kvm )); then
    [[ "$(uname -s)" == Linux && "$(uname -m)" == aarch64 ]] || {
        echo "lab setup: --kvm requires an ARM64 Linux Docker host" >&2
        exit 2
    }
    [[ -c /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || {
        echo "lab setup: --kvm requires readable/writable /dev/kvm" >&2
        exit 2
    }
fi

echo "[setup] building $image"
docker build --provenance=false --platform linux/arm64 -t "$image" \
    -f "$lab_root/docker/Dockerfile" "$lab_root"
docker volume create "$kernel_volume" >/dev/null

if docker container inspect "$container" >/dev/null 2>&1; then
    desired_image=$(docker image inspect -f '{{.Id}}' "$image")
    current_image=$(docker inspect -f '{{.Image}}' "$container")
    current_lab=$(docker inspect -f '{{range .Mounts}}{{if eq .Destination "/workspace/openurma-gem5-lab"}}{{.Source}}{{end}}{{end}}' "$container")
    current_kvm=$(docker inspect -f '{{range .HostConfig.Devices}}{{if eq .PathInContainer "/dev/kvm"}}yes{{end}}{{end}}' "$container")
    if [[ "$current_image" != "$desired_image" || "$current_lab" != "$lab_root" || \
          ( "$enable_kvm" == 1 && "$current_kvm" != yes ) ]]; then
        echo "[setup] replacing $container because its image, workspace mount, or KVM device contract changed"
        docker rm -f "$container" >/dev/null
    fi
fi
if ! docker container inspect "$container" >/dev/null 2>&1; then
    docker_kvm_args=()
    (( enable_kvm == 0 )) || docker_kvm_args+=(--device /dev/kvm:/dev/kvm)
    docker run -d --name "$container" --platform linux/arm64 \
        "${docker_kvm_args[@]}" \
        --label openurma.gem5.lab=managed \
        --mount "type=bind,src=$lab_root,dst=/workspace/openurma-gem5-lab" \
        --mount "type=volume,src=$kernel_volume,dst=/opt/openurma-gem5-lab" \
        "$image" >/dev/null
else
    running=$(docker inspect -f '{{.State.Running}}' "$container")
    [[ "$running" == true ]] || docker start "$container" >/dev/null
fi

native_args=(--skip-deps --jobs "$jobs")
(( sources_only == 0 )) || native_args+=(--sources-only)
echo "[setup] invoking the shared native Ubuntu setup inside $container"
docker exec \
    -e OPENURMA_EXECUTION_MODE=native \
    -e OPENURMA_LAB_ROOT=/workspace/openurma-gem5-lab \
    -e KSRC=/opt/openurma-gem5-lab/oe66 \
    -e JOBS="$jobs" \
    "$container" bash /workspace/openurma-gem5-lab/scripts/setup-native.sh \
    "${native_args[@]}"
echo "[setup] PASS"
echo "Start the official two-node stack with: ./lab start --profile fast --provider official"
