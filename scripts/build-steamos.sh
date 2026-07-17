#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-$repo_root/build-steamos}"

for command in git cmake ninja; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "Required command not found: $command" >&2
    exit 1
  fi
done

git -C "$repo_root" submodule update --init --recursive

cmake -S "$repo_root" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DLINUX_LOCAL_DEV=ON \
  -DENABLE_VR=ON \
  -DENABLE_VULKAN=ON \
  -DENABLE_AUTOUPDATE=OFF \
  -DENABLE_ANALYTICS=OFF \
  -DDISTRIBUTOR="PrimedSteam"

cmake --build "$build_dir" --target dolphin-emu --parallel

echo
echo "Built PrimedGun for SteamOS:"
echo "$build_dir/Binaries/PrimedGun"
