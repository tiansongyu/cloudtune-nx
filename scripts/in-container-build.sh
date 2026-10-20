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
export CLOUDTUNE_BUILD_STARTED_COMMIT
CLOUDTUNE_BUILD_STARTED_COMMIT="$(git rev-parse HEAD)"
export CLOUDTUNE_BUILD_STARTED_DIRTY=false
[[ -z "$(git status --porcelain)" ]] || CLOUDTUNE_BUILD_STARTED_DIRTY=true
./scripts/setup-airplay-codec.sh switch
cmake -S . -B build/switch -G Ninja -DPLATFORM_SWITCH=ON \
    -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build build/switch --target CloudTune.nro cloudtune-background-package \
    --parallel "${BUILD_JOBS:-8}"
# Background service and overlay are separate processes and separate binaries.
if [[ -f overlay/Makefile ]]; then
    make -C overlay OUTPUT_DIR="$(pwd)/build/overlay" -j"${BUILD_JOBS:-8}"
fi
python3 - <<'PY'
import importlib.util, json, os, pathlib, re, subprocess, sys
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("package_verifier", "scripts/verify-package.py")
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)
version = re.search(r"project\(CloudTuneNX VERSION ([0-9.]+)", pathlib.Path("CMakeLists.txt").read_text()).group(1)
_, components = verifier.validate_build("build/switch", "build/switch/background", "build/overlay", version)
commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
dirty = bool(subprocess.check_output(["git", "status", "--porcelain"], text=True).strip())
dirty = dirty or os.environ["CLOUDTUNE_BUILD_STARTED_DIRTY"] == "true"
dirty = dirty or commit != os.environ["CLOUDTUNE_BUILD_STARTED_COMMIT"]
provenance = {"version": version, "git_commit": commit,
              "source_has_uncommitted_changes": dirty, "components": components}
pathlib.Path("build/validation/build-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
print(f"Verified all three Switch components; recorded build provenance (dirty={dirty}).")
PY
