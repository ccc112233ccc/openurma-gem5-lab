#!/usr/bin/env bash
# Run on the same Linux builder as UMDK. No provider or driver source changes.
set -euo pipefail
root="${UBSIM_LAB_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}"
cc="${CROSS_COMPILE:-aarch64-linux-gnu-}gcc"
build="${UMDK_BUILD_DIR:-$root/artifacts/umdk-build}"
out="${UBSIM_INTEGRITY_OUTPUT:-$root/artifacts/urma-integrity}"
start=$SECONDS
"$cc" -std=c11 -O2 -g -Wall -Wextra -Werror \
  -I"$root/sources/umdk/src/urma/lib/urma/core/include" \
  "$root/tools/urma_integrity.c" \
  -L"$build/urma/lib/urma/core" -Wl,-rpath-link,"$build/urma/common" \
  -lurma -lpthread -o "$out"
echo "Built $out wall_seconds=$((SECONDS-start))"
