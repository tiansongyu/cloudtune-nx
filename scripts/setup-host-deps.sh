#!/usr/bin/env bash
set -euo pipefail
# Build only the missing crypto dependency, without changing system packages.
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version="2.28.10"
deps_dir="$repo_root/.deps"
archive="$deps_dir/downloads/mbedtls-$version.tar.gz"
source_dir="$deps_dir/src/mbedtls-$version"
prefix="$deps_dir/mbedtls"
expected="0f2e0525903a89ae1d39ce439d858be66933bda54c5b6102b72a29ed8fe7c088"
mkdir -p "$deps_dir/downloads" "$deps_dir/src"
if [[ ! -f "$archive" ]]; then
    curl -fL --retry 3 \
        "https://codeload.github.com/Mbed-TLS/mbedtls/tar.gz/refs/tags/v$version" \
        -o "$archive"
fi
printf '%s  %s\n' "$expected" "$archive" | sha256sum --check
if [[ ! -f "$source_dir/CMakeLists.txt" ]]; then
    tar -xzf "$archive" -C "$deps_dir/src"
fi
cmake -S "$source_dir" -B "$deps_dir/build/mbedtls" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build "$deps_dir/build/mbedtls" --parallel "${BUILD_JOBS:-8}"
cmake --install "$deps_dir/build/mbedtls"
printf '\nHost crypto libraries installed in %s\n' "$prefix"
printf 'Configure with: -DCMAKE_PREFIX_PATH=%q\n' "$prefix"
