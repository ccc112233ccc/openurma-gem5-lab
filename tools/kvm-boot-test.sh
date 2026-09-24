#!/usr/bin/env bash
# Launch one guest directly on an ARM64 Linux host for KVM bring-up diagnosis.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
lab=$(cd "$script_dir/.." && pwd)
cpu_mode=${1:-kvm}
run_dir=${2:-$lab/run-kvm-$cpu_mode}
provider=${OPENURMA_PROVIDER:-udma}
terminal_port=${OPENURMA_TERMINAL_PORT:-3456}
gem5=${OPENURMA_GEM5:-$lab/gem5/build/ARM/gem5.opt}
kernel=${OPENURMA_KERNEL:-$lab/artifacts/kernel/vmlinux}
config=${OPENURMA_CONFIG:-$lab/configs/single_node_fs_openurma.py}
m5_path=${OPENURMA_M5_PATH:-$lab/system}

case "$cpu_mode" in
    kvm|kvm_server_o3|atomic_fast) ;;
    *) echo "kvm-boot-test.sh: expected kvm, kvm_server_o3, or atomic_fast" >&2; exit 2 ;;
esac
case "$provider" in
    legacy|udma|official) ;;
    *) echo "kvm-boot-test.sh: OPENURMA_PROVIDER must be legacy, udma, or official" >&2; exit 2 ;;
esac
[[ ! -e "$run_dir" ]] || {
    echo "kvm-boot-test.sh: output already exists: $run_dir" >&2
    exit 2
}
[[ -x "$gem5" && -f "$kernel" && -f "$config" ]] || {
    echo "kvm-boot-test.sh: build artifacts are incomplete; run './lab --runtime native setup' first" >&2
    exit 2
}
if [[ "$cpu_mode" == kvm* ]]; then
    "$script_dir/kvm-preflight.sh" "$gem5"
fi

official_args=()
case "$provider" in
    official)
        initrd=${OPENURMA_INITRD:-$lab/out/official-udma.cpio.gz}
        dma_backend=udma
        official_args=(--official-udma-discovery --udma-endpoint-eid=0x100)
        echo "[kvm-boot-test] warning: official provider registration under KVM depends on host ARM CPU address-width/ASID capabilities; a host exceeding the modeled 40-bit UMMU OAS clears UMMU_FEAT_SVA and makes KSVA fail. Use OPENURMA_PROVIDER=udma for the portable control" >&2
        ;;
    udma)
        initrd=${OPENURMA_INITRD:-$lab/out/openurma-interactive.cpio.gz}
        dma_backend=udma
        ;;
    legacy)
        initrd=${OPENURMA_INITRD:-$lab/out/openurma-interactive.cpio.gz}
        dma_backend=legacy
        ;;
esac
[[ -f "$initrd" ]] || { echo "kvm-boot-test.sh: initramfs missing: $initrd" >&2; exit 2; }

mkdir -p "$run_dir"
printf '[kvm-boot-test] cpu=%s provider=%s terminal=localhost:%s output=%s\n' \
    "$cpu_mode" "$provider" "$terminal_port" "$run_dir"
export M5_PATH="$m5_path"
exec "$gem5" --listener-mode=on --outdir="$run_dir" "$config" \
    "${official_args[@]}" \
    --kernel="$kernel" --initrd="$initrd" --root-device=/dev/ram \
    --cpu="$cpu_mode" --num-cpus=1 --benchmark-cpu=0 \
    --m5ops-base=0x10010000 --dma-backend="$dma_backend" \
    --udma-poll-interval="${OPENURMA_UDMA_POLL_INTERVAL:-1ms}" \
    --terminal-port="$terminal_port" \
    --extra-cmdline="openurma_node=0 openurma_provider=$provider"
