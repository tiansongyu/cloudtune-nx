#!/usr/bin/env bash
set -euo pipefail
mkdir -p build/validation
dkp-pacman -Q > build/validation/toolchain-packages.txt
aarch64-none-elf-g++ --version > build/validation/compiler.txt
cmake --version >> build/validation/compiler.txt
if [[ "${1:-}" == "--smoke" ]]; then
    mkdir -p build/smoke
    aarch64-none-elf-gcc -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft \
        -fPIE -ffunction-sections -fdata-sections -D__SWITCH__ \
        -I"$DEVKITPRO/libnx/include" docker/smoke/main.c \
        -L"$DEVKITPRO/libnx/lib" -specs="$DEVKITPRO/libnx/switch.specs" \
        -Wl,-Map,build/smoke/toolchain-smoke.map -lnx -o build/smoke/toolchain-smoke.elf
    nacptool --create "CloudTune NX Toolchain Test" "CloudTune NX contributors" "0.1.0" \
        build/smoke/toolchain-smoke.nacp
    elf2nro build/smoke/toolchain-smoke.elf build/smoke/toolchain-smoke.nro \
        --nacp=build/smoke/toolchain-smoke.nacp
    aarch64-none-elf-readelf -h build/smoke/toolchain-smoke.elf
    sha256sum build/smoke/toolchain-smoke.{elf,nro}
    exit 0
fi
cmake -S . -B build/switch -G Ninja -DPLATFORM_SWITCH=ON \
    -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build build/switch --target CloudTune.nro cloudtune-background-package \
    --parallel "${BUILD_JOBS:-8}"
# Background service and overlay are separate processes and separate binaries.
if [[ -f overlay/Makefile ]]; then
    make -C overlay -j"${BUILD_JOBS:-8}"
fi
