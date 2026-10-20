#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
image="${CLOUDTUNE_IMAGE:-${NETEASE_SWITCH_IMAGE:-cloudtune-nx/toolchain:local}}"
jobs="${BUILD_JOBS:-8}"
mkdir -p "$repo_root/build/validation"
if [[ "${1:-}" == "--image" ]] || ! docker image inspect "$image" >/dev/null 2>&1; then
    docker build --progress=plain -f "$repo_root/docker/Dockerfile" -t "$image" "$repo_root/docker" \
        2>&1 | tee "$repo_root/build/validation/docker-image.log"
    [[ "${1:-}" != "--image" ]] || exit 0
fi
docker run --rm --user "$(id -u):$(id -g)" \
    -e BUILD_JOBS="$jobs" -v "$repo_root:/workspace" -w /workspace \
    "$image" bash scripts/in-container-build.sh "$@" \
    2>&1 | tee "$repo_root/build/validation/docker-build.log"
