#!/usr/bin/env bash
# Fail before launching any process when the runtime cannot host ArmV8KvmCPU.
set -euo pipefail

die() { printf 'kvm-preflight: %s\n' "$*" >&2; exit 2; }

gem5_binary=${1:-}
[[ "$(uname -s)" == Linux ]] || die "KVM execution requires Linux"
[[ "$(uname -m)" == aarch64 ]] ||
    die "ArmV8KvmCPU requires an ARM64 host; found $(uname -m)"
[[ -c /dev/kvm ]] ||
    die "/dev/kvm is absent (for Docker recreate with ./lab --runtime docker setup --kvm)"
[[ -r /dev/kvm && -w /dev/kvm ]] ||
    die "/dev/kvm is not readable and writable by uid $(id -u)"
[[ -n "$gem5_binary" && -x "$gem5_binary" ]] ||
    die "gem5 executable is missing or not executable: ${gem5_binary:-<unset>}"

build_dir=$(cd "$(dirname "$gem5_binary")" && pwd)
use_kvm="$build_dir/config/use_kvm.hh"
kvm_isa="$build_dir/config/kvm_isa.hh"
grep -Eq '^#define[[:space:]]+USE_KVM[[:space:]]+1([[:space:]]|$)' "$use_kvm" 2>/dev/null ||
    die "gem5 was not built with USE_KVM=1: $use_kvm"
grep -Eq '^#define[[:space:]]+KVM_ISA[[:space:]]+"arm"([[:space:]]|$)' "$kvm_isa" 2>/dev/null ||
    die "gem5 KVM ISA is not arm: $kvm_isa"

printf '[kvm-preflight] PASS: ARM64 host, /dev/kvm access, and gem5 ARM KVM build verified\n'
