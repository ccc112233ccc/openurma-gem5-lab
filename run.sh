#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/runtime.sh
source "$script_dir/scripts/runtime.sh"

# Paths are inside the ARM64 Linux build container. They may be overridden for
# a different checkout while the documented lab layout remains the default.
container="$OPENURMA_CONTAINER"
lab="${OPENURMA_LAB_ROOT:-$(ou_runtime_default_lab "$script_dir")}"
gem5="${OPENURMA_GEM5:-$lab/gem5/build/ARM/gem5.opt}"
m5_path="${OPENURMA_M5_PATH:-$lab/system}"
kernel="${OPENURMA_KERNEL:-$lab/artifacts/kernel/vmlinux}"
initrd="${OPENURMA_INITRD:-$lab/out/openurma-interactive.cpio.gz}"
outdir="${OPENURMA_M5_OUT:-$lab/run}"
pipe_data="${OPENURMA_PIPE_DATA:-0}"
provider="${OPENURMA_PROVIDER:-legacy}"
dma_backend="${OPENURMA_DMA_BACKEND:-$provider}"

case "$provider" in
    legacy|udma) ;;
    *) echo "OPENURMA_PROVIDER must be legacy or udma" >&2; exit 2 ;;
esac
case "$dma_backend" in
    legacy|timing|udma) ;;
    *) echo "OPENURMA_DMA_BACKEND must be legacy, timing, or udma" >&2; exit 2 ;;
esac

ou_runtime_start

for path in "$gem5" "$kernel" "$initrd"; do
    if ! ou_exec test -f "$path"; then
        echo "missing in runtime environment: $path" >&2
        echo "The build is incomplete, or its location needs an OPENURMA_* override." >&2
        exit 2
    fi
done
for resource in boot.arm64 boot.arm; do
    if ! ou_exec test -f "$m5_path/binaries/$resource"; then
        echo "missing gem5 ARM system resource: $m5_path/binaries/$resource" >&2
        echo "Set OPENURMA_M5_PATH to the extracted aarch-system root." >&2
        exit 2
    fi
done

echo "Starting the OpenURMA full-system guest. In a second terminal run:"
echo "  ./attach.sh"
echo "The UART transcript is also written to $outdir/system.terminal."

gem5_args=(
    "$gem5"
    --listener-mode=on \
    --outdir="$outdir" \
    "$lab/configs/single_node_fs_openurma.py" \
    --kernel="$kernel" \
    --initrd="$initrd" \
    --root-device=/dev/ram \
    --mem-size=2GB \
    --cpu=atomic \
    --cpu-freq=3GHz \
    --mem-type=DDR3_1600_8x8 \
    --mem-channels=1 \
    --link-delay-ns=0 \
    --dma-backend="$dma_backend" \
    --extra-cmdline="openurma_provider=$provider"
)
if [[ "$OPENURMA_EXECUTION_MODE" == docker ]]; then
    exec docker exec -it \
        -e "M5_PATH=$m5_path" \
        -e "OPENURMA_PIPE_DATA=$pipe_data" \
        "$container" "${gem5_args[@]}"
else
    export M5_PATH="$m5_path" OPENURMA_PIPE_DATA="$pipe_data"
    exec "${gem5_args[@]}"
fi
