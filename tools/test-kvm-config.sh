#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
lab=$(cd "$script_dir/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/openurma-kvm-config.XXXXXX")
trap 'rm -rf -- "$tmp"' EXIT

OPENURMA_EXECUTION_MODE=native "$lab/run-dual.sh" --profile kvm --print-config >"$tmp/kvm"
grep -qx 'profile=kvm' "$tmp/kvm"
grep -qx 'provider=udma' "$tmp/kvm"
grep -qx 'cpu_mode=kvm' "$tmp/kvm"
grep -qx 'cpu_boot_model=ArmV8KvmCPU' "$tmp/kvm"
grep -qx 'initial_memory_mode=atomic_noncaching' "$tmp/kvm"
grep -qx 'm5ops_mode=addr' "$tmp/kvm"
grep -qx 'cpu_count=1' "$tmp/kvm"
grep -qx 'udma_poll_interval=1ms' "$tmp/kvm"
grep -qx 'kvm_host_cpu_contract=portable_udma_provider' "$tmp/kvm"
grep -qx 'sync_request=auto' "$tmp/kvm"
grep -qx 'virtual_time_synchronization=disabled' "$tmp/kvm"
grep -qx 'sync_mode=adapter-local' "$tmp/kvm"

OPENURMA_EXECUTION_MODE=native "$lab/run-dual.sh" --profile kvm --sync \
    --print-config >"$tmp/kvm-sync"
grep -qx 'sync_request=on' "$tmp/kvm-sync"
grep -qx 'virtual_time_synchronization=enabled' "$tmp/kvm-sync"

OPENURMA_EXECUTION_MODE=native "$lab/run-dual.sh" --profile fast \
    --print-config >"$tmp/fast"
grep -qx 'udma_poll_interval=10ns' "$tmp/fast"
grep -qx 'virtual_time_synchronization=enabled' "$tmp/fast"

OPENURMA_EXECUTION_MODE=native "$lab/run-dual.sh" --profile fast --no-sync \
    --print-config >"$tmp/fast-unsync"
grep -qx 'sync_request=off' "$tmp/fast-unsync"
grep -qx 'virtual_time_synchronization=disabled' "$tmp/fast-unsync"

OPENURMA_EXECUTION_MODE=native "$lab/run-dual.sh" --profile fast \
    --cpu-mode kvm --print-config >"$tmp/kvm-override"
grep -qx 'udma_poll_interval=1ms' "$tmp/kvm-override"

OPENURMA_EXECUTION_MODE=native OPENURMA_UDMA_POLL_INTERVAL=25us \
    "$lab/run-dual.sh" --profile kvm --print-config >"$tmp/kvm-explicit"
grep -qx 'udma_poll_interval=25us' "$tmp/kvm-explicit"

if OPENURMA_EXECUTION_MODE=native "$lab/run-dual.sh" --profile kvm \
        --num-cpus 2 --print-config >"$tmp/invalid" 2>&1; then
    echo "test-kvm-config.sh: multi-vCPU KVM configuration was accepted" >&2
    exit 1
fi
grep -q 'KVM modes currently require --num-cpus=1' "$tmp/invalid"

OPENURMA_EXECUTION_MODE=native "$lab/run-dual.sh" --profile kvm \
    --provider official --print-config >"$tmp/official" 2>"$tmp/official.err"
grep -qx 'kvm_host_cpu_contract=official_provider_host_dependent_ksva' "$tmp/official"
grep -q 'official provider under KVM sees host ARM CPU address-width/ASID capabilities' "$tmp/official.err"

python3 - <<'PY' "$lab/configs/single_node_fs_openurma.py"
import ast
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text()
tree = ast.parse(source)
assignments = {
    node.targets[0].id: ast.literal_eval(node.value)
    for node in tree.body
    if isinstance(node, ast.Assign)
    and len(node.targets) == 1
    and isinstance(node.targets[0], ast.Name)
    and node.targets[0].id == "KVM_CPU_MODES"
}
assert assignments["KVM_CPU_MODES"] == ("kvm", "kvm_server_o3")
assert "if not kvm_boot:" in source
assert "DT advertises platform MMIO timer (KVM boot)" in source
PY

echo "KVM configuration contract tests passed"
