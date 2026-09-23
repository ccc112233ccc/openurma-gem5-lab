#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab_dir="$(cd "$script_dir/.." && pwd)"
package_file="${OPENURMA_UBUNTU_PACKAGES:-$lab_dir/docker/ubuntu-22.04-packages.txt}"

die() { printf 'install-ubuntu-deps.sh: %s\n' "$*" >&2; exit 2; }

[[ "$(uname -s)" == Linux ]] || die "native setup requires Linux"
[[ -r /etc/os-release ]] || die "cannot identify the Linux distribution"
# shellcheck disable=SC1091
source /etc/os-release
[[ "${ID:-}" == ubuntu ]] || die "native setup currently supports Ubuntu, found ${ID:-unknown}"
[[ "${VERSION_ID:-}" == 22.04 ]] ||
    die "native setup is validated on Ubuntu 22.04, found ${VERSION_ID:-unknown}"
[[ "$(uname -m)" == aarch64 ]] ||
    die "native setup currently supports ARM64 Ubuntu; found $(uname -m)"
[[ -r "$package_file" ]] || die "package list not found: $package_file"

mapfile -t packages < <(sed '/^[[:space:]]*$/d' "$package_file")
(( ${#packages[@]} > 0 )) || die "package list is empty: $package_file"

if [[ "$(id -u)" -eq 0 ]]; then
    apt=(apt-get)
else
    command -v sudo >/dev/null || die "sudo is required to install packages"
    apt=(sudo apt-get)
fi

"${apt[@]}" update
"${apt[@]}" install -y --no-install-recommends "${packages[@]}"
printf '[native-setup] Ubuntu build dependencies are installed\n'
