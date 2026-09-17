#!/usr/bin/env bash
set -euo pipefail

lab_root="$(cd "$(dirname "$0")/.." && pwd)"
kernel_root="${KSRC:-$lab_root/oe66}"
udma_root="$kernel_root/drivers/ub/urma/hw/udma"
output="${1:-$lab_root/official-udma/contract.tsv}"

[[ -d "$udma_root" ]] || {
    echo "UDMA source not found: $udma_root" >&2
    exit 1
}

tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT

# Extract only function-like references made by the unmodified official
# driver.  Type names and included header basenames must not inflate the
# hardware contract.
rg -o --no-filename \
    '\b(ubase|ummu|iommu_dev)_[a-zA-Z0-9_]+[[:space:]]*\(' \
    "$udma_root" -g '*.[ch]' |
    sed -E 's/[[:space:]]*\($//' | sort -u > "$tmp/external"

{
    printf 'service\tgroup\tsource_provider\n'
    while IFS= read -r service; do
        case "$service" in
            ubase_*ctrlq*|ubase_*cmd*|ubase_*mailbox*|ubase_*mbx*|ubase_*activate*|ubase_*deactivate*)
                group=control
                ;;
            ubase_*event*|ubase_*comp*|ubase_*crq*|ubase_*port*|ubase_*reset*)
                group=event
                ;;
            ubase_*)
                group=discovery
                ;;
            ummu_*|iommu_dev_*)
                group=translation
                ;;
            *)
                group=other
                ;;
        esac
        case "$service" in
            ubase_*) provider=UBASE ;;
            ummu_*) provider=UMMU ;;
            iommu_dev_*) provider=IOMMU ;;
            *) provider=kernel ;;
        esac
        printf '%s\t%s\t%s\n' "$service" "$group" "$provider"
    done < "$tmp/external"
} > "$output"

echo "Wrote $(($(wc -l < "$output") - 1)) external services to $output"
