# Building Netease Switch

The repository produces a standalone Nintendo Switch NRO, an Atmosphère
background service, and an Ultrahand/Tesla overlay. The NRO and overlay are
separate clients of the same background player.

## Reproducible Docker toolchain

Requirements: Linux, macOS or Windows with a working Docker installation,
Git, and approximately 4 GB of free space for the toolchain and build outputs.
On Windows, run the scripts in WSL2 with Docker integration enabled.

```sh
./scripts/docker-build.sh --image
./scripts/docker-build.sh --smoke
./scripts/docker-build.sh
```

The image is named `netease-switch/toolchain:local`. Its upstream
`devkitpro/devkita64` image is pinned by digest in
[`docker/Dockerfile`](../docker/Dockerfile). The image contains the complete
official Switch portlibs set; the build does not silently upgrade packages.
To use a separately maintained image:

```sh
NETEASE_SWITCH_IMAGE=my-toolchain:verified BUILD_JOBS=12 ./scripts/docker-build.sh
```

The container runs as your host UID/GID, so generated files remain editable
without `sudo`. Source code is mounted at `/workspace`; toolchains live under
`/opt/devkitpro`.

| Component | Verified version |
| --- | --- |
| devkitA64 | r29.2-1 |
| libnx | 4.12.0-1 |
| switch-cmake | 1.5.2-1 |
| switch-curl | 7.69.1-5 |
| switch-mbedtls | 2.28.10-1 |
| switch-zlib | 1.3.1-1 |
| switch-mesa | 20.1.0-5 |
| switch-glfw | 3.3.4-2 |

The complete package inventory and compiler versions are recorded on every
build in `build/validation/toolchain-packages.txt` and
`build/validation/compiler.txt`.

The API client uses mbedTLS for AES, RSA, X25519 and GCM. The official Switch
portlibs image does not provide an OpenSSL package. Do not link host
`libcrypto.so` into a Switch build.

## Outputs

The primary app outputs are:

```text
build/switch/NeteaseSwitch.nro
build/switch/NeteaseSwitch.elf
```

The smoke test writes `build/smoke/toolchain-smoke.nro` and its ELF. It verifies
the compiler, libnx, NACP metadata generation and NRO packaging independently
from application dependencies.

Always retain the ELF corresponding to the NRO or system module tested on
hardware. An ELF from a later build cannot reliably symbolicate an earlier
Atmosphère crash report.

Build logs are written under `build/validation/`. The first environment
verification on 2026-10-20 compiled an AArch64 NRO and ran it successfully in
Eden 0.2.1 on the isolated Xvfb display `:119`; see
[`EMULATOR.md`](EMULATOR.md) for the reproduction commands and limits.

## Direct container access

```sh
docker run --rm -it \
  --user "$(id -u):$(id -g)" \
  -v "$PWD:/workspace" -w /workspace \
  netease-switch/toolchain:local bash
```

Inside the container:

```sh
cmake -S . -B build/switch -G Ninja \
  -DPLATFORM_SWITCH=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/switch --target NeteaseSwitch.nro --parallel 8
```

The Borealis toolchain chooses the official devkitPro Switch CMake files.
Never reuse a desktop build directory for Switch compilation.

## Troubleshooting

**Docker permission denied:** configure your Docker installation so your user
can run `docker info`; the scripts do not change your group membership.

**A required package is missing:** check the pinned image and package list.
Changing the upstream digest is an explicit toolchain update and requires
repeating the smoke build and emulator test.

**CMake fails after a platform change:** use separate build directories
(`build/switch`, `build/desktop`, `build/host`) for different toolchains.

**Resources or Chinese text are absent:** check the packaged RomFS, font
resources and Borealis resource paths before attributing the issue to Eden.
Eden synthesizes some system font archives when no firmware is installed;
that does not reproduce every real Switch font.

**Background audio differs on hardware:** the overlay requires a compatible
overlay loader and the NSP requires Atmosphère. Their memory, IPC and audio
resource behavior must also be tested on a physical console.
